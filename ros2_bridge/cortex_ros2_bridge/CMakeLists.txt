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
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)

# cortex_wire_cpp — pure-CMake library, prefer installed package; fall back to
# in-tree add_subdirectory for monorepo dev (when this package is built before
# cortex_wire_cpp has been installed).
find_package(cortex_wire_cpp QUIET)
if(NOT cortex_wire_cpp_FOUND)
  set(_cwc_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../cortex_wire_cpp")
  if(EXISTS "${_cwc_dir}/CMakeLists.txt")
    message(STATUS "cortex_wire_cpp: using in-tree source at ${_cwc_dir}")
    set(CORTEX_WIRE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CORTEX_WIRE_INSTALL     OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_cwc_dir}" "${CMAKE_BINARY_DIR}/cortex_wire_cpp_external")
  else()
    message(FATAL_ERROR
      "cortex_wire_cpp not found via find_package and no in-tree copy at "
      "${_cwc_dir}. Install cortex_wire_cpp or fix the source layout.")
  endif()
endif()

# ---- libraries ------------------------------------------------------------

add_library(cortex_ros2_bridge_config SHARED
  src/config.cpp
)
target_include_directories(cortex_ros2_bridge_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_config PUBLIC yaml-cpp)

add_library(cortex_ros2_bridge_adapters SHARED
  src/registry.cpp
  src/adapters/primitives.cpp
)
target_include_directories(cortex_ros2_bridge_adapters PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_adapters PUBLIC
  cortex_ros2_bridge_config
  cortex_wire::cortex_wire
)
ament_target_dependencies(cortex_ros2_bridge_adapters PUBLIC
  std_msgs
  builtin_interfaces
)

# Components library — registered with rclcpp_components so the
# component_container_mt executable can load it as composable nodes.
add_library(cortex_ros2_bridge_components SHARED
  src/qos.cpp
  src/binding_factory.cpp
  src/cortex_to_ros2_node.cpp
  src/ros2_to_cortex_node.cpp
)
target_include_directories(cortex_ros2_bridge_components PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_components PUBLIC
  cortex_ros2_bridge_config
  cortex_ros2_bridge_adapters
  cortex_wire::cortex_wire
)
ament_target_dependencies(cortex_ros2_bridge_components PUBLIC
  rclcpp
  rclcpp_components
  std_msgs
  builtin_interfaces
)

rclcpp_components_register_node(cortex_ros2_bridge_components
  PLUGIN "cortex_ros2_bridge::CortexToRos2Bridge"
  EXECUTABLE cortex_to_ros2
)
rclcpp_components_register_node(cortex_ros2_bridge_components
  PLUGIN "cortex_ros2_bridge::Ros2ToCortexBridge"
  EXECUTABLE ros2_to_cortex
)

# ---- install --------------------------------------------------------------

install(DIRECTORY include/ DESTINATION include)

install(TARGETS cortex_ros2_bridge_config
  EXPORT export_cortex_ros2_bridge
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
# Internal-to-this-package libraries (not exported downstream because they
# transitively pin cortex_wire which is brought in via add_subdirectory).
install(TARGETS cortex_ros2_bridge_adapters cortex_ros2_bridge_components
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_export_include_directories(include)
ament_export_targets(export_cortex_ros2_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(yaml-cpp rclcpp rclcpp_components std_msgs builtin_interfaces)

# ---- tests ----------------------------------------------------------------

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)

  # Skip the opinionated style linters; keep cppcheck, lint_cmake, xmllint.
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  set(ament_cmake_uncrustify_FOUND TRUE)
  set(ament_cmake_pep257_FOUND TRUE)
  set(ament_cmake_flake8_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()

  ament_add_gtest(test_config test/test_config.cpp)
  target_link_libraries(test_config cortex_ros2_bridge_config)

  ament_add_gtest(test_registry test/test_registry.cpp)
  target_link_libraries(test_registry cortex_ros2_bridge_adapters)

  ament_add_gtest(test_primitives test/test_primitives.cpp)
  target_link_libraries(test_primitives cortex_ros2_bridge_adapters)

  ament_add_gtest(test_qos test/test_qos.cpp)
  target_link_libraries(test_qos cortex_ros2_bridge_components)

  ament_add_gtest(test_binding_factory test/test_binding_factory.cpp)
  target_link_libraries(test_binding_factory cortex_ros2_bridge_components)

  # End-to-end smoke test: spins up a mock discovery daemon + a Python-style
  # publisher / subscriber, exercises both components, and asserts delivery.
  ament_add_gtest(test_components_e2e test/test_components_e2e.cpp)
  target_link_libraries(test_components_e2e cortex_ros2_bridge_components)
endif()

ament_package()
