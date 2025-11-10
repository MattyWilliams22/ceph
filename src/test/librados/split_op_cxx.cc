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
  const std::string xattr_key = "xattr_key_1";
  const std::string xattr_value = "xattr_value_2";
  encode(omap_value, omap_val_bl);
  encode(xattr_value, xattr_val_bl);
  std::map<std::string, bufferlist> omap_map = {
    {omap_key_1, omap_val_bl},
    {omap_key_2, omap_val_bl}
  };
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


  // 3. Read from xattr before switching primary osd
  int err = 0;
  ObjectReadOperation read;
  bufferlist attr_read_bl;
  read.getxattr(xattr_key.c_str(), &attr_read_bl, &err);
  std::map<string, bufferlist> pattrs{ {"_", {}}, {xattr_key, {}}};
  read.getxattrs(&pattrs, &err);
  read.cmpxattr(xattr_key.c_str(), CEPH_OSD_CMPXATTR_OP_EQ, xattr_val_bl);
  int ret = ioctx.operate("error_inject_oid", &read, nullptr);
  EXPECT_TRUE(ret == 1);
  EXPECT_EQ(0, err);
  EXPECT_EQ(1U, pattrs.size());


  // 4. Find up osds
  bufferlist map_inbl, map_outbl;
  auto map_formatter = std::make_unique<JSONFormatter>(false);
  ceph::messaging::osd::OSDMapRequest osdMapRequest{pool_name, "error_inject_oid", nspace};
  encode_json("OSDMapRequest", osdMapRequest, map_formatter.get());

  std::ostringstream map_oss;
  map_formatter.get()->flush(map_oss);
  int rc = cluster.mon_command(map_oss.str(), map_inbl, &map_outbl, nullptr);
  EXPECT_TRUE(rc == 0);

  JSONParser p;
  bool success = p.parse(map_outbl.c_str(), map_outbl.length());
  EXPECT_TRUE(success);
  ceph::messaging::osd::OSDMapReply reply;
  reply.decode_json(&p);
  std::vector<int> prev_up_osds = reply.up;
  std::string pgid = reply.pgid;

  std::stringstream prev_out_vec;
  std::copy(prev_up_osds.begin(), prev_up_osds.end(), std::ostream_iterator<int>(prev_out_vec, " "));
  std::cout << "Previous up osds: " << prev_out_vec.str().c_str() << std::endl;
  

  // 5. Switch primary osd
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
  std::stringstream new_out_vec;
  std::copy(new_up_osds.begin(), new_up_osds.end(), std::ostream_iterator<int>(new_out_vec, " "));
  std::cout << "Desired up osds: " << new_out_vec.str().c_str() << std::endl;


  // 6. Set new up map
  bufferlist upmap_inbl, upmap_outbl;
  std::ostringstream upmap_oss;
  upmap_oss << "{\"prefix\": \"osd pg-upmap\", \"pgid\": \"" << pgid << "\", \"id\": [";
  for (size_t i = 0; i < new_up_osds.size(); i++) {
    upmap_oss << new_up_osds[i];
    if (i != new_up_osds.size() - 1) {
      upmap_oss << ", ";
    }
  }
  upmap_oss << "]}";
  rc = cluster.mon_command(upmap_oss.str(), upmap_inbl, &upmap_outbl, nullptr);
  EXPECT_TRUE(rc == 0);


  // 7. Wait for new upmap to show up
  bool upmap_in_effect = false;
  auto start_time = std::chrono::steady_clock::now();
  auto timeout = std::chrono::seconds(30);
  while (!upmap_in_effect && (std::chrono::steady_clock::now() - start_time < timeout)) {
    bufferlist check_inbl, check_outbl;
    auto check_formatter = std::make_unique<JSONFormatter>(false);
    ceph::messaging::osd::OSDMapRequest checkRequest{pool_name, "error_inject_oid", nspace};
    encode_json("OSDMapRequest", checkRequest, check_formatter.get());
    std::ostringstream check_oss;
    check_formatter.get()->flush(check_oss);
    int rc_check = cluster.mon_command(check_oss.str(), check_inbl, &check_outbl, nullptr);
    EXPECT_TRUE(rc_check == 0);
    
    JSONParser p_check;
    bool success_check = p_check.parse(check_outbl.c_str(), check_outbl.length());
    EXPECT_TRUE(success_check);
    ceph::messaging::osd::OSDMapReply reply_check;
    reply_check.decode_json(&p_check);
    std::vector<int> current_acting_osds = reply_check.acting;
    if (!current_acting_osds.empty() && current_acting_osds[0] == new_primary) {
      std::stringstream out_vec;
      std::copy(current_acting_osds.begin(), current_acting_osds.end(), std::ostream_iterator<int>(out_vec, " "));
      std::cout << "New acting osds: " << out_vec.str().c_str() << std::endl;
      upmap_in_effect = true;
    } else {
      std::this_thread::sleep_for(1s);
    }
  }
  EXPECT_TRUE(upmap_in_effect);
  

  // 8a. Read from omap
  std::map<std::string,bufferlist> vals{ {"_", {}} };
  read.omap_get_vals2("", LONG_MAX, &vals, nullptr, &err);
  EXPECT_TRUE(AssertOperateWithoutSplitOp(0, "error_inject_oid", &read, nullptr));
  EXPECT_EQ(0, err);
  EXPECT_EQ(2U, vals.size());
  EXPECT_NE(vals.find(omap_key_1), vals.end());
  bufferlist retrieved_val_bl = vals[omap_key_1];
  std::string retrieved_value;
  decode(retrieved_value, retrieved_val_bl);
  EXPECT_EQ(omap_value, retrieved_value);

  // 8b. Read from xattr after taking primary osd out
  bufferlist attr_read_bl_after;
  read.getxattr(xattr_key.c_str(), &attr_read_bl_after, &err);
  std::map<string, bufferlist> pattrs_after{ {"_", {}}, {xattr_key, {}}};
  read.getxattrs(&pattrs_after, &err);
  read.cmpxattr(xattr_key.c_str(), CEPH_OSD_CMPXATTR_OP_EQ, xattr_val_bl);
  ret = ioctx.operate("error_inject_oid", &read, nullptr);
  EXPECT_TRUE(ret == 1);
  EXPECT_EQ(0, err);
  EXPECT_EQ(1U, pattrs_after.size());


  // 9. Set osd_debug_reject_backfill_probability to 0.0
  cct->_conf->osd_debug_reject_backfill_probability = 0.0;


  // 10. Reset up map to previous values
  bufferlist resetmap_inbl, resetmap_outbl;
  std::ostringstream resetmap_oss;
  resetmap_oss << "{\"prefix\": \"osd pg-upmap\", \"pgid\": \"" << pgid << "\", \"id\": [";
  for (size_t i = 0; i < prev_up_osds.size(); i++) {
    resetmap_oss << prev_up_osds[i];
    if (i != prev_up_osds.size() - 1) {
      resetmap_oss << ", ";
    }
  }
  resetmap_oss << "]}";
  rc = cluster.mon_command(resetmap_oss.str(), resetmap_inbl, &resetmap_outbl, nullptr);
  EXPECT_TRUE(rc == 0);


  // 11. Wait for prev upmap to show up
  bool reset_in_effect = false;
  start_time = std::chrono::steady_clock::now();
  while (!reset_in_effect && (std::chrono::steady_clock::now() - start_time < timeout)) {
    bufferlist check_reset_inbl, check_reset_outbl;
    std::ostringstream check_reset_oss;
    auto check_reset_formatter = std::make_unique<JSONFormatter>(false);
    ceph::messaging::osd::OSDMapRequest checkResetRequest{pool_name, "error_inject_oid", nspace};
    encode_json("OSDMapRequest", checkResetRequest, check_reset_formatter.get());
    check_reset_formatter.get()->flush(check_reset_oss);
    int rc_reset = cluster.mon_command(check_reset_oss.str(), check_reset_inbl, &check_reset_outbl, nullptr);
    EXPECT_TRUE(rc_reset == 0);
    
    JSONParser p_check_reset;
    bool success_check_reset = p_check_reset.parse(check_reset_outbl.c_str(), check_reset_outbl.length());
    EXPECT_TRUE(success_check_reset);
    ceph::messaging::osd::OSDMapReply reply_check_reset;
    reply_check_reset.decode_json(&p_check_reset);
    std::vector<int> current_acting_osds = reply_check_reset.acting;
    if (!current_acting_osds.empty() && current_acting_osds[0] == prev_primary) {
      std::stringstream out_vec;
      std::copy(current_acting_osds.begin(), current_acting_osds.end(), std::ostream_iterator<int>(out_vec, " "));
      std::cout << "Final acting osds after cleanup: " << out_vec.str().c_str() << std::endl;
      reset_in_effect = true;
    } else {
      std::this_thread::sleep_for(1s);
    }
  }
  EXPECT_TRUE(reset_in_effect);
}

INSTANTIATE_TEST_SUITE_P_EC(LibRadosSplitOpECPP);
