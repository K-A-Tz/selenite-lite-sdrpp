cmake_minimum_required(VERSION 3.13)
project(selenite_source LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if(WIN32)
    target_link_libraries(selenite_source PRIVATE ole32 uuid avrt)
endif()