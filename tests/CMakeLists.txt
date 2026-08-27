# Test targets. Each is its own executable; CTest registers them all.
add_executable(smoke_test test_main.cpp smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE decodefabric)
target_include_directories(smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit_smoke COMMAND smoke_test)

add_executable(batching_test test_main.cpp batching_test.cpp)
target_link_libraries(batching_test PRIVATE decodefabric)
target_include_directories(batching_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit_batching COMMAND batching_test)

add_executable(protocol_test test_main.cpp protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE decodefabric)
target_include_directories(protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit_protocol COMMAND protocol_test)

add_executable(authority_test test_main.cpp authority_test.cpp)
target_link_libraries(authority_test PRIVATE decodefabric)
target_include_directories(authority_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit_authority COMMAND authority_test)

add_executable(distributed_smoke test_main.cpp distributed_smoke.cpp)
target_link_libraries(distributed_smoke PRIVATE decodefabric)
target_include_directories(distributed_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mp_smoke COMMAND distributed_smoke)

add_executable(mp_probe mp_probe.cpp)
target_link_libraries(mp_probe PRIVATE decodefabric)
target_include_directories(mp_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
