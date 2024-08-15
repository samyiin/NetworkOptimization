cmake_minimum_required(VERSION 3.20)
project(NetworkOptimization C)

set(CMAKE_C_STANDARD 99)

add_executable(Benchmark.client Benchmark/client.c)

add_executable(Benchmark.server Benchmark/server.c)

add_executable(ibv_pingpong Optimization/ibv_api.c Optimization/ibv_pingpong.c)

target_link_libraries(ibv_pingpong ibverbs)
