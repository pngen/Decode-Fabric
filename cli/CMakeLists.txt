add_executable(decodefabric-cli main.cpp)
target_link_libraries(decodefabric-cli PRIVATE decodefabric)
target_include_directories(decodefabric-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
