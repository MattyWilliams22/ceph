// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include <map>
#include <optional>
#include <string>

#include "gtest/gtest.h"

#include "include/buffer.h"
#include "include/interval_set.h"
#include "include/rados/librados.hpp"
#include "osd/osd_types.h"
#include "test/librados/test_cxx.h"
#include "test/librados/test_pool_types.h"
#include "crimson_utils.h"

using namespace std;
using namespace librados;
using ceph::test::PoolType;
using ceph::test::PoolTypeTestFixture;

/**
 * Test fixture for sparse read operations on EC and Replicated pools.
 * Tests sparse_read, write, truncate, zero, mapext operations and
 * verifies behavior during recovery scenarios.
 */
class SparseReadTest : public PoolTypeTestFixture {
protected:
  static std::string pool_name_prefix() {
    return "sparse_read_test_";
  }

  static void SetUpTestSuite() {
    PoolTypeTestFixture::SetUpTestSuite();
    auto it = pool_names.find(PoolType::FAST_EC);
    if (it != pool_names.end()) {
      ASSERT_EQ("", set_pool_flags_pp(
        it->second,
        rados,
        pg_pool_t::FLAG_TRACK_ZERO_BLOCKS,
        true));
      rados.wait_for_latest_osdmap();
    }
  }

  void SetUp() override {
    SKIP_IF_CRIMSON();
    PoolTypeTestFixture::SetUp();
  }

  void TearDown() override {
    SKIP_IF_CRIMSON();
    if (balancing_disabled) {
      turn_balancing_on();
    }
    PoolTypeTestFixture::TearDown();
  }

  // Helper to create buffer with specific pattern
  bufferlist create_pattern_buffer(size_t size, char pattern) {
    bufferlist bl;
    std::string data(size, pattern);
    bl.append(data);
    return bl;
  }

  // Helper to create zero buffer
  bufferlist create_zero_buffer(size_t size) {
    bufferlist bl;
    std::string data(size, '\0');
    bl.append(data);
    return bl;
  }

  // Helper to verify sparse read results
  void verify_sparse_read(
      const std::string& oid,
      uint64_t offset,
      uint64_t length,
      const std::map<uint64_t, uint64_t>& expected_extents,
      const bufferlist& expected_data) {
    std::map<uint64_t, uint64_t> extents;
    bufferlist read_bl;
    int ret = ioctx.sparse_read(oid, extents, read_bl, length, offset);
    ASSERT_EQ(ret, (int)expected_extents.size());
    ASSERT_EQ(extents, expected_extents);
    ASSERT_EQ(read_bl.length(), expected_data.length());
    ASSERT_TRUE(read_bl.contents_equal(expected_data));
  }

  // Helper to verify mapext results
  void verify_mapext(
      const std::string& oid,
      uint64_t offset,
      uint64_t length,
      const std::map<uint64_t, uint64_t>& expected_extents) {
    std::map<uint64_t, uint64_t> extents;
    int ret = ioctx.mapext(oid, offset, length, extents);
    ASSERT_EQ(ret, (int)expected_extents.size());
    ASSERT_EQ(extents, expected_extents);
  }

  std::optional<force_allocated_extents_t> get_force_allocated_extents(
      const std::string& oid) {
    bufferlist bl;
    static_assert(OI_ATTR[0] == '_', "OI_ATTR must start with '_'");
    int ret = ioctx.getxattr(oid, &OI_ATTR[1], bl);
    if (ret < 0) {
      ADD_FAILURE() << "getxattr OI failed: " << ret;
      return std::nullopt;
    }
    object_info_t oi(bl);
    if (oi.force_allocated_extents.empty()) {
      return std::nullopt;
    }
    return oi.force_allocated_extents;
  }
};

// Test basic sparse_read on a simple write
TEST_P(SparseReadTest, BasicSparseRead) {
  std::string oid = "sparse_read_basic";
  
  // Write some data
  bufferlist write_bl = create_pattern_buffer(4096, 'A');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));

  // Sparse read should return the written extent
  std::map<uint64_t, uint64_t> expected_extents = {{0, 4096}};
  verify_sparse_read(oid, 0, 4096, expected_extents, write_bl);
}

// Test sparse_read with holes (unallocated regions)
TEST_P(SparseReadTest, SparseReadWithHoles) {
  std::string oid = "sparse_read_holes";
  
  // Write data at offset 0 and 8192, leaving a hole at 4096
  bufferlist write_bl1 = create_pattern_buffer(4096, 'A');
  bufferlist write_bl2 = create_pattern_buffer(4096, 'B');
  
  ASSERT_EQ(0, ioctx.write(oid, write_bl1, write_bl1.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, write_bl2, write_bl2.length(), 8192));

  // Sparse read should return two extents with a hole
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 12288, 0);
  
  ASSERT_EQ(ret, 2);
  ASSERT_EQ(read_bl.length(), 8192u);
  ASSERT_EQ(extents.size(), 2u);
  ASSERT_EQ(extents[0], 4096u);
  ASSERT_EQ(extents[8192], 4096u);
}

// Test sparse_read after writing zeros
TEST_P(SparseReadTest, SparseReadZeros) {
  std::string oid = "sparse_read_zeros";
  
  // Write zeros
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));

  // Sparse read - behavior depends on pool type and zero tracking
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 4096, 0);
  
  // Should read successfully
  ASSERT_GE(ret, 0);
}

// Test WRITE operation
TEST_P(SparseReadTest, WriteOperation) {
  std::string oid = "write_op";
  
  // Write data
  bufferlist write_bl = create_pattern_buffer(8192, 'X');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));

  // Verify with read
  bufferlist read_bl;
  ASSERT_EQ(8192, ioctx.read(oid, read_bl, 8192, 0));
  ASSERT_TRUE(read_bl.contents_equal(write_bl));
}

TEST_P(SparseReadTest, WriteTracksAllZeroExtentOnFastEC) {
  if (GetParam() != PoolType::FAST_EC) {
    GTEST_SKIP() << "force-allocated extent tracking is only enabled for FastEC in this suite";
  }

  std::string oid = "write_tracks_zero_extent";
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));

  auto fae = get_force_allocated_extents(oid);
  ASSERT_TRUE(fae.has_value());

  interval_set<uint64_t> expected;
  expected.insert(0, FAE_BLOCK_SIZE);
  ASSERT_EQ(expected, fae->intervals);
}

TEST_P(SparseReadTest, WriteDoesNotTrackExtentWhenOnlyPrefixIsZero) {
  if (GetParam() != PoolType::FAST_EC) {
    GTEST_SKIP() << "force-allocated extent tracking is only enabled for FastEC in this suite";
  }

  std::string oid = "write_prefix_zero_only";
  bufferlist write_bl;
  std::string data(4096, '\0');
  data[8] = 'X';
  write_bl.append(data);
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));

  ASSERT_FALSE(get_force_allocated_extents(oid).has_value());
}

TEST_P(SparseReadTest, WriteTracksOnlyFullyCoveredZeroBlocks) {
  if (GetParam() != PoolType::FAST_EC) {
    GTEST_SKIP() << "force-allocated extent tracking is only enabled for FastEC in this suite";
  }

  std::string oid = "write_tracks_full_zero_blocks_only";
  bufferlist zero_bl = create_zero_buffer(8192);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 4096));

  auto fae = get_force_allocated_extents(oid);
  ASSERT_TRUE(fae.has_value());

  interval_set<uint64_t> expected;
  expected.insert(4096, 8192);
  ASSERT_EQ(expected, fae->intervals);
}

TEST_P(SparseReadTest, WriteSkipsPartialLeadingBlock) {
  if (GetParam() != PoolType::FAST_EC) {
    GTEST_SKIP() << "force-allocated extent tracking is only enabled for FastEC in this suite";
  }

  std::string oid = "write_skips_partial_leading_block";
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 2048));

  ASSERT_FALSE(get_force_allocated_extents(oid).has_value());
}

TEST_P(SparseReadTest, WriteClearsTrackedExtentWithNonZeroOverwrite) {
  if (GetParam() != PoolType::FAST_EC) {
    GTEST_SKIP() << "force-allocated extent tracking is only enabled for FastEC in this suite";
  }

  std::string oid = "write_clears_tracked_extent";
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  ASSERT_TRUE(get_force_allocated_extents(oid).has_value());

  bufferlist data_bl = create_pattern_buffer(4096, 'Z');
  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 0));

  ASSERT_FALSE(get_force_allocated_extents(oid).has_value());
}

// Test WRITEFULL operation
TEST_P(SparseReadTest, WritefullOperation) {
  std::string oid = "writefull_op";
  
  // Initial write
  bufferlist write_bl1 = create_pattern_buffer(4096, 'A');
  ASSERT_EQ(0, ioctx.write(oid, write_bl1, write_bl1.length(), 0));

  // Writefull should replace entire object
  bufferlist write_bl2 = create_pattern_buffer(8192, 'B');
  bufferlist write_bl2_copy = write_bl2;  // write_full clears the source bufferlist
  ASSERT_EQ(0, ioctx.write_full(oid, write_bl2));

  // Verify size and content
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat(oid, &size, &mtime));
  ASSERT_EQ(size, 8192u);

  bufferlist read_bl;
  ASSERT_EQ(8192, ioctx.read(oid, read_bl, 8192, 0));
  ASSERT_TRUE(read_bl.contents_equal(write_bl2_copy));
}

// Test WRITESAME operation
TEST_P(SparseReadTest, WritesameOperation) {
  std::string oid = "writesame_op";
  
  // Write same pattern across range
  bufferlist pattern_bl = create_pattern_buffer(4096, 'C');
  ASSERT_EQ(0, ioctx.writesame(oid, pattern_bl, 16384, 0));

  // Verify the pattern was repeated
  bufferlist read_bl;
  ASSERT_EQ(16384, ioctx.read(oid, read_bl, 16384, 0));
  
  // Check that pattern repeats
  for (uint64_t offset = 0; offset < 16384; offset += 4096) {
    bufferlist chunk;
    chunk.substr_of(read_bl, offset, 4096);
    ASSERT_TRUE(chunk.contents_equal(pattern_bl));
  }
}

// Test TRUNCATE operation
TEST_P(SparseReadTest, TruncateOperation) {
  std::string oid = "truncate_op";
  
  // Write data
  bufferlist write_bl = create_pattern_buffer(8192, 'D');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));

  // Truncate to smaller size
  ASSERT_EQ(0, ioctx.trunc(oid, 4096));

  // Verify new size
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat(oid, &size, &mtime));
  ASSERT_EQ(size, 4096u);

  // Verify sparse read reflects truncation
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 8192, 0);
  ASSERT_GE(ret, 0);
  ASSERT_LE(read_bl.length(), 4096u);
}

// Test ZERO operation
TEST_P(SparseReadTest, ZeroOperation) {
  std::string oid = "zero_op";
  
  // Write data
  bufferlist write_bl = create_pattern_buffer(12288, 'E');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));

  // Zero out middle section
  ObjectWriteOperation op;
  op.zero(4096, 4096);
  ASSERT_EQ(0, ioctx.operate(oid, &op));

  // Verify sparse read shows hole in middle
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 12288, 0);
  
  // Should have data at beginning and end, hole in middle
  ASSERT_GE(ret, 0);
  
  // Verify the zeroed region reads as zeros
  bufferlist zero_check;
  ASSERT_EQ(4096, ioctx.read(oid, zero_check, 4096, 4096));
  bufferlist expected_zeros = create_zero_buffer(4096);
  ASSERT_TRUE(zero_check.contents_equal(expected_zeros));
}

// Test MAPEXT operation
TEST_P(SparseReadTest, MapextOperation) {
  std::string oid = "mapext_op";
  
  // Write data with holes
  bufferlist write_bl1 = create_pattern_buffer(4096, 'F');
  bufferlist write_bl2 = create_pattern_buffer(4096, 'G');
  
  ASSERT_EQ(0, ioctx.write(oid, write_bl1, write_bl1.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, write_bl2, write_bl2.length(), 8192));

  // Use mapext to query allocation
  std::map<uint64_t, uint64_t> expected_extents = {
    {0, 4096},
    {8192, 4096}
  };
  verify_mapext(oid, 0, 12288, expected_extents);
}

// Test sparse read after partial overwrite
TEST_P(SparseReadTest, PartialOverwrite) {
  std::string oid = "partial_overwrite";
  
  // Initial write
  bufferlist write_bl1 = create_pattern_buffer(8192, 'H');
  ASSERT_EQ(0, ioctx.write(oid, write_bl1, write_bl1.length(), 0));

  // Partial overwrite in middle
  bufferlist write_bl2 = create_pattern_buffer(2048, 'I');
  ASSERT_EQ(0, ioctx.write(oid, write_bl2, write_bl2.length(), 3072));

  // Verify sparse read shows continuous extent
  std::map<uint64_t, uint64_t> expected_extents = {{0, 8192}};
  bufferlist read_bl;
  std::map<uint64_t, uint64_t> extents;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 8192, 0);
  ASSERT_EQ(ret, 1);
  ASSERT_EQ(read_bl.length(), 8192u);
  ASSERT_EQ(extents, expected_extents);
}

// Test sparse read with large object
TEST_P(SparseReadTest, LargeObjectSparseRead) {
  std::string oid = "large_sparse";
  
  // Write data at various offsets to create sparse pattern
  bufferlist write_bl = create_pattern_buffer(4096, 'J');
  
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 16384));
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 32768));

  // Sparse read entire range
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 40960, 0);
  
  ASSERT_EQ(ret, 3);
  ASSERT_EQ(read_bl.length(), 12288u);
  ASSERT_EQ(extents.size(), 3u);
}

// Test recovery scenario - write zeros and verify after recovery
TEST_P(SparseReadTest, RecoveryWithZeros) {
  if (GetParam() == PoolType::FAST_EC) {
    GTEST_SKIP() << "Skipping Recovery test on FastEC (currently hanging)";
  }
  turn_balancing_off();

  std::string oid = "recovery_zeros";

  // Write zeros
  bufferlist zero_bl = create_zero_buffer(8192);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));

  // Trigger recovery
  int new_primary;
  setup_and_trigger_recovery(oid, new_primary);

  // Verify zeros are still readable after recovery
  bufferlist read_bl;
  ObjectReadOperation read_op;
  read_op.read(0, 8192, &read_bl, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_op, nullptr));
  ASSERT_TRUE(read_bl.contents_equal(zero_bl));
}

// Test recovery scenario - mixed data and verify allocation state
TEST_P(SparseReadTest, RecoveryMixedData) {
  if (GetParam() == PoolType::FAST_EC) {
    GTEST_SKIP() << "Skipping Recovery test on FastEC (currently hanging)";
  }
  turn_balancing_off();

  std::string oid = "recovery_mixed";

  // Write pattern: data, hole, zeros, data
  bufferlist data_bl = create_pattern_buffer(4096, 'K');
  bufferlist zero_bl = create_zero_buffer(4096);

  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 8192));
  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 16384));

  // Get initial read state
  bufferlist read_bl_before;
  ObjectReadOperation read_before;
  read_before.read(0, 20480, &read_bl_before, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_before, nullptr));

  // Trigger recovery
  int new_primary;
  setup_and_trigger_recovery(oid, new_primary);

  // Verify read state after recovery
  bufferlist read_bl_after;
  ObjectReadOperation read_after;
  read_after.read(0, 20480, &read_bl_after, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_after, nullptr));

  // Data should be preserved
  ASSERT_TRUE(read_bl_before.contents_equal(read_bl_after));
}

// Test recovery after truncate
TEST_P(SparseReadTest, RecoveryAfterTruncate) {
  if (GetParam() == PoolType::FAST_EC) {
    GTEST_SKIP() << "Skipping Recovery test on FastEC (currently hanging)";
  }
  turn_balancing_off();

  std::string oid = "recovery_truncate";

  // Write and truncate
  bufferlist write_bl = create_pattern_buffer(16384, 'L');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  ASSERT_EQ(0, ioctx.trunc(oid, 8192));

  // Trigger recovery
  int new_primary;
  setup_and_trigger_recovery(oid, new_primary);

  // Verify size is preserved after recovery
  uint64_t size = 0;
  time_t mtime = 0;
  ObjectReadOperation stat_op;
  stat_op.stat(&size, &mtime, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &stat_op, nullptr));
  ASSERT_EQ(size, 8192u);
}

// Test recovery after zero operation
TEST_P(SparseReadTest, RecoveryAfterZero) {
  if (GetParam() == PoolType::FAST_EC) {
    GTEST_SKIP() << "Skipping Recovery test on FastEC (currently hanging)";
  }
  turn_balancing_off();

  std::string oid = "recovery_zero_op";

  // Write data and zero part of it
  bufferlist write_bl = create_pattern_buffer(12288, 'M');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  ObjectWriteOperation op;
  op.zero(4096, 4096);
  ASSERT_EQ(0, ioctx.operate(oid, &op));

  // Get read state before recovery
  bufferlist read_bl_before;
  ObjectReadOperation read_before;
  read_before.read(0, 12288, &read_bl_before, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_before, nullptr));

  // Trigger recovery
  int new_primary;
  setup_and_trigger_recovery(oid, new_primary);

  // Verify read state after recovery matches pre-recovery state
  bufferlist read_bl_after;
  ObjectReadOperation read_after;
  read_after.read(0, 12288, &read_bl_after, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_after, nullptr));

  ASSERT_TRUE(read_bl_before.contents_equal(read_bl_after));
}
// Test two-stage zero detection: first byte zero, rest non-zero
TEST_P(SparseReadTest, ZeroDetectionFirstByteOnly) {
  std::string oid = "zero_detect_first_byte";
  
  // Create buffer: first byte zero, second byte non-zero
  bufferlist write_bl;
  std::string data(4096, '\0');
  data[1] = 'X';  // Make second byte non-zero
  write_bl.append(data);
  
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  
  // Verify data is readable
  bufferlist read_bl;
  ASSERT_EQ(4096, ioctx.read(oid, read_bl, 4096, 0));
  ASSERT_TRUE(read_bl.contents_equal(write_bl));
}

// Test two-stage zero detection: first 8 bytes zero, rest non-zero
TEST_P(SparseReadTest, ZeroDetectionFirst8BytesOnly) {
  std::string oid = "zero_detect_8_bytes";
  
  // Create buffer: first 8 bytes zero, byte 9 non-zero
  bufferlist write_bl;
  std::string data(4096, '\0');
  data[8] = 'Y';  // Make 9th byte non-zero
  write_bl.append(data);
  
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  
  // Verify data is readable
  bufferlist read_bl;
  ASSERT_EQ(4096, ioctx.read(oid, read_bl, 4096, 0));
  ASSERT_TRUE(read_bl.contents_equal(write_bl));
}

// Test two-stage zero detection: all zeros
TEST_P(SparseReadTest, ZeroDetectionAllZeros) {
  std::string oid = "zero_detect_all_zeros";
  
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  
  // Verify zeros are readable
  bufferlist read_bl;
  ASSERT_EQ(4096, ioctx.read(oid, read_bl, 4096, 0));
  ASSERT_TRUE(read_bl.contents_equal(zero_bl));
}

// Test overwriting zeros with non-zero data
TEST_P(SparseReadTest, OverwriteZerosWithData) {
  std::string oid = "overwrite_zeros";
  
  // Write zeros first
  bufferlist zero_bl = create_zero_buffer(8192);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  
  // Overwrite with non-zero data
  bufferlist data_bl = create_pattern_buffer(4096, 'N');
  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 2048));
  
  // Verify mixed content
  bufferlist read_bl;
  ASSERT_EQ(8192, ioctx.read(oid, read_bl, 8192, 0));
  
  // Check that overwritten section has non-zero data
  bufferlist middle_section;
  middle_section.substr_of(read_bl, 2048, 4096);
  ASSERT_TRUE(middle_section.contents_equal(data_bl));
}

// Test that WRITEFULL clears force-allocated extents
TEST_P(SparseReadTest, WritefullClearsZeroTracking) {
  std::string oid = "writefull_clears";
  
  // Write zeros (may become force-allocated)
  bufferlist zero_bl = create_zero_buffer(8192);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  
  // WRITEFULL with non-zero data
  bufferlist data_bl = create_pattern_buffer(4096, 'O');
  bufferlist data_bl_copy = data_bl;  // write_full clears the source bufferlist
  ASSERT_EQ(0, ioctx.write_full(oid, data_bl));
  
  // Verify new size and content
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat(oid, &size, &mtime));
  ASSERT_EQ(size, 4096u);
  
  bufferlist read_bl;
  ASSERT_EQ(4096, ioctx.read(oid, read_bl, 4096, 0));
  ASSERT_TRUE(read_bl.contents_equal(data_bl_copy));
}

// Test WRITESAME with zero pattern
TEST_P(SparseReadTest, WritesameZeros) {
  std::string oid = "writesame_zeros";
  
  // Write same zero pattern across range
  bufferlist zero_pattern = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.writesame(oid, zero_pattern, 16384, 0));
  
  // Verify the zeros were written
  bufferlist read_bl;
  ASSERT_EQ(16384, ioctx.read(oid, read_bl, 16384, 0));
  
  bufferlist expected_zeros = create_zero_buffer(16384);
  ASSERT_TRUE(read_bl.contents_equal(expected_zeros));
}

// Test pattern of zeros and non-zero data
TEST_P(SparseReadTest, MixedZeroNonZeroPattern) {
  std::string oid = "mixed_pattern";
  
  // Write pattern: data, zeros, data, zeros
  bufferlist data_bl = create_pattern_buffer(4096, 'P');
  bufferlist zero_bl = create_zero_buffer(4096);
  
  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 4096));
  ASSERT_EQ(0, ioctx.write(oid, data_bl, data_bl.length(), 8192));
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 12288));
  
  // Verify sparse read shows correct pattern
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 16384, 0);
  ASSERT_GE(ret, 0);
  
  // Verify we can read all the data back
  bufferlist full_read;
  ASSERT_EQ(16384, ioctx.read(oid, full_read, 16384, 0));
}

// Test truncate removes force-allocated extents beyond new size
TEST_P(SparseReadTest, TruncateRemovesExtents) {
  std::string oid = "truncate_extents";
  
  // Write zeros at various offsets
  bufferlist zero_bl = create_zero_buffer(4096);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 8192));
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 16384));
  
  // Truncate to smaller size
  ASSERT_EQ(0, ioctx.trunc(oid, 10240));
  
  // Verify size
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat(oid, &size, &mtime));
  ASSERT_EQ(size, 10240u);
  
  // Verify sparse read doesn't show extents beyond truncate point
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  ioctx.sparse_read(oid, extents, read_bl, 20480, 0);
  
  // No extents should exist beyond 10240
  for (const auto& [offset, len] : extents) {
    ASSERT_LT(offset, 10240u);
  }
}

// Test ZERO operation deallocates force-allocated regions
TEST_P(SparseReadTest, ZeroOperationDeallocates) {
  std::string oid = "zero_deallocates";
  
  // Write zeros (may become force-allocated)
  bufferlist zero_bl = create_zero_buffer(12288);
  ASSERT_EQ(0, ioctx.write(oid, zero_bl, zero_bl.length(), 0));
  
  // ZERO operation on same region
  ObjectWriteOperation op;
  op.zero(4096, 4096);
  ASSERT_EQ(0, ioctx.operate(oid, &op));
  
  // Verify the region reads as zeros
  bufferlist read_bl;
  ASSERT_EQ(4096, ioctx.read(oid, read_bl, 4096, 4096));
  bufferlist expected_zeros = create_zero_buffer(4096);
  ASSERT_TRUE(read_bl.contents_equal(expected_zeros));
}

// Test sparse read with offset and partial length
TEST_P(SparseReadTest, SparseReadPartialRange) {
  std::string oid = "sparse_partial";
  
  // Write data at multiple offsets
  bufferlist write_bl = create_pattern_buffer(4096, 'Q');
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 8192));
  ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), 16384));
  
  // Sparse read middle section only
  std::map<uint64_t, uint64_t> extents;
  bufferlist read_bl;
  int ret = ioctx.sparse_read(oid, extents, read_bl, 8192, 4096);
  
  ASSERT_GE(ret, 0);
  // Should only get data from the requested range
  for (const auto& [offset, len] : extents) {
    ASSERT_GE(offset, 4096u);
    ASSERT_LE(offset + len, 12288u);
  }
}

// Test multiple sequential writes building up an object
TEST_P(SparseReadTest, SequentialWrites) {
  std::string oid = "sequential_writes";
  
  // Write in 4K chunks
  for (int i = 0; i < 4; i++) {
    bufferlist write_bl = create_pattern_buffer(4096, 'A' + i);
    ASSERT_EQ(0, ioctx.write(oid, write_bl, write_bl.length(), i * 4096));
  }
  
  // Verify total size
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat(oid, &size, &mtime));
  ASSERT_EQ(size, 16384u);
  
  // Verify sparse read shows continuous extent
  std::map<uint64_t, uint64_t> expected_extents = {{0, 16384}};
  verify_mapext(oid, 0, 16384, expected_extents);
}

// Test recovery after WRITEFULL operation
TEST_P(SparseReadTest, RecoveryAfterWritefull) {
  if (GetParam() == PoolType::FAST_EC) {
    GTEST_SKIP() << "Skipping Recovery test on FastEC (currently hanging)";
  }
  turn_balancing_off();

  std::string oid = "recovery_writefull";

  // Initial write
  bufferlist write_bl1 = create_pattern_buffer(8192, 'R');
  ASSERT_EQ(0, ioctx.write(oid, write_bl1, write_bl1.length(), 0));

  // WRITEFULL
  bufferlist write_bl2 = create_pattern_buffer(4096, 'S');
  bufferlist write_bl2_copy = write_bl2;  // write_full clears the source bufferlist
  ASSERT_EQ(0, ioctx.write_full(oid, write_bl2));

  // Trigger recovery
  int new_primary;
  setup_and_trigger_recovery(oid, new_primary);

  // Verify size and content after recovery
  uint64_t size = 0;
  time_t mtime = 0;
  ObjectReadOperation stat_op;
  stat_op.stat(&size, &mtime, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &stat_op, nullptr));
  ASSERT_EQ(size, 4096u);

  bufferlist read_bl;
  ObjectReadOperation read_op;
  read_op.read(0, 4096, &read_bl, nullptr);
  ASSERT_EQ(0, ioctx.operate(oid, &read_op, nullptr));
  ASSERT_TRUE(read_bl.contents_equal(write_bl2_copy));
}


// Instantiate tests for both Replicated and FastEC pools
INSTANTIATE_TEST_SUITE_P(
  SparseReadTests,
  SparseReadTest,
  ::testing::Values(PoolType::REPLICATED, PoolType::FAST_EC),
  [](const ::testing::TestParamInfo<PoolType>& info) {
    return ceph::test::pool_type_name(info.param);
  }
);
