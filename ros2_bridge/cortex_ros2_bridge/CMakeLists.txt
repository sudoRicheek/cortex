cmake_minimum_required(VERSION 3.16)
project(cortex_ros2_bridge VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(cppzmq REQUIRED)
# msgpack-cxx is header-only. We avoid CMake imported-target propagation
# (which would force msgpack-cxx into our export set on every distro) and
# just locate its include path. Downstream targets get the headers via our
# include directory export.
find_path(MSGPACK_INCLUDE_DIR msgpack.hpp
  HINTS /usr/include /usr/local/include
  REQUIRED)

# ---- libraries ------------------------------------------------------------

add_library(cortex_ros2_bridge_config SHARED
  src/config.cpp
)
target_include_directories(cortex_ros2_bridge_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_config PUBLIC yaml-cpp)

add_library(cortex_ros2_bridge_wire SHARED
  src/cortex_wire/header.cpp
  src/cortex_wire/metadata.cpp
  src/cortex_wire/discovery_client.cpp
)
target_include_directories(cortex_ros2_bridge_wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${MSGPACK_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_wire PUBLIC
  cppzmq
)

# ---- install --------------------------------------------------------------

install(DIRECTORY include/ DESTINATION include)

install(TARGETS cortex_ros2_bridge_config cortex_ros2_bridge_wire
  EXPORT export_cortex_ros2_bridge
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})
install(PROGRAMS scripts/gen_fingerprint_table.py
  DESTINATION lib/${PROJECT_NAME}/scripts)

ament_export_include_directories(include)
ament_export_targets(export_cortex_ros2_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(yaml-cpp cppzmq)

# ---- tests ----------------------------------------------------------------

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)

  # Skip the opinionated style linters; keep the substantive ones (cppcheck,
  # lint_cmake, xmllint). Style is enforced by clang-format separately if/when
  # we add one.
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  set(ament_cmake_uncrustify_FOUND TRUE)
  set(ament_cmake_pep257_FOUND TRUE)
  set(ament_cmake_flake8_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()

  ament_add_gtest(test_config test/test_config.cpp)
  target_link_libraries(test_config cortex_ros2_bridge_config)

  ament_add_gtest(test_wire_header test/test_wire_header.cpp)
  target_link_libraries(test_wire_header cortex_ros2_bridge_wire)

  ament_add_gtest(test_wire_metadata test/test_wire_metadata.cpp)
  target_link_libraries(test_wire_metadata cortex_ros2_bridge_wire)

  ament_add_gtest(test_oob_buffer test/test_oob_buffer.cpp)
  target_link_libraries(test_oob_buffer cortex_ros2_bridge_wire)

  ament_add_gtest(test_discovery_client test/test_discovery_client.cpp)
  target_link_libraries(test_discovery_client cortex_ros2_bridge_wire)

  ament_add_gtest(test_fingerprint_table test/test_fingerprint_table.cpp)
  target_link_libraries(test_fingerprint_table cortex_ros2_bridge_wire)
endif()

ament_package()
