// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "include/rados/librados.hpp"
#include "test/librados/test_cxx.h"
#include "gtest/gtest.h"

namespace ceph {
namespace test {

// Pool type enumeration for parameterized tests
enum class PoolType {
  REPLICATED,
  FAST_EC,
  LEGACY_EC
};

// Convert pool type to string for test naming
inline std::string pool_type_name(PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return "Replicated";
    case PoolType::FAST_EC:
      return "FastEC";
    case PoolType::LEGACY_EC:
      return "LegacyEC";
    default:
      return "Unknown";
  }
}

// Create pool based on type
inline std::string create_pool_by_type(
    const std::string& pool_name,
    librados::Rados& cluster,
    PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return create_pool_pp(pool_name, cluster);
    case PoolType::FAST_EC: {
      std::string result = create_ec_pool_pp(pool_name, cluster, true, true);
      if (result != "") {
        return result;
      }
      result = set_allow_ec_overwrites_pp(pool_name, cluster, true);
      cluster.wait_for_latest_osdmap();
      return result;
    }
    case PoolType::LEGACY_EC:
      return create_ec_pool_pp(pool_name, cluster, false, false);
    default:
      return "Unknown pool type";
  }
}

// Destroy pool based on type
inline int destroy_pool_by_type(
    const std::string& pool_name,
    librados::Rados& cluster,
    PoolType type) {
  switch (type) {
    case PoolType::REPLICATED:
      return destroy_pool_pp(pool_name, cluster);
    case PoolType::FAST_EC:
    case PoolType::LEGACY_EC:
      return destroy_ec_pool_pp(pool_name, cluster);
    default:
      return -EINVAL;
  }
}

// Generic base class for parameterized pool type tests
// Can be used for any test that needs to run on multiple pool types
class PoolTypeTestFixture : public ::testing::TestWithParam<PoolType> {
 protected:
  static librados::Rados rados;
  static std::map<PoolType, std::string> pool_names;
  librados::IoCtx ioctx;
  std::string pool_name;
  std::string nspace;
  PoolType pool_type;

  static std::vector<PoolType> get_supported_pool_types() {
    return {PoolType::REPLICATED, PoolType::FAST_EC};
  }

  static std::string pool_name_prefix() {
    return "pool_type_test_";
  }

  static void after_pool_create(PoolType type,
                                const std::string& pool_name,
                                librados::Rados& cluster) {
  }

  static void cleanup_namespace(librados::Rados& cluster,
                                librados::IoCtx& ioctx,
                                const std::string& ns) {
    ioctx.snap_set_read(librados::SNAP_HEAD);
    ioctx.set_namespace(ns);

    int tries = 20;
    while (--tries) {
      int got_enoent = 0;
      for (librados::NObjectIterator it = ioctx.nobjects_begin();
           it != ioctx.nobjects_end(); ++it) {
        ioctx.locator_set_key(it->get_locator());
        librados::ObjectWriteOperation op;
        op.remove();
        librados::AioCompletion* completion = cluster.aio_create_completion();
        int ret = ioctx.aio_operate(it->get_oid(), completion, &op,
                                    librados::OPERATION_IGNORE_CACHE);
        if (ret == 0) {
          completion->wait_for_complete();
          if (completion->get_return_value() == -ENOENT) {
            ++got_enoent;
          } else {
            ASSERT_EQ(0, completion->get_return_value());
          }
        }
        completion->release();
      }
      if (!got_enoent) {
        break;
      }
      sleep(1);
    }
  }

  static void SetUpTestSuite() {
    ASSERT_EQ("", connect_cluster_pp(rados));

    for (auto type : get_supported_pool_types()) {
      std::string pname = get_temp_pool_name(
        pool_name_prefix() + pool_type_name(type) + "_");
      ASSERT_EQ("", create_pool_by_type(pname, rados, type));
      after_pool_create(type, pname, rados);
      pool_names[type] = pname;
    }
  }

  static void TearDownTestSuite() {
    for (auto& [type, pname] : pool_names) {
      ASSERT_EQ(0, destroy_pool_by_type(pname, rados, type));
    }
    pool_names.clear();
    rados.shutdown();
  }

  void SetUp() override {
    pool_type = GetParam();
    auto it = pool_names.find(pool_type);
    ASSERT_NE(pool_names.end(), it);
    pool_name = it->second;
    ASSERT_EQ(0, rados.ioctx_create(pool_name.c_str(), ioctx));

    auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    nspace = std::string(test_info->name()) + "_" +
             std::to_string(std::chrono::system_clock::now()
                              .time_since_epoch().count());
    ioctx.set_namespace(nspace);

    cleanup_namespace(rados, ioctx, nspace);
  }

  void TearDown() override {
    cleanup_namespace(rados, ioctx, "");
    cleanup_namespace(rados, ioctx, nspace);
    ioctx.close();
  }
};

// Base class for EC-only tests
class ECOnlyTestFixture : public ::testing::Test {
 protected:
  static librados::Rados rados;
  librados::IoCtx ioctx;
  std::string pool_name;

  void SetUp() override {
    pool_name = get_temp_pool_name();
    ASSERT_EQ("", create_pool_by_type(pool_name, rados, PoolType::FAST_EC));
    ASSERT_EQ(0, rados.ioctx_create(pool_name.c_str(), ioctx));
  }
  
  void TearDown() override {
    ioctx.close();
    ASSERT_EQ(0, destroy_pool_by_type(pool_name, rados, PoolType::FAST_EC));
  }
};

} // namespace test
} // namespace ceph
