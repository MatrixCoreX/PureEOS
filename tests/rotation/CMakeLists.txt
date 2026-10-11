cmake_minimum_required(VERSION 3.16)
project(pureeos_rotation_tests LANGUAGES CXX)

# Host-only historical queue/reference-model tests and the shared growth formula.
# Production rotation v2's paginated tables/penalties require the native WASM
# integration suite; passing this target alone does not validate that adapter.
include(CTest)
option(ROTATION_ENABLE_SANITIZERS "Enable address/undefined behavior sanitizers" OFF)
find_path(ROTATION_BOOST_INCLUDE_DIR boost/test/included/unit_test.hpp)
if(ROTATION_BOOST_INCLUDE_DIR)
  set(ROTATION_BOOST_INCLUDE_DIRS ${ROTATION_BOOST_INCLUDE_DIR})
else()
  # PureEOS vendors modular Boost headers. Reuse them without configuring or
  # compiling the full chain; a separately checked out Boost also works here.
  set(ROTATION_BOOST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../../PureEOS/libraries/boost"
      CACHE PATH "Modular Boost source tree for host-only rotation tests")
  if(NOT EXISTS "${ROTATION_BOOST_SOURCE}/libs/test/include/boost/test/included/unit_test.hpp")
    message(FATAL_ERROR "Set ROTATION_BOOST_INCLUDE_DIR to installed Boost headers, or ROTATION_BOOST_SOURCE to a Boost source tree")
  endif()
  file(GLOB ROTATION_BOOST_INCLUDE_DIRS
    "${ROTATION_BOOST_SOURCE}/libs/*/include"
    "${ROTATION_BOOST_SOURCE}/libs/numeric/*/include")
endif()

add_executable(pureeos_rotation_tests
  ../pureeos_rotation_tests.cpp
  ../../contracts/eosio.system/src/rotation.cpp)
target_compile_features(pureeos_rotation_tests PRIVATE cxx_std_17)
target_compile_definitions(pureeos_rotation_tests PRIVATE PUREEOS_ROTATION_STANDALONE)
target_include_directories(pureeos_rotation_tests PRIVATE
  ../../contracts/eosio.system/include)
target_include_directories(pureeos_rotation_tests SYSTEM PRIVATE ${ROTATION_BOOST_INCLUDE_DIRS})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pureeos_rotation_tests PRIVATE -Wall -Wextra -Werror)
  if(ROTATION_ENABLE_SANITIZERS)
    target_compile_options(pureeos_rotation_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pureeos_rotation_tests PRIVATE -fsanitize=address,undefined)
  endif()
endif()

add_test(NAME pureeos_rotation_model COMMAND pureeos_rotation_tests --report_level=detailed)
