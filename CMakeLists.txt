cmake_minimum_required(VERSION 3.16)
project(distributed_timeseries_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# -mavx2 enables the AVX2 scan path; the engine also checks CPUID at runtime.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" HAS_AVX2_FLAG)
if(HAS_AVX2_FLAG)
  add_compile_options(-mavx2)
endif()

find_package(Threads REQUIRED)

# ---- Header-only core engine ------------------------------------------------
add_library(tsdb_core INTERFACE)
target_include_directories(tsdb_core INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tsdb_core INTERFACE Threads::Threads)

# ---- gRPC / protobuf code generation ---------------------------------------
option(BUILD_GRPC "Build the gRPC node server and router" ON)
if(BUILD_GRPC)
  find_package(Protobuf REQUIRED)
  find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_package(PkgConfig REQUIRED)
  # pkg-config pulls in the full gRPC dependency chain (gpr, absl, etc.).
  pkg_check_modules(GRPCPP REQUIRED grpc++)

  set(PROTO ${CMAKE_SOURCE_DIR}/proto/tsdb.proto)
  set(GEN ${CMAKE_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${GEN})
  set(PB_SRCS ${GEN}/tsdb.pb.cc ${GEN}/tsdb.grpc.pb.cc)
  set(PB_HDRS ${GEN}/tsdb.pb.h ${GEN}/tsdb.grpc.pb.h)
  add_custom_command(
    OUTPUT ${PB_SRCS} ${PB_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_SOURCE_DIR}/proto
            --cpp_out=${GEN} --grpc_out=${GEN}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN} ${PROTO}
    DEPENDS ${PROTO}
    COMMENT "Generating gRPC/protobuf sources")

  add_library(tsdb_proto STATIC ${PB_SRCS})
  target_include_directories(tsdb_proto PUBLIC ${GEN} ${Protobuf_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS})
  target_link_directories(tsdb_proto PUBLIC ${GRPCPP_LIBRARY_DIRS})
  target_link_libraries(tsdb_proto PUBLIC ${GRPCPP_LIBRARIES} ${Protobuf_LIBRARIES})

  add_executable(tsdb_node src/server_main.cpp)
  target_link_libraries(tsdb_node PRIVATE tsdb_core tsdb_proto)

  add_executable(tsdb_router src/router_main.cpp)
  target_link_libraries(tsdb_router PRIVATE tsdb_core tsdb_proto)

  add_executable(loadgen src/loadgen_main.cpp)
  target_link_libraries(loadgen PRIVATE tsdb_core tsdb_proto)
endif()

# ---- Benchmarks (no external deps) -----------------------------------------
foreach(b bench_ingest bench_compression bench_query bench_simd)
  add_executable(${b} bench/${b}.cpp)
  target_link_libraries(${b} PRIVATE tsdb_core)
endforeach()

# ---- Tests ------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  find_package(GTest)
  if(GTest_FOUND)
    enable_testing()
    foreach(t test_gorilla test_engine test_consistent_hash)
      add_executable(${t} test/${t}.cpp)
      target_link_libraries(${t} PRIVATE tsdb_core GTest::gtest GTest::gtest_main)
      add_test(NAME ${t} COMMAND ${t})
    endforeach()
  else()
    message(STATUS "GTest not found; skipping unit tests")
  endif()
endif()
