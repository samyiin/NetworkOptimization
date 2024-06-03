cmake_minimum_required(VERSION 3.26)
project(NetworkOptimization C)

set(CMAKE_C_STANDARD 11)

add_executable(NetworkOptimization main.c
        Benchmark/server.c
        Benchmark/server.h)
