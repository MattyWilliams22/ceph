add_executable(ceph_test_objectcacher_stress
  object_cacher_stress.cc
  FakeWriteback.cc
  MemWriteback.cc
  )
target_link_libraries(ceph_test_objectcacher_stress
  osdc
  global
  ${EXTRALIBS}
  ${CMAKE_DL_LIBS}
  )
install(TARGETS ceph_test_objectcacher_stress
  DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(ceph_test_objectcacher_misc
  object_cacher_misc.cc
  MemWriteback.cc
  )
target_link_libraries(ceph_test_objectcacher_misc
  osdc
  global
  ${EXTRALIBS}
  ${CMAKE_DL_LIBS}
  )
install(TARGETS ceph_test_objectcacher_misc
  DESTINATION ${CMAKE_INSTALL_BINDIR})

# unittest_split_ops_ut
# SplitOp.cc is compiled into libcommon (see src/CMakeLists.txt), which global
# pulls in, so this needs no further libraries.
add_executable(unittest_split_ops_ut
  test_splitop.cc
  )
add_ceph_unittest(unittest_split_ops_ut)
target_link_libraries(unittest_split_ops_ut global ${BLKID_LIBRARIES})

# unittest_sparse_read_reply
# Pure unit tests for CB_ObjectOperation_sparse_read (split vs legacy format,
# flag plumbing).  No cluster or I/O needed.
add_executable(unittest_sparse_read_reply
  test_sparse_read_reply.cc
  )
add_ceph_unittest(unittest_sparse_read_reply)
target_link_libraries(unittest_sparse_read_reply global ${BLKID_LIBRARIES})
