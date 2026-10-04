cmake_minimum_required(VERSION 3.16)
project(ros2_rt_middleware CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -O3 for production; -g for profiling with perf
add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic -g)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(rmw REQUIRED)

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

# ── latency_transport static library (shared impl for nodes + benchmark) ──────
add_library(latency_transport STATIC
  src/zero_copy_transport/latency_impl.cpp
)
ament_target_dependencies(latency_transport rclcpp std_msgs sensor_msgs)
target_include_directories(latency_transport PUBLIC
  ${RT_EXECUTOR_INC} ${ZC_TRANSPORT_INC}
)
target_link_libraries(latency_transport rt_executor)

# ── latency_publisher node ────────────────────────────────────────────────────
add_executable(latency_publisher
  src/zero_copy_transport/latency_publisher_node.cpp
)
ament_target_dependencies(latency_publisher rclcpp std_msgs sensor_msgs)
target_link_libraries(latency_publisher latency_transport rt)

# ── latency_subscriber node ───────────────────────────────────────────────────
add_executable(latency_subscriber
  src/zero_copy_transport/latency_subscriber_node.cpp
)
ament_target_dependencies(latency_subscriber rclcpp std_msgs sensor_msgs)
target_link_libraries(latency_subscriber latency_transport)

# ── benchmark_runner node ─────────────────────────────────────────────────────
add_executable(benchmark_runner
  src/benchmarks/benchmark_runner.cpp
)
ament_target_dependencies(benchmark_runner rclcpp std_msgs sensor_msgs)
target_include_directories(benchmark_runner PRIVATE ${BENCH_INC})
target_link_libraries(benchmark_runner latency_transport rt pthread)

# ── Install targets ───────────────────────────────────────────────────────────
install(TARGETS rt_executor latency_transport latency_publisher latency_subscriber benchmark_runner
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY ${RT_EXECUTOR_INC}/ DESTINATION include)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_ring_buffer test/test_ring_buffer.cpp)
  target_include_directories(test_ring_buffer PRIVATE ${RT_EXECUTOR_INC})

  ament_add_gtest(test_memory_pool test/test_memory_pool.cpp)
  target_include_directories(test_memory_pool PRIVATE ${RT_EXECUTOR_INC})

  ament_add_gtest(test_executor_ordering test/test_executor_ordering.cpp)
  target_include_directories(test_executor_ordering PRIVATE ${RT_EXECUTOR_INC})
  ament_target_dependencies(test_executor_ordering rclcpp)
  target_link_libraries(test_executor_ordering rt_executor pthread)

  ament_add_gtest(test_latency_end_to_end test/test_latency_end_to_end.cpp)
  target_include_directories(test_latency_end_to_end PRIVATE ${RT_EXECUTOR_INC} ${ZC_TRANSPORT_INC})
  ament_target_dependencies(test_latency_end_to_end rclcpp std_msgs sensor_msgs)
  target_link_libraries(test_latency_end_to_end latency_transport rt_executor pthread rt)
endif()

ament_package()
