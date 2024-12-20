cmake_minimum_required(VERSION 3.16)
project(HotBlock VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(HOTBLOCK_BUILD_TESTS "Build the Google Test suites" ON)
option(HOTBLOCK_BUILD_BENCH "Build the benchmark driver" ON)
set(HOTBLOCK_SANITIZE "" CACHE STRING "Sanitizer to enable: thread or address (empty = none)")

if(HOTBLOCK_SANITIZE)
  add_compile_options(-fsanitize=${HOTBLOCK_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${HOTBLOCK_SANITIZE})
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(hotblock
  src/block_device.cc
  src/block_pool.cc
)
target_include_directories(hotblock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hotblock PUBLIC Threads::Threads)
target_compile_options(hotblock PRIVATE -Wall -Wextra -Wpedantic)

if(HOTBLOCK_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(hotblock_tests
    tests/block_device_test.cc
    tests/block_pool_test.cc
    tests/lru_list_test.cc
  )
  target_link_libraries(hotblock_tests PRIVATE hotblock GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(hotblock_tests DISCOVERY_TIMEOUT 30)
endif()
