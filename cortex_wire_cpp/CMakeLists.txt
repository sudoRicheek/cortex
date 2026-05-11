cmake_minimum_required(VERSION 3.16)
project(cortex_wire_cpp VERSION 0.1.0 LANGUAGES CXX
  DESCRIPTION "C++ port of Cortex's IPC wire format (header, msgpack metadata, OOB frames, discovery)"
  HOMEPAGE_URL "https://github.com/sudoRicheek/cortex"
)

# ---- options --------------------------------------------------------------

option(CORTEX_WIRE_BUILD_TESTS "Build cortex_wire_cpp unit tests" ON)
option(CORTEX_WIRE_INSTALL     "Generate install rules for cortex_wire_cpp" ON)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- dependencies ---------------------------------------------------------

find_package(cppzmq REQUIRED)

# msgpack-cxx ships either as a CMake config (msgpack-cxxConfig.cmake on newer
# distros) or as a plain header set under libmsgpack-dev (Ubuntu 22.04). Try
# config first; fall back to a path lookup.
find_package(msgpack-cxx QUIET)
if(NOT msgpack-cxx_FOUND)
  find_path(MSGPACK_INCLUDE_DIR msgpack.hpp
    HINTS /usr/include /usr/local/include
    REQUIRED)
endif()

# ---- library --------------------------------------------------------------

add_library(cortex_wire
  src/header.cpp
  src/metadata.cpp
  src/discovery_client.cpp
)
add_library(cortex_wire::cortex_wire ALIAS cortex_wire)

target_include_directories(cortex_wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(msgpack-cxx_FOUND)
  target_link_libraries(cortex_wire PUBLIC msgpack-cxx)
else()
  # Header-only path: expose the include dir transitively, but do not enter
  # the install/export set (we'd need an imported target for that).
  target_include_directories(cortex_wire PUBLIC
    $<BUILD_INTERFACE:${MSGPACK_INCLUDE_DIR}>
  )
endif()
target_link_libraries(cortex_wire PUBLIC cppzmq)

set_target_properties(cortex_wire PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME cortex_wire
  # Allow linkage into downstream SHARED libraries (e.g. the ROS 2 bridge
  # adapter lib) even when we are built as a static archive.
  POSITION_INDEPENDENT_CODE ON
)

# ---- install --------------------------------------------------------------

if(CORTEX_WIRE_INSTALL)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(TARGETS cortex_wire
    EXPORT cortex_wire_cppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(PROGRAMS scripts/gen_fingerprint_table.py
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/cortex_wire_cpp/scripts
  )

  set(cortex_wire_cpp_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/cortex_wire_cpp)

  install(EXPORT cortex_wire_cppTargets
    FILE cortex_wire_cppTargets.cmake
    NAMESPACE cortex_wire::
    DESTINATION ${cortex_wire_cpp_cmake_dir}
  )

  configure_package_config_file(
    cmake/cortex_wire_cppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cortex_wire_cppConfig.cmake
    INSTALL_DESTINATION ${cortex_wire_cpp_cmake_dir}
  )
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cortex_wire_cppConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cortex_wire_cppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cortex_wire_cppConfigVersion.cmake
    DESTINATION ${cortex_wire_cpp_cmake_dir}
  )
endif()

# ---- tests ----------------------------------------------------------------

if(CORTEX_WIRE_BUILD_TESTS)
  enable_testing()
  # Prefer a system-installed GTest; fall back to FetchContent for environments
  # without one (CI runners, hermetic builds).
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  set(_cortex_wire_tests
    test_header
    test_metadata
    test_oob_buffer
    test_discovery_client
    test_fingerprint_table
  )
  foreach(t ${_cortex_wire_tests})
    add_executable(${t} test/${t}.cpp)
    target_link_libraries(${t} PRIVATE
      cortex_wire
      GTest::gtest
      GTest::gtest_main
    )
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
