add_executable(df-coordinator coordinator_main.cpp)
target_link_libraries(df-coordinator PRIVATE decodefabric)
target_include_directories(df-coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(df-worker worker_main.cpp)
target_link_libraries(df-worker PRIVATE decodefabric)
target_include_directories(df-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
