cmake_minimum_required(VERSION 3.20)

project(ofe LANGUAGES CXX)

include(FetchContent)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build the test suite" ON)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG yaml-cpp-0.9.0
)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)

FetchContent_MakeAvailable(yaml-cpp googletest)

find_package(Threads REQUIRED)

add_library(ofe_headers INTERFACE)
target_include_directories(ofe_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(ofe_headers INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)

if(BUILD_TESTING)
  add_executable(ofe_tests
    test/engine_tests.cpp
  )

  target_link_libraries(ofe_tests PRIVATE ofe_headers GTest::gtest_main yaml-cpp::yaml-cpp Threads::Threads)

  target_compile_options(ofe_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
  )

  include(GoogleTest)
  gtest_discover_tests(ofe_tests)
endif()
