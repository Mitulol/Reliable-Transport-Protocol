cmake_minimum_required(VERSION 3.12)
project(a3-reliable-transport)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Add the cpp folder as the actual project source
add_subdirectory(cpp)
