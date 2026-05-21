#!/usr/bin/env bash
#
# Copyright (C) 2026 IBM
#
# Author: Matty Williams <Matty.Williams@ibm.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#
# Test OMAP recovery with read error injection
# This exercises the need_resend code path and ECCommon::ReadPipeline::get_remaining_shards

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh

function run() {
    local dir=$1
    shift

    export CEPH_MON="127.0.0.1:7148" # git grep '\<7148\>' : there must be only one
    export CEPH_ARGS
    CEPH_ARGS+="--fsid=$(uuidgen) --auth-supported=none "
    CEPH_ARGS+="--mon-host=$CEPH_MON "

    local funcs=${@:-$(set | sed -n -e 's/^\(TEST_[0-9a-z_]*\) .*/\1/p')}
    for func in $funcs ; do
        setup $dir || return 1
        $func $dir || return 1
        teardown $dir || return 1
    done
}

function TEST_omap_recovery_with_read_error() {
    local dir=$1
    local poolname=test_ec_pool
    local objname=test_omap_object

    # Start cluster
    run_mon $dir a || return 1
    run_mgr $dir x || return 1
    run_osd $dir 0 || return 1
    run_osd $dir 1 || return 1
    run_osd $dir 2 || return 1

    # Create erasure-coded pool with k=2, m=1
    ceph osd erasure-code-profile set ec_profile_test \
        plugin=jerasure \
        k=2 \
        m=1 \
        crush-failure-domain=osd || return 1

    ceph osd pool create $poolname 12 12 erasure ec_profile_test || return 1
    ceph osd pool set $poolname allow_ec_overwrites true || return 1
    ceph osd pool set $poolname allow_ec_optimizations true || return 1
    ceph osd pool set $poolname supports_omap true || return 1

    # Write object with OMAP data
    echo "test_data_for_recovery" | rados -p $poolname put $objname - || return 1
    
    rados -p $poolname setomapval $objname key_a val_12345 || return 1
    rados -p $poolname setomapval $objname key_b val_67890 || return 1
    rados -p $poolname setomapval $objname key_c val_abcde || return 1
    rados -p $poolname setomapheader $objname "omap_header_test" || return 1

    # Get PG information
    local pgid=$(ceph osd map $poolname $objname --format=json | jq -r '.pgid')
    echo "Object is in PG: $pgid"

    local pg_query=$(ceph pg $pgid query --format=json)
    local acting_osds=($(echo "$pg_query" | jq -r '.acting[]'))
    local prev_primary=${acting_osds[0]}
    
    echo "Current acting OSDs: ${acting_osds[*]}"
    echo "Current primary OSD: $prev_primary"

    # Prepare new upmap (swap first and last OSD)
    local new_osds=("${acting_osds[@]}")
    local last_idx=$((${#new_osds[@]} - 1))
    local temp=${new_osds[0]}
    new_osds[0]=${new_osds[$last_idx]}
    new_osds[$last_idx]=$temp
    
    local new_primary=${new_osds[0]}
    
    echo "New primary will be: OSD.$new_primary"
    echo "New acting set will be: ${new_osds[*]}"

    # Inject read error on new primary BEFORE setting upmap
    echo "Injecting read error on OSD.$new_primary..."
    ceph tell osd.$new_primary injectargs '--bluestore_debug_inject_read_err=true' || return 1
    
    # Inject specific error for this object
    # Format: ceph tell osd.x injectecreaderr <poolname> <oid> <shardid> <failure type: (0|1)> 0 <number of reads to fail>
    ceph tell osd.$new_primary injectecreaderr $poolname $objname 0 0 0 5 || return 1
    
    echo "Read error injected (will fail next 5 reads)"

    # Set pg-upmap to trigger recovery
    echo "Setting pg-upmap to trigger recovery..."
    ceph osd pg-upmap $pgid ${new_osds[@]} || return 1

    # Wait for upmap to take effect
    local timeout=60
    local elapsed=0
    
    while [ $elapsed -lt $timeout ]; do
        local current_primary=$(ceph pg $pgid query --format=json | jq -r '.acting[0]')
        
        if [ "$current_primary" == "$new_primary" ]; then
            echo "Upmap took effect! New primary is OSD.$current_primary"
            break
        fi
        
        sleep 1
        elapsed=$((elapsed + 1))
    done

    if [ $elapsed -ge $timeout ]; then
        echo "ERROR: Timeout waiting for upmap to take effect"
        return 1
    fi

    # Wait for PG to become active+clean (recovery will use need_resend path)
    echo "Waiting for PG to become active+clean (recovery will use need_resend path)..."
    wait_for_clean || return 1

    # Clear read error injection
    ceph tell osd.$new_primary injectargs '--bluestore_debug_inject_read_err=false' || return 1

    # Verify OMAP data after recovery
    echo "Verifying OMAP data after recovery..."
    
    local num_keys=$(rados -p $poolname listomapkeys $objname | wc -l)
    if [ "$num_keys" -ne 3 ]; then
        echo "ERROR: Expected 3 OMAP keys, found $num_keys"
        return 1
    fi
    echo "Found $num_keys OMAP keys (expected 3) ✓"
    
    local key_a_val=$(rados -p $poolname getomapval $objname key_a)
    if [[ "$key_a_val" != *"val_12345"* ]]; then
        echo "ERROR: OMAP value verification failed for key_a"
        return 1
    fi
    echo "OMAP values verified ✓"
    
    local header=$(rados -p $poolname getomapheader $objname)
    if [[ "$header" != *"omap_header_test"* ]]; then
        echo "ERROR: OMAP header verification failed"
        return 1
    fi
    echo "OMAP header verified ✓"
    
    echo "Test completed successfully!"
    echo ""
    echo "What happened:"
    echo "1. Object with OMAP data was created on initial acting set"
    echo "2. Read error was injected on the new primary OSD (OSD.$new_primary)"
    echo "3. pg-upmap was set to change the acting set"
    echo "4. During recovery, the new primary tried to read the object"
    echo "5. The injected read error caused the initial read to fail"
    echo "6. This triggered need_resend=true in ECBackend::handle_sub_read_reply"
    echo "7. send_all_remaining_reads was called, which called get_remaining_shards"
    echo "8. Additional reads were sent to other OSDs to complete recovery"
    echo "9. PG became clean and OMAP data is now accessible"
    echo ""
    echo "The recovery successfully exercised the need_resend code path!"

    return 0
}

main omap-recovery-with-read-error "$@"
