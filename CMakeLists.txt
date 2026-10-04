cmake_minimum_required(VERSION 3.16)
project(ros2_rt_middleware C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -march=native tunes for the build host. Turn it off for binaries that must
# run on other machines (CI artefacts, packages).
option(RTMW_NATIVE_ARCH "Compile with -march=native" ON)

# Warnings and tuning for this package's own targets only (not for the
# rosidl-generated code).
function(rtmw_target_options target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -g)
  if(RTMW_NATIVE_ARCH)
    target_compile_options(${target} PRIVATE -march=native)
  endif()
endfunction()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_default_generators REQUIRED)

# ── Messages ──────────────────────────────────────────────────────────────────
# Fixed-size message types so loaned (zero-copy) publishing is possible.
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/JointSample.msg"
  "msg/CloudSample.msg"
)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} rosidl_typesupport_cpp)

set(RT_EXECUTOR_INC src/rt_executor/include)
set(ZC_TRANSPORT_INC src/zero_copy_transport/include)
set(BENCH_INC src/benchmarks)

# ── rt_executor shared library ────────────────────────────────────────────────
add_library(rt_executor SHARED
  src/rt_executor/rt_executor.cpp
)
ament_target_dependencies(rt_executor rclcpp)
target_include_directories(rt_executor
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${RT_EXECUTOR_INC}>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(rt_executor pthread)
rtmw_target_options(rt_executor)

# ── latency_transport static library (shared impl for nodes + benchmark) ──────
add_library(latency_transport STATIC
  src/zero_copy_transport/csv_logger.cpp
  src/zero_copy_transport/shm_channel.cpp
  src/zero_copy_transport/latency_publisher.cpp
  src/zero_copy_transport/latency_subscriber.cpp
)
ament_target_dependencies(latency_transport rclcpp rmw)
target_include_directories(latency_transport PUBLIC
  ${RT_EXECUTOR_INC} ${ZC_TRANSPORT_INC}
)
target_link_libraries(latency_transport rt_executor "${cpp_typesupport_target}" rt)
rtmw_target_options(latency_transport)

# ── Executables ───────────────────────────────────────────────────────────────
foreach(exe latency_publisher latency_subscriber benchmark_runner)
  if(exe STREQUAL "benchmark_runner")
    add_executable(${exe} src/benchmarks/benchmark_runner.cpp)
  else()
    add_executable(${exe} src/zero_copy_transport/${exe}_node.cpp)
  endif()
  ament_target_dependencies(${exe} rclcpp rmw)
  target_include_directories(${exe} PRIVATE ${BENCH_INC})
  target_link_libraries(${exe} latency_transport pthread)
  rtmw_target_options(${exe})
endforeach()

# ── Install targets ───────────────────────────────────────────────────────────
install(TARGETS rt_executor latency_transport latency_publisher latency_subscriber benchmark_runner
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY ${RT_EXECUTOR_INC}/ DESTINATION include)
install(PROGRAMS scripts/plot_latency.py scripts/run_benchmarks.sh
  DESTINATION lib/${PROJECT_NAME}
)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_ring_buffer test/test_ring_buffer.cpp)
  target_include_directories(test_ring_buffer PRIVATE ${RT_EXECUTOR_INC})

  ament_add_gtest(test_memory_pool test/test_memory_pool.cpp)
  target_include_directories(test_memory_pool PRIVATE ${RT_EXECUTOR_INC})

  ament_add_gtest(test_executor_ordering test/test_executor_ordering.cpp)
  target_link_libraries(test_executor_ordering rt_executor pthread)

  ament_add_gtest(test_shm_channel test/test_shm_channel.cpp)
  target_link_libraries(test_shm_channel latency_transport pthread)

  ament_add_gtest(test_latency_end_to_end test/test_latency_end_to_end.cpp TIMEOUT 120)
  target_link_libraries(test_latency_end_to_end latency_transport pthread)
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
