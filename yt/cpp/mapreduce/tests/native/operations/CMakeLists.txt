# This file was generated by the build system used internally in Yandex and called "ya"
# (https://github.com/yandex/yatool).
#
# Configuration files of ya build system are usually named ya.make. If ya.make file is present
# at the root of the repository, then this repository supports ya build.
#
# If the repository supports both CMake and ya build configurations, please modify both of them.
#
# If only CMake build configuration is supported then modify only CMake files and note that only
# simple modifications are allowed like adding source-files to targets or adding simple properties
# like target_include_directories. These modifications will be ported to original ya.make files
# by maintainers. Any complex modifications which can't be easily ported back to the ya build
# system may be rejected.

find_package(linux-headers-generic REQUIRED)

add_executable(yt-cpp-mapreduce-tests-native-operations)

target_compile_options(yt-cpp-mapreduce-tests-native-operations PRIVATE
  -Wdeprecated-this-capture
)

target_link_libraries(yt-cpp-mapreduce-tests-native-operations PUBLIC
  linux-headers-generic::linux-headers-generic
  contrib-libs-cxxsupp
  yutil
  library-cpp-cpuid_check
  cpp-mapreduce-client
  cpp-mapreduce-common
  cpp-mapreduce-interface
  mapreduce-library-lazy_sort
  mapreduce-library-operation_tracker
  mapreduce-tests-yt_unittest_lib
  mapreduce-tests-gtest_main
  tests-native-proto_lib
  cpp-mapreduce-util
)

target_link_options(yt-cpp-mapreduce-tests-native-operations PRIVATE
  -ldl
  -lrt
  -Wl,--no-as-needed
  -fPIC
  -fPIC
  -lpthread
  -lrt
  -ldl
  -lutil
)

target_sources(yt-cpp-mapreduce-tests-native-operations PRIVATE
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/helpers.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/job_binary.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/jobs.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/operation_commands.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/operation_tracker.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/operation_watch.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/operations.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/prepare_operation.cpp
  ${PROJECT_SOURCE_DIR}/yt/cpp/mapreduce/tests/native/operations/raw_operations.cpp
)

set_property(
  TARGET
  yt-cpp-mapreduce-tests-native-operations
  PROPERTY
  SPLIT_FACTOR
  1
)

add_yunittest(
  NAME
  yt-cpp-mapreduce-tests-native-operations
  TEST_TARGET
  yt-cpp-mapreduce-tests-native-operations
  TEST_ARG
  --print-before-suite
  --print-before-test
  --fork-tests
  --print-times
  --show-fails
)

set_yunittest_property(
  TEST
  yt-cpp-mapreduce-tests-native-operations
  PROPERTY
  LABELS
  LARGE
)

set_yunittest_property(
  TEST
  yt-cpp-mapreduce-tests-native-operations
  PROPERTY
  PROCESSORS
  4
)

set_yunittest_property(
  TEST
  yt-cpp-mapreduce-tests-native-operations
  PROPERTY
  ENVIRONMENT
)

target_allocator(yt-cpp-mapreduce-tests-native-operations
  cpp-malloc-tcmalloc
  libs-tcmalloc-no_percpu_cache
)

vcs_info(yt-cpp-mapreduce-tests-native-operations)

