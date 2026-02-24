=================================================
Support for RBD and CephFS in Erasure Coded pools
=================================================

Introduction
============

This document covers the design for enabling Omap (object map) support
and synchronous read operations in erasure-coded pools.
These enhancements enable EC pools to support CLS methods, as well as
RBD and CephFS workloads without the need for a separate replica pool
for metadata.

Current Limitations
-------------------

Erasure-coded pools have historically been limited in their support for
metadata operations. Specifically:

- **Omap operations** (key-value metadata storage on objects) were not
  supported, limiting the use of EC pools for workloads requiring metadata.
- **Cls operations** (server-side object class methods) were not available,
  preventing RBD and other advanced features from working with EC pools.
- **Synchronous read operations** were not implemented in the EC backend,
  which are required for Cls operations to function correctly.

These limitations prevented EC pools from being used for many important
workloads, particularly RBD (RADOS Block Device) which relies heavily on both
omap and Cls operations.

Feature Relationships
---------------------

The two main features in this design are independent but complementary:

- **Omap Support**: Enables key-value metadata storage on EC pool objects
  through replication across primary-capable shards with journal-based updates
  managed by the primary OSD.
- **Synchronous Reads**: Provides synchronous read semantics in the EC backend
  using Boost pull-type coroutines, enabling synchronous operations without
  blocking threads.

Together, these features enable full RBD support and other advanced workloads
on erasure-coded pools.

Benefits and Use Cases
----------------------

Enabling these features provides several key benefits:

- **RBD on EC Pools**: Full support for RADOS Block Device on erasure-coded
  pools, enabling cost-effective block storage with erasure coding efficiency.
- **CephFS on EC Pools**: TODO
- **Metadata Operations**: Applications can store and query metadata alongside
  object data in EC pools.
- **Server-Side Processing**: Cls operations enable efficient server-side data
  processing without client round-trips.
- **Storage Efficiency**: Combines the space efficiency of erasure coding with
  the functionality previously only available in replicated pools.


Omap Support for EC Pools
==========================

Current Limitations
-------------------

In the original EC pool implementation, omap operations were not supported due
to the complexity of maintaining consistent key-value metadata across erasure-
coded shards. Unlike replicated pools where each replica maintains a complete
copy of the omap data, EC pools distribute data across multiple shards, making
metadata management more complex.

The primary challenges include:

- Ensuring consistency of metadata across shards
- Handling partial updates and failures
- Maintaining performance for metadata operations
- Supporting recovery and reconstruction scenarios

Design Approach
---------------

The omap implementation for EC pools uses a replication-based approach:

- Omap data is **replicated** across all primary-capable shards in a PG
- A **journal** is used to store omap updates before they are committed
- Updates are applied atomically across all primary-capable shards
- Consistency is maintained through the journal commit protocol

This approach provides:

- Strong consistency guarantees for metadata operations
- Efficient recovery through journal replay
- Compatibility with existing omap APIs
- Minimal impact on data path performance

Omap Architecture
-----------------

Shard Distribution and Primary-Capable Shards
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In an erasure-coded pool with k data shards and m parity shards, the
primary-capable shards are:

- The first data shard
- All m parity shards

This means there are **m + 1 primary-capable shards** in total. For example,
in a k=4, m=2 configuration, shards 0, 4, and 5 are primary-capable.

.. ditaa::

   EC Pool (k=4, m=2)

   +--------+  +--------+  +--------+  +--------+  +--------+  +--------+
   | Shard  |  | Shard  |  | Shard  |  | Shard  |  | Shard  |  | Shard  |
   |   0    |  |   1    |  |   2    |  |   3    |  |   4    |  |   5    |
   | (Data) |  | (Data) |  | (Data) |  | (Data) |  | (Parity|  | (Parity|
   |PRIMARY |  |        |  |        |  |        |  |PRIMARY |  |PRIMARY |
   |CAPABLE |  |        |  |        |  |        |  |CAPABLE |  |CAPABLE |
   +--------+  +--------+  +--------+  +--------+  +--------+  +--------+
       |                                               |           |
       v                                               v           v
   +--------+                                      +--------+  +--------+
   |  Omap  |                                      |  Omap  |  |  Omap  |
   | Replica|                                      | Replica|  | Replica|
   +--------+                                      +--------+  +--------+

   Primary-capable shards (0, 4, 5) maintain omap replicas

Each primary-capable shard maintains a complete copy of the omap data for
objects in the PG. This replication ensures that omap data remains available
even if some shards fail, and allows any primary-capable shard to serve omap
read requests when acting as primary.

Journal Implementation
~~~~~~~~~~~~~~~~~~~~~~

The ECOmapJournal is maintained **only on the primary OSD**. The journal:

- Records all omap update operations before they are applied
- Ensures atomic application of updates across all primary-capable replicas
- Enables recovery in case of failures during update operations
- Provides a consistent view of omap state during recovery

Journal entries contain:

- Operation type (set, remove, clear)
- Key-value pairs for the operation
- Transaction identifier
- Version information

The journal is stored locally on the primary OSD, simplifying the
implementation while still providing the necessary durability guarantees
through the replication of omap data itself to primary-capable shards.

Commit Protocol
~~~~~~~~~~~~~~~

The omap commit protocol ensures that updates are applied consistently across
all primary-capable shards. Since the journal is only on the primary, the
protocol is streamlined:

.. ditaa::

   Client                Primary           Primary-Capable      Primary-Capable
                                              Shard 1              Shard 2
      |                     |                     |                     |
      | Omap Update         |                     |                     |
      |-------------------->|                     |                     |
      |                     |                     |                     |
      |                     | Write to Journal    |                     |
      |                     |------+              |                     |
      |                     |      |              |                     |
      |                     |<-----+              |                     |
      |                     |                     |                     |
      |                     | Apply Update        |                     |
      |                     |-------------------->|                     |
      |                     |                     |                     |
      |                     | Apply Update        |                     |
      |                     |------------------------------------------>|
      |                     |                     |                     |
      |                     |<--------------------|                     |
      |                     |                     | Apply ACK           |
      |                     |                     |                     |
      |                     |<------------------------------------------|
      |                     |                     |                     | Apply ACK
      |                     |                     |                     |
      | Update Complete     |                     |                     |
      |<--------------------|                     |                     |

The protocol follows these steps:

#. Client submits omap update to primary OSD
#. Primary writes the operation to its local journal
#. Primary sends the omap update to all primary-capable shards
#. Each primary-capable shard applies the update and acknowledges
#. Primary responds to client with completion

This approach ensures that updates are durable (through replication to
primary-capable shards) and consistent across all replicas.

Recovery and Consistency
~~~~~~~~~~~~~~~~~~~~~~~~~

Omap recovery is integrated into the existing EC recovery loop. When a
primary-capable shard recovers:

- The recovering shard receives omap data from the current primary or another
  primary-capable shard
- Omap data is transferred as part of the normal EC recovery process
- The journal on the primary ensures that any in-flight operations are
  properly handled during recovery

During primary failover:

- The new primary (which must be a primary-capable shard) already has a
  complete copy of the omap data
- A new journal is initialized on the new primary
- Operations can continue without data loss

This integration with existing recovery mechanisms simplifies the
implementation and ensures consistency with EC pool recovery behavior.

Omap Operations
---------------

Supported Operations
~~~~~~~~~~~~~~~~~~~~

The following omap operations are supported in EC pools:

- ``omap_get_keys``: Retrieve all keys in the omap
- ``omap_get_vals``: Retrieve all key-value pairs in the omap
- ``omap_get_vals_by_keys``: Retrieve specific key-value pairs
- ``omap_set``: Set one or more key-value pairs
- ``omap_rm_keys``: Remove one or more keys
- ``omap_clear``: Remove all key-value pairs
- ``omap_get_header``: Retrieve the omap header
- ``omap_set_header``: Set the omap header

These operations provide the same semantics as in replicated pools, ensuring
compatibility with existing applications.

Operation Flow
~~~~~~~~~~~~~~

Read operations follow a simple flow:

.. ditaa::

   Client                Primary
      |                     |
      | Omap Read           |
      |-------------------->|
      |                     |
      |                     | Read Local Omap
      |                     |------+
      |                     |      |
      |                     |<-----+
      |                     |
      |                     | Apply Journal Updates
      |                     |------+
      |                     |      |
      |                     |<-----+
      |                     |
      | Return Data         |
      |<--------------------|

Read operations are served from the primary OSD by:

#. Reading the stored omap data from the local replica
#. Applying any pending updates from the ECOmapJournal on top of the stored
   omap
#. Returning the combined result to the client

This approach ensures that reads always see the most recent committed state,
including any updates that are in the journal but not yet fully persisted to
all replicas. The journal updates are applied in-memory during the read
operation, providing low-latency access to metadata while maintaining
consistency.

Write operations follow the commit protocol described earlier, ensuring
consistency across all replicas.


Synchronous Reads
=================

Motivation
----------

Synchronous read operations are required to support Cls operations in EC pools.
Cls methods must execute synchronously, meaning they must complete a read
operation and receive the data before proceeding with their logic. The
traditional asynchronous read path in the EC backend does not provide this
capability.

Additionally, synchronous reads are beneficial for:

- Simplifying certain code paths that require sequential operations
- Enabling synchronous semantics without blocking threads
- Supporting future features that require synchronous data access

The key challenge is implementing synchronous semantics without blocking
threads, which would harm performance and scalability.

Implementation Design
---------------------

The synchronous read implementation uses Boost pull-type coroutines to provide
synchronous semantics without blocking threads:

- A Boost coroutine is created for the synchronous read operation
- The coroutine initiates an asynchronous read and yields control
- When the asynchronous read completes, the coroutine is resumed
- The coroutine returns the read data to the caller

This approach provides the synchronous semantics required by Cls operations
while maintaining the performance benefits of asynchronous I/O and avoiding
thread blocking.

Boost Pull-Type Coroutines
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The implementation uses Boost.Coroutine2 pull-type coroutines to bridge
asynchronous and synchronous code. The coroutine:

- Yields control back to the caller when waiting for I/O
- Allows the thread to process other work while I/O is in progress
- Resumes execution when the asynchronous operation completes
- Provides synchronous semantics to the Cls operation

This mechanism allows Cls operations to be written in a straightforward,
synchronous style while the underlying I/O remains asynchronous and
non-blocking.

Integration with EC Backend
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The synchronous read path integrates with the existing EC backend:

- The ECSwitch routes synchronous reads to the EC Backend
- The handler uses the existing asynchronous read infrastructure
- Results are returned synchronously to the caller using the coroutine

This integration minimizes code duplication and leverages existing, well-tested
read logic.

Performance Impact
------------------

Latency Considerations
~~~~~~~~~~~~~~~~~~~~~~

Synchronous reads introduce some latency overhead compared to pure asynchronous
operations:

- **Coroutine Overhead**: Creating and resuming coroutines has a small CPU cost
- **Context Switching**: Yielding and resuming coroutines involves context
  switching overhead

However, these overheads are minimal compared to the I/O latency itself. In
practice, the latency impact is negligible for most workloads.

Throughput Implications
~~~~~~~~~~~~~~~~~~~~~~~~

The throughput impact of synchronous reads depends on the workload:

- **Cls-Heavy Workloads**: Workloads with many Cls operations may see some
  impact, but the coroutine approach minimizes this
- **Mixed Workloads**: Workloads with a mix of synchronous and asynchronous
  operations will see minimal impact
- **Pure Data Workloads**: Workloads without Cls operations will see very
  minimal impact

Performance testing will quantify these impacts and guide optimisation efforts.

Comparison with Async Operations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Synchronous reads are not intended to replace asynchronous operations. They
serve a specific purpose for Cls operations and other use cases requiring
synchronous semantics. The EC backend continues to use asynchronous reads for
all other operations, maintaining optimal performance for the common case.


CLS, RBD, and CephFS Support
=============================

Cls (Class) Support
-------------------

Background
~~~~~~~~~~

Cls (class) operations are server-side methods that execute on OSDs, enabling
efficient data processing without client round-trips. Examples include:

- **RBD Operations**: Image metadata management, snapshot operations
- **RGW Operations**: Bucket index operations, object tagging
- **Custom Operations**: User-defined server-side processing

Cls operations are inherently synchronous - they must read data, process it,
and potentially write results, all within a single operation context. This
synchronous nature is why they require synchronous read support in the EC
backend.

Why Cls Requires Synchronous Reads
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cls methods follow a pattern like:

#. Read object data or metadata
#. Process the data (e.g., update counters, check conditions)
#. Write updated data or metadata
#. Return results to client

This pattern requires that the read operation complete and return data before
the processing logic can execute. Asynchronous reads, which return immediately
and deliver data via callbacks, don't fit this model.

Current Limitations in EC Pools
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Without synchronous read support, Cls operations could not be implemented in
EC pools. This prevented:

- RBD from using EC pools for image storage
- RGW from using EC pools for certain bucket operations
- Custom Cls methods from working with EC pools

Enabling Cls in EC Pools
~~~~~~~~~~~~~~~~~~~~~~~~~

Technical Requirements
^^^^^^^^^^^^^^^^^^^^^^

Enabling Cls in EC pools requires:

#. **Synchronous Read Support**: Implemented via coroutines as described above
#. **Omap Support**: Many Cls operations require omap for metadata storage

All of these requirements are met by the features described in this document.

Integration with Synchronous Reads
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Cls operations use synchronous reads through a straightforward integration:

.. code-block:: cpp

   // Simplified Cls operation using synchronous reads
   int cls_method(cls_method_context_t hctx)
   {
       bufferlist bl;

       // Synchronous read - blocks until data is available
       int r = cls_cxx_read(hctx, 0, 1024, &bl);
       if (r < 0)
           return r;

       // Process data
       process_data(bl);

       // Write results
       return cls_cxx_write(hctx, 0, bl);
   }

The ``cls_cxx_read`` function internally uses the synchronous read path,
suspending the coroutine until data is available.

Integration with Omap Support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Many Cls operations require omap access for metadata:

.. code-block:: cpp

   // Cls operation using omap
   int cls_method_with_omap(cls_method_context_t hctx)
   {
       map<string, bufferlist> vals;

       // Read omap values
       int r = cls_cxx_map_get_vals(hctx, "", "", 100, &vals);
       if (r < 0)
           return r;

       // Process metadata
       process_metadata(vals);

       // Update omap
       return cls_cxx_map_set_vals(hctx, &vals);
   }

The omap operations work seamlessly with Cls, providing the metadata storage
required for complex operations.

RBD Support
-----------

RBD-Specific Considerations
~~~~~~~~~~~~~~~~~~~~~~~~~~~

RBD (RADOS Block Device) is a primary beneficiary of Cls support in EC pools.
RBD uses Cls operations extensively for:

- Image metadata management
- Snapshot operations
- Clone operations
- Exclusive lock management

With Cls support, RBD can now use EC pools for image storage, providing
significant storage efficiency improvements for block storage workloads.

CephFS Support
--------------

TODO


EC Backend Changes
==================

Backend Modifications
---------------------

The ECBackend and related classes have been enhanced to support the new
functionality. The implementation integrates omap support and synchronous reads
into the existing EC backend architecture.

Key changes include:

- Integration of omap operations into the EC backend
- Support for synchronous read operations using Boost coroutines
- Recovery of omap data integrated into the existing EC recovery loop

These changes are designed to minimize impact on existing code paths while
providing the new functionality.

Primary-Capable Shards
----------------------

Role in Omap Replication
~~~~~~~~~~~~~~~~~~~~~~~~~

In an erasure-coded pool with k data shards and m parity shards, the
primary-capable shards are:

- The first data shard
- All m parity shards

This gives a total of **m + 1 primary-capable shards**. These shards are
responsible for:

- Maintaining complete omap replicas
- Serving omap read requests when acting as primary
- Receiving omap updates from the primary
- Recovering omap data during shard recovery

For example, in a k=4, m=2 profile, shards 0, 4, and 5 are primary-capable.

Selection and Management
~~~~~~~~~~~~~~~~~~~~~~~~~

Primary-capable shards are determined by the EC profile:

- The first data shard is always primary-capable
- All parity shards are primary-capable
- Other data shards are not primary-capable

This selection provides a good balance between availability (m + 1 replicas)
and overhead (not replicating to all k + m shards).

The primary OSD manages omap operations and coordinates updates to all
primary-capable shards.

Failover Handling
~~~~~~~~~~~~~~~~~

When a primary-capable shard fails:

#. The primary detects the failure
#. Omap operations continue using remaining primary-capable shards
#. If the failed shard was the primary, a new primary is elected from the
   remaining primary-capable shards
#. The new primary continues serving omap operations (it already has a complete
   omap replica)
#. When the failed shard recovers, it receives omap data through the normal EC
   recovery process

This failover mechanism ensures high availability of omap data.


Consistency and Ordering
=========================

Omap Consistency
----------------

Journal-Based Consistency Model
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The journal-based consistency model provides strong guarantees:

- **Atomicity**: Omap updates are atomic - either all replicas are updated or
  none are
- **Durability**: Once an update is acknowledged, it is durable across multiple
  shards
- **Consistency**: All replicas maintain the same omap state
- **Isolation**: Concurrent updates are serialized through the journal

These guarantees ensure that applications can rely on omap data being
consistent and durable.

Conflict Resolution
~~~~~~~~~~~~~~~~~~~

Conflicts are prevented through serialization:

- All omap updates go through the primary OSD
- The primary serializes updates using the journal
- Concurrent updates from different clients are ordered by the primary
- No conflict resolution is needed because conflicts cannot occur

This approach is simpler and more reliable than optimistic conflict resolution.

Recovery Scenarios
~~~~~~~~~~~~~~~~~~

Several recovery scenarios are handled:

**Shard Recovery:**

#. Recovering shard requests journal from primary
#. Primary sends journal entries since last known state
#. Recovering shard replays journal to catch up
#. Shard rejoins as primary-capable

**Primary Failure:**

#. New primary is elected
#. New primary has complete omap state (as a primary-capable shard)
#. Operations continue without data loss

**Multiple Shard Failures:**

#. If majority of primary-capable shards survive, operations continue
#. If too many shards fail, PG becomes unavailable until recovery

Operation Ordering
------------------

Interaction Between Data and Omap Operations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Data and omap operations are ordered to maintain consistency:

- Operations to the same object are serialized
- Omap updates are applied before the operation is acknowledged
- Data writes and omap updates are atomic (both succeed or both fail)

This ordering ensures that applications see a consistent view of objects and
their metadata.

Synchronous Operation Ordering Guarantees
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Synchronous operations provide strong ordering guarantees:

- Synchronous reads see all previously committed writes
- Synchronous reads block concurrent writes to the same object
- Multiple synchronous reads to the same object are serialized

These guarantees are necessary for Cls operations to function correctly.

Cls Operation Ordering
~~~~~~~~~~~~~~~~~~~~~~~

Cls operations are ordered with respect to other operations:

- Cls operations are serialized per object
- Cls operations see a consistent snapshot of object state
- Cls operations' effects are atomic

This ordering ensures that Cls methods can safely read, process, and write
data without race conditions.

Failure Scenarios
=================

Shard Failures
--------------

Omap Data Recovery
~~~~~~~~~~~~~~~~~~

When a shard fails and recovers, omap data is recovered through journal replay:

#. Recovering shard identifies its last known journal position
#. Primary sends journal entries since that position
#. Recovering shard applies journal entries in order
#. Once caught up, shard rejoins as primary-capable

This process ensures that the recovered shard has consistent omap data.

Degraded Mode Operations
~~~~~~~~~~~~~~~~~~~~~~~~~

When some primary-capable shards are down, the PG operates in degraded mode:

- Omap operations continue using available primary-capable shards
- Performance may be reduced due to fewer replicas
- If too few shards are available, omap operations are blocked
- Data operations continue normally (using EC reconstruction if needed)

This degraded mode operation ensures availability while maintaining consistency.


Performance Considerations
==========================

Omap Performance
----------------

Journal Overhead
~~~~~~~~~~~~~~~~

The journal introduces some performance overhead:

- **Journal Updates**: Each omap update requires the journal to be updated
- **Latency**: Omap operations require the primary osd to check the journal for
  updates
- **Storage**: Journal entries consume memory on the primary osd

However, this overhead is acceptable given the consistency guarantees provided.
Performance testing will quantify the impact and guide optimisation efforts.

Replication Impact
~~~~~~~~~~~~~~~~~~

Replicating omap data across primary-capable shards has performance
implications:

- **Network Traffic**: Updates generate network traffic to multiple shards
- **Storage**: Each primary-capable shard stores a complete omap replica
- **CPU**: Applying updates on multiple shards consumes CPU

These costs are offset by the benefits of high availability and fast reads.

Synchronous Read Performance
-----------------------------

Coroutine Overhead
~~~~~~~~~~~~~~~~~~

Coroutines introduce minimal overhead:

- **Creation**: Creating a coroutine is faster than creating a thread
- **Suspension**: Suspending a coroutine is a lightweight operation
- **Resumption**: Resuming a coroutine is also lightweight

PG Blocking Impact
~~~~~~~~~~~~~~~~~~

Blocking other operations during synchronous reads can impact performance:

- **Latency**: Queued operations experience additional latency
- **Throughput**: PG throughput may be reduced if many synchronous reads are
  in flight
- **Fairness**: Long-running synchronous reads can starve other operations

Mitigation Strategies
~~~~~~~~~~~~~~~~~~~~~

To minimize the performance impact of synchronous reads:

- **Concurrency Limits**: Limit concurrent synchronous reads per PG
- **Timeouts**: Enforce strict timeouts on synchronous operations
- **Prioritization**: Prioritize data operations over Cls operations when
  appropriate
- **Monitoring**: Monitor synchronous read latency and adjust limits

These strategies ensure that synchronous reads don't significantly impact
overall system performance.

Cls Operation Performance
--------------------------

Comparison with Replicated Pools
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cls operations in EC pools have different performance characteristics than in
replicated pools:

- **Latency**: Higher latency due to synchronous read overhead and EC
  reconstruction
- **Throughput**: Lower throughput due to PG blocking and serialization
- **CPU**: Higher CPU usage due to EC encoding/decoding

However, the storage efficiency benefits of EC pools often outweigh these
performance costs.


Testing
=======

Test Strategy Overview
----------------------

The testing strategy for these features is comprehensive and multi-layered:

- **Unit Tests**: Test individual components in isolation
- **Integration Tests**: Test interactions between components
- **Teuthology Tests**: TODO

A key aspect of the testing strategy is the use of a common test class that
enables running existing tests on both replicated and fast EC pools.

Common Test Class Approach
~~~~~~~~~~~~~~~~~~~~~~~~~~~

A common test class has been implemented that:

- Provides a unified interface for test cases
- Supports both replicated and FastEC pools
- Allows existing test suites to run on EC pools with minimal modifications
- Ensures consistent test coverage across pool types

This approach significantly increases test coverage while minimizing test
development effort.

Omap Testing
------------

Basic Operations
~~~~~~~~~~~~~~~~

Basic omap operation tests verify:

- Setting and getting key-value pairs
- Removing keys
- Clearing all entries
- Getting and setting headers
- Iterating over keys and values

These tests ensure that basic omap functionality works correctly.

Recovery Testing
~~~~~~~~~~~~~~~~~~~

Omap recovery tests verify:

- Recovery when the primary goes down
- Recovery when the omap is larger than 2 chunks

Synchronous Read Testing
-------------------------

Correctness Testing
~~~~~~~~~~~~~~~~~~~

Correctness tests verify:

- Synchronous reads return correct data
- Synchronous reads see committed writes
- Synchronous reads block concurrent writes appropriately
- Coroutine suspension and resumption work correctly

These tests ensure that synchronous reads provide correct semantics.

Cls Testing
-----------

Common Test Class Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The common test class for Cls testing:

- Inherits from existing Cls test base classes
- Adds support for EC pool creation and configuration
- Provides helper methods for EC-specific testing
- Enables running existing Cls tests on EC pools

This implementation allows extensive reuse of existing test code.

FastEC Pool Testing
~~~~~~~~~~~~~~~~~~~

FastEC pool testing verifies:

- All Cls operations work correctly on FastEC pools
- Performance is acceptable for FastEC workloads
- Failure handling works correctly
- Recovery mechanisms function properly

FastEC pools are the primary target for these features.

RBD-Specific Tests
~~~~~~~~~~~~~~~~~~

RBD-specific tests verify:

- RBD images can be created on EC pools
- RBD operations (read, write, snapshot, clone) work correctly
- RBD metadata operations function properly
- RBD performance is acceptable

These tests ensure that RBD, a primary use case, works correctly.

Existing Test Suite Integration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Integration with existing test suites includes:

- Running existing Cls tests on EC pools
- Running existing RBD tests on EC pools
- Running existing omap tests on EC pools
- Identifying and fixing any EC-specific issues

This integration ensures comprehensive coverage with minimal new test
development.

Integration Testing
-------------------

Combined Omap and Cls Operations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Integration tests verify:

- Cls operations that use omap work correctly
- Omap updates within Cls operations are atomic
- Concurrent Cls and omap operations are handled correctly
- Failure during combined operations is handled properly

These tests ensure that the features work together correctly.

RBD Workload Testing
~~~~~~~~~~~~~~~~~~~~

RBD workload tests verify:

- Realistic RBD workloads function correctly on EC pools
- Performance is acceptable for production use
- Failure scenarios are handled gracefully
- Recovery works correctly under RBD workloads

These tests validate the primary use case for these features.

End-to-End Scenarios
~~~~~~~~~~~~~~~~~~~~

End-to-end tests verify:

- Complete workflows from client to storage
- Multiple clients accessing EC pools simultaneously
- Mixed workloads (data, omap, Cls operations)
- Long-running stability tests

These tests ensure that the system works correctly in realistic scenarios.

Test Tools
----------

ceph_test_rados Enhancements
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ceph_test_rados tool has been enhanced to:

- Support EC pool testing
- Test omap operations on EC pools
- Test Cls operations on EC pools
- Provide detailed failure injection capabilities

These enhancements enable comprehensive testing of the new features.

Existing Tools Usage
~~~~~~~~~~~~~~~~~~~~

Existing tools are used extensively:

- **ceph_test_rados**: General RADOS testing
- **rbd_test**: RBD testing on EC pools
- **rados bench**: Performance testing

Leveraging existing tools reduces development effort and ensures consistency.

Migration and Compatibility
===========================

Release Requirements
--------------------

Umbrella Release Requirement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ability to enable omap support on EC pools is tied to the **Umbrella
release**. This introduces important version requirements:

- **All OSDs must be running at least the Umbrella release** before omap
  support can be enabled on any EC pool
- **Any OSDs added to the cluster in the future** must also be running at least
  the Umbrella release
- Attempting to enable omap support on a cluster with pre-Umbrella OSDs will
  fail with an error

This requirement ensures that all OSDs in the cluster have the necessary code
to support omap operations on EC pools, preventing data corruption or
inconsistencies that could arise from version mismatches.

.. WARNING::
   Enabling omap support on EC pools requires all OSDs to be at the Umbrella
   release or later. Mixed-version clusters with pre-Umbrella OSDs cannot use
   this feature.

Upgrade Path
------------

Enabling Features on Existing Pools
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Existing EC pools can be upgraded to support the new features:

#. Upgrade all OSDs to the Umbrella release or later
#. Verify that all OSDs in the cluster are at the required version
#. Enable omap support on the pool (if desired)
#. Enable Cls support on the pool (if desired)
#. Existing data remains accessible throughout the upgrade

The upgrade process is designed to be non-disruptive, but the version
requirement must be strictly enforced.

Backward Compatibility
~~~~~~~~~~~~~~~~~~~~~~

Backward compatibility is maintained with important caveats:

- Pools without omap support continue to work as before on any version
- Pools without Cls support continue to work as before on any version
- Clients that don't use the new features are unaffected
- **Downgrade is not supported** once omap has been enabled on an EC pool, as
  pre-Umbrella OSDs cannot handle omap data on EC pools
- Pools with omap enabled require all OSDs to remain at Umbrella or later

This compatibility ensures that upgrades are safe, but downgrades are
restricted to protect data integrity.

Configuration
-------------

Required Settings
~~~~~~~~~~~~~~~~~

To enable the new features, the following settings are required:

- ``allows_ec_overwrites = true`
- ``allows_ec_optimizations = true``
- ``supports_omap = true``

These settings can be configured per-pool. The cluster will enforce
that all OSDs are at the Umbrella release before allowing omap to be enabled.

Crimson Implementation
======================

The omap support described in this document applies to the classic OSD
implementation. A separate implementation effort is required for Crimson (the
next-generation OSD implementation).

Crimson-Specific Considerations
--------------------------------

The Crimson implementation will need to:

- Implement omap support using Crimson's asynchronous architecture
- Integrate with Crimson's seastar-based I/O framework
- Adapt the journal mechanism to Crimson's storage backend
- Ensure compatibility with the classic OSD implementation

The Crimson implementation is a separate work item and is not covered in detail
in this document.

References
==========

Code References
---------------

- ``src/osd/ECBackend.h``: EC backend header
- ``src/osd/ECBackend.cc``: EC backend implementation
- ``src/osd/PrimaryLogPG.h``: Primary Log PG header
- ``src/osd/PrimaryLogPG.cc``: Primary Log PG implementation
- ``src/cls/``: Cls implementation directory

External Documentation
----------------------

- Ceph Documentation: https://docs.ceph.com/
- RADOS Paper: https://ceph.com/papers/weil-rados-pdsw07.pdf
- Erasure Coding Overview: https://en.wikipedia.org/wiki/Erasure_code
- Boost.Coroutine2: https://www.boost.org/doc/libs/release/libs/coroutine2/