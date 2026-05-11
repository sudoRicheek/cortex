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

# ---- libraries ------------------------------------------------------------

add_library(cortex_ros2_bridge_config SHARED
  src/config.cpp
)
target_include_directories(cortex_ros2_bridge_config PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(cortex_ros2_bridge_config PUBLIC yaml-cpp)

# ---- install --------------------------------------------------------------

install(DIRECTORY include/ DESTINATION include)

install(TARGETS cortex_ros2_bridge_config
  EXPORT export_cortex_ros2_bridge
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_export_include_directories(include)
ament_export_targets(export_cortex_ros2_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(yaml-cpp)

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
  ament_lint_auto_find_test_dependencies()

  ament_add_gtest(test_config test/test_config.cpp)
  target_link_libraries(test_config cortex_ros2_bridge_config)
endif()

ament_package()
