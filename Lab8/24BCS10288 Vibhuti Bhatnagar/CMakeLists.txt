cmake_minimum_required(VERSION 3.10)
project(txn_manager_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(txn_manager_demo main.cpp)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(txn_manager_demo PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow)
endif()
