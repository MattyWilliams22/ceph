#include <common/perf_counters_collection.h>

#include "test/librados/test_cxx.h"
#include "test/librados/testcase_cxx.h"
#include "crimson_utils.h"
#include "cls/fifo/cls_fifo_ops.h"
#include "cls/version/cls_version_ops.h"
#include <fmt/format.h>
#include <json_spirit/json_spirit.h>

#include "common/ceph_json.h"
#include "common/JSONFormatter.h"
#include "common/json/OSDStructures.h"
#include "librados/librados_asio.h"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <climits>
#include <thread>
#include <chrono>

using namespace std;
using namespace librados;

typedef RadosTestPP LibRadosSplitOpPP;
typedef RadosTestECPP LibRadosSplitOpECPP;

// TEST_P(LibRadosSplitOpECPP, ReadWithVersion) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl;
//   bl.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "foo", &write1));

//   ObjectReadOperation read;
//   read.read(0, bl.length(), NULL, NULL);

//   bufferlist exec_inbl, exec_outbl;
//   int exec_rval;
//   read.exec("version", "read", exec_inbl, &exec_outbl, &exec_rval);
//   ASSERT_TRUE(AssertOperateWithSplitOp(0, "foo", &read, &bl));
//   ASSERT_EQ(0, memcmp(bl.c_str(), "ceph", 4));
//   ASSERT_EQ(0, exec_rval);
//   cls_version_read_ret exec_version;
//   auto iter = exec_outbl.cbegin();
//   decode(exec_version, iter);
//   ASSERT_EQ(0, exec_version.objv.ver);
//   ASSERT_EQ("", exec_version.objv.tag);
// }

// TEST_P(LibRadosSplitOpECPP, SmallRead) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl;
//   bl.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "foo", &write1));

//   ioctx.set_no_objver(true);
//   ObjectReadOperation read;
//   read.read(0, bl.length(), NULL, NULL);
//   ASSERT_TRUE(AssertOperateWithSplitOp(0, "foo", &read, &bl));
//   ioctx.set_no_objver(true);
// }

// TEST_P(LibRadosSplitOpECPP, ReadWithIllegalClsOp) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl;
//   bl.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "foo", &write1));

//   bufferlist new_bl;
//   new_bl.append("CEPH");
//   ObjectWriteOperation write2;
//   bufferlist exec_inbl, exec_outbl;
//   int exec_rval;
//   rados::cls::fifo::op::init_part op;
//   encode(op, exec_inbl);
//   write2.exec("fifo", "init_part", exec_inbl, &exec_outbl, &exec_rval);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(-EOPNOTSUPP, "foo", &write2));
// }

// TEST_P(LibRadosSplitOpECPP, XattrReads) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl, attr_bl, attr_read_bl;
//   std::string attr_key = "xattr_key";
//   std::string attr_value = "xattr_value";

//   bl.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl);
//   encode(attr_value, attr_bl);
//   write1.setxattr(attr_key.c_str(), attr_bl);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "xattr_oid_pumpkin", &write1));

//   ObjectReadOperation read;
//   read.read(0, bl.length(), NULL, NULL);

//   int getxattr_rval, getxattrs_rval;
//   read.getxattr(attr_key.c_str(), &attr_read_bl, &getxattr_rval);
//   std::map<string, bufferlist> pattrs{ {"_", {}}, {attr_key, {}}};
//   read.getxattrs(&pattrs, &getxattrs_rval);
//   read.cmpxattr(attr_key.c_str(), CEPH_OSD_CMPXATTR_OP_EQ, attr_bl);

//   ASSERT_TRUE(AssertOperateWithSplitOp(1, "xattr_oid_pumpkin", &read, &bl));
//   ASSERT_EQ(0, memcmp(bl.c_str(), "ceph", 4));
//   ASSERT_EQ(0, getxattr_rval);
//   ASSERT_EQ(0, getxattrs_rval);
//   ASSERT_EQ(1U, pattrs.size());
// }

// TEST_P(LibRadosSplitOpECPP, Stat) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl, attr_bl, attr_read_bl;
//   std::string attr_key = "my_key";
//   std::string attr_value = "my_attr";

//   bl.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl);
//   encode(attr_value, attr_bl);
//   write1.setxattr(attr_key.c_str(), attr_bl);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "foo", &write1));

//   ObjectReadOperation read;
//   read.read(0, bl.length(), NULL, NULL);

//   uint64_t size;
//   timespec time;
//   time.tv_nsec = 0;
//   time.tv_sec = 0;
//   int stat_rval;
//   read.stat2(&size, &time, &stat_rval);

//   ASSERT_TRUE(AssertOperateWithSplitOp(0, "foo", &read, &bl));
//   ASSERT_EQ(0, memcmp(bl.c_str(), "ceph", 4));
//   ASSERT_EQ(0, stat_rval);
//   ASSERT_EQ(4, size);
//   ASSERT_NE(0, time.tv_nsec);
//   ASSERT_NE(0, time.tv_sec);
// }

// TEST_P(LibRadosSplitOpECPP, OMAPReads) {
//   SKIP_IF_CRIMSON();
//   bufferlist bl_write, omap_val_bl, omap_header_bl;
//   const std::string omap_key_1 = "omap_key_1_elephant";
//   const std::string omap_key_2 = "omap_key_2_fox";
//   const std::string omap_key_3 = "omap_key_3_squirrel";
//   const std::string omap_value = "omap_value_1_giraffe";
//   const std::string omap_header = "this is the omap header";
  
//   encode(omap_value, omap_val_bl);
//   encode(omap_header, omap_header_bl);
  
//   std::map<std::string, bufferlist> omap_map = {
//     {omap_key_1, omap_val_bl},
//     {omap_key_2, omap_val_bl},
//     {omap_key_3, omap_val_bl}
//   };

//   bl_write.append("ceph");
//   ObjectWriteOperation write1;
//   write1.write(0, bl_write);
//   write1.omap_set_header(omap_header_bl);
//   write1.omap_set(omap_map);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &write1));

//   int err = 0;
//   bufferlist bl_read;
//   ObjectReadOperation read;

  
//   read.read(0, bl_write.length(), &bl_read, nullptr);
//   std::map<std::string,bufferlist> vals{ {"_", {}}, {omap_key_1, {}}};
//   read.omap_get_vals2("", LONG_MAX, &vals, nullptr, &err);
//   ASSERT_TRUE(AssertOperateWithSplitOp(0, "omap_oid_axolotl", &read, nullptr));
//   ASSERT_EQ(0, err);
//   ASSERT_EQ(0, memcmp(bl_read.c_str(), "ceph", 4));
//   ASSERT_EQ(3U, vals.size());
//   ASSERT_NE(vals.find(omap_key_1), vals.end());
//   bufferlist retrieved_val_bl = vals[omap_key_1];
//   std::string retrieved_value;
//   decode(retrieved_value, retrieved_val_bl);
//   ASSERT_EQ(omap_value, retrieved_value);

//   bufferlist omap_header_read_bl;
//   std::set<std::string> keys;
//   read.omap_get_keys2("", LONG_MAX, &keys, nullptr, &err);
//   read.omap_get_header(&omap_header_read_bl, &err);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &read, nullptr));
//   ASSERT_EQ(0, err);
//   std::string omap_header_read;
//   decode(omap_header_read, omap_header_read_bl);
//   ASSERT_EQ(omap_header, omap_header_read);
//   ASSERT_EQ(3U, keys.size());
  
//   std::map<std::string,bufferlist> vals_by_keys{ {"_", {}} };
//   std::set<std::string> key_filter = {omap_key_1, omap_key_2};
//   read.omap_get_vals_by_keys(key_filter, &vals_by_keys, &err);
//   std::map<std::string, std::pair<bufferlist, int> > assertions;
//   assertions[omap_key_3] = make_pair(omap_val_bl, CEPH_OSD_CMPXATTR_OP_EQ);
//   read.omap_cmp(assertions, &err);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &read, nullptr));
//   ASSERT_EQ(0, err);
//   ASSERT_EQ(2U, vals_by_keys.size());

//   std::set<std::string> keys_to_remove = {omap_key_2};
//   write1.omap_rm_keys(keys_to_remove);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &write1));
//   read.omap_get_vals2("", LONG_MAX, &vals, nullptr, &err);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &read, nullptr));
//   ASSERT_EQ(0, err);
//   ASSERT_EQ(2U, vals.size());

//   write1.omap_clear();
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &write1));
//   read.omap_get_vals2("", LONG_MAX, &vals, nullptr, &err);
//   ASSERT_TRUE(AssertOperateWithoutSplitOp(0, "omap_oid_axolotl", &read, nullptr));
//   ASSERT_EQ(0, err);
//   ASSERT_EQ(0U, vals.size());

//   // omap_rmkeyrange has not been tested
// }

TEST_P(LibRadosSplitOpECPP, ErrorInject) {
  SKIP_IF_CRIMSON();
  bufferlist bl_write, omap_val_bl, xattr_val_bl;
  const std::string omap_key_1 = "key_a";
  const std::string omap_key_2 = "key_b";
  const std::string omap_value = "val_c";
  encode(omap_value, omap_val_bl);
  std::map<std::string, bufferlist> omap_map = {
    {omap_key_1, omap_val_bl},
    {omap_key_2, omap_val_bl}
  };
  const std::string xattr_key = "xattr_key_1";
  const std::string xattr_value = "xattr_value_2";
  encode(xattr_value, xattr_val_bl);
  bl_write.append("ceph");
  
  // 1a. Write data to omap
  ObjectWriteOperation write1;
  write1.write(0, bl_write);
  write1.omap_set(omap_map);
  EXPECT_TRUE(AssertOperateWithoutSplitOp(0, "error_inject_oid", &write1));

  // 1b. Write data to xattrs
  write1.setxattr(xattr_key.c_str(), xattr_val_bl);
  EXPECT_TRUE(AssertOperateWithoutSplitOp(0, "error_inject_oid", &write1));

  // 2. Set osd_debug_reject_backfill_probability to 1.0
  CephContext* cct = static_cast<CephContext*>(cluster.cct());
  cct->_conf->osd_debug_reject_backfill_probability = 1.0;

  // 3. Read xattrs before switching primary osd
  read_xattrs("error_inject_oid", xattr_key, xattr_value, 1, 1, 0);

  // 4. Find up osds
  ceph::messaging::osd::OSDMapReply reply;
  int res = request_osd_map(pool_name, "error_inject_oid", nspace, &reply);
  EXPECT_TRUE(res == 0);
  std::vector<int> prev_up_osds = reply.up;
  std::string pgid = reply.pgid;
  print_osd_map("Previous up osds: ", prev_up_osds);
  
  // 5. Find unused osd to be new primary
  int prev_primary = prev_up_osds[0];
  int new_primary = 0;
  while (true) {
    auto it = std::find(prev_up_osds.begin(), prev_up_osds.end(), new_primary);
    if (it == prev_up_osds.end()) {
      break;
    }
    new_primary++;
  }
  std::vector<int> new_up_osds = prev_up_osds;
  new_up_osds[0] = new_primary;
  std::cout << "Previous primary osd: " << prev_primary << std::endl;
  std::cout << "New primary osd: " << new_primary << std::endl;
  print_osd_map("Desired up osds: ", new_up_osds);

  // 6. Set new up map
  int rc = set_osd_upmap(pgid, new_up_osds);
  EXPECT_TRUE(rc == 0);

  // 7. Wait for new upmap to appear as acting set of osds
  int res2 = wait_for_upmap(pool_name, "error_inject_oid", nspace, new_primary, 30s);
  EXPECT_TRUE(res2 == 0);
  
  // 8a. Read omap
  read_omap("error_inject_oid", omap_key_1, omap_value, 2, 0);

  // 8b. Read xattrs after switching primary osd
  read_xattrs("error_inject_oid", xattr_key, xattr_value, 1, 1, 0);

  // 9. Set osd_debug_reject_backfill_probability to 0.0
  cct->_conf->osd_debug_reject_backfill_probability = 0.0;

  // 10. Reset up map to previous values
  int rc2 = set_osd_upmap(pgid, prev_up_osds);
  EXPECT_TRUE(rc2 == 0);

  // 11. Wait for prev upmap to appear as acting set of osds
  int res3 = wait_for_upmap(pool_name, "error_inject_oid", nspace, prev_primary, 30s);
  EXPECT_TRUE(res3 == 0);
}

INSTANTIATE_TEST_SUITE_P_EC(LibRadosSplitOpECPP);
