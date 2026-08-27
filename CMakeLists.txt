cmake_minimum_required(VERSION 3.21)
project(DecodeFabric VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically-linked runtime so downstreams do not need a matching dynamic CRT.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_definitions(_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

# Strict diagnostics. /W4 /WX on MSVC. A single warning fails the build.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
else()
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror>)
endif()

option(DF_ENABLE_CUDA "Build the CUDA decode backend" ON)
option(DF_ENABLE_TESTS "Build and register tests" ON)
option(DF_ENABLE_EXAMPLES "Build examples" ON)
option(DF_ENABLE_BENCHMARKS "Build benchmarks" ON)
option(DF_BUILD_CLI "Build the DecodeFabric CLI" ON)
option(DF_BUILD_TOOLS "Build control-plane binaries" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DF_CUDA_FOUND OFF)
set(DF_CUDA_TOOLKIT_ROOT "")
if(DF_ENABLE_CUDA AND MSVC)
  set(_cuda_root "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1")
  if(NOT EXISTS "${_cuda_root}/bin/nvcc.exe")
    find_program(DF_NVCC nvcc)
    get_filename_component(_nvcc_dir "${DF_NVCC}" DIRECTORY)
    if(DF_NVCC)
      get_filename_component(_cuda_root "${_nvcc_dir}/.." ABSOLUTE)
    endif()
  endif()
  if(EXISTS "${_cuda_root}/bin/nvcc.exe")
    set(DF_CUDA_FOUND ON)
    set(DF_CUDA_TOOLKIT_ROOT "${_cuda_root}")
    set(CMAKE_CUDA_COMPILER "${_cuda_root}/bin/nvcc.exe" CACHE FILEPATH "" FORCE)
    set(CUDAToolkit_ROOT "${_cuda_root}" CACHE PATH "" FORCE)
    message(STATUS "DecodeFabric: using CUDA toolkit at ${_cuda_root}")
  else()
    message(STATUS "DecodeFabric: CUDA 13.1 nvcc not found; building CPU-only.")
  endif()
endif()

if(DF_CUDA_FOUND)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_ARCHITECTURES 120)
endif()

set(DF_SOURCES
  src/error.cpp
  src/core.cpp
  src/fabric.cpp
  src/cpu_executor.cpp
  src/protocol.cpp
  src/persistence.cpp
  src/transport.cpp
  src/distributed.cpp
)

if(DF_CUDA_FOUND)
  list(APPEND DF_SOURCES src/cuda_executor.cu)
endif()

add_library(decodefabric STATIC ${DF_SOURCES})
add_library(DecodeFabric::DecodeFabric ALIAS decodefabric)

target_include_directories(decodefabric PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(decodefabric PUBLIC cxx_std_20)

if(MSVC)
  target_link_libraries(decodefabric PRIVATE ws2_32)
  if(DF_CUDA_FOUND)
    target_link_libraries(decodefabric PUBLIC CUDA::cudart)
  endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS decodefabric EXPORT DecodeFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT DecodeFabricTargets
  FILE DecodeFabricTargets.cmake
  NAMESPACE DecodeFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DecodeFabric)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DecodeFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/DecodeFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DecodeFabric)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/DecodeFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/DecodeFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/DecodeFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DecodeFabric)

install(FILES README.md LICENSE NOTICE CONTRIBUTING.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(DF_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DF_ENABLE_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DF_ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(DF_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(DF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
