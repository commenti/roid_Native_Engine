cmake_minimum_required(VERSION 3.22.1)
project("observer" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# CRITICAL BYTEHOOK BYPASS: Force disable all ByteHook dependency resolution
add_definitions(-DDISABLE_BYTEHOOK)

add_library(observer SHARED
    src/main.cpp
    src/ring_buffer.cpp
    src/dispatcher.cpp
    src/hooks_network.cpp
    src/hooks_crypto.cpp
)

target_include_directories(observer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_library(log-lib log)
target_link_libraries(observer PRIVATE ${log-lib})
