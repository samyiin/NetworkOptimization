cmake_minimum_required(VERSION 3.20)
project(NetworkOptimization C)

set(CMAKE_C_STANDARD 99)

add_executable(Benchmark.client Benchmark/client.c
        LowLatencyProtocols/ib_functions.c
        LowLatencyProtocols/ib_functions.h
        LowLatencyProtocols/main.c)

add_executable(Benchmark.server Benchmark/server.c)

# Not really important in part two
add_executable(server Optimization/bw_template.c)

target_link_libraries(server ibverbs)

# Part Three
add_executable(LowLatencyProtocols.server LowLatencyProtocols/ib_functions.c LowLatencyProtocols/main.c
        LowLatencyProtocols/kv_api.c)
target_link_libraries(LowLatencyProtocols.server ibverbs)


#add_executable(LowLatencyProtocols.client LowLatencyProtocols/ib_functions.c LowLatencyProtocols/main.c
#        LowLatencyProtocols/kv_api.c
#        LowLatencyProtocols/kv_api.h)