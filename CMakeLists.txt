cmake_minimum_required(VERSION 3.16)
project(pulselink CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

enable_testing()
add_subdirectory(test)
add_subdirectory(examples/part2-node-to-node)
add_subdirectory(examples/part3-pairing)
