cmake_minimum_required(VERSION 3.15)
project(TradingEngine VERSION 3.1 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -flto")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/strategies
    ${CMAKE_SOURCE_DIR}/src/market_data
)

# Core library
add_library(trading_core INTERFACE)
target_include_directories(trading_core INTERFACE
    ${CMAKE_SOURCE_DIR}/src/core
)

# Strategy library
add_library(trading_strategies INTERFACE)
target_include_directories(trading_strategies INTERFACE
    ${CMAKE_SOURCE_DIR}/src/strategies
)
target_link_libraries(trading_strategies INTERFACE trading_core)

# Main executable (if you create main.cpp)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
    add_executable(trading_engine src/main.cpp)
    target_link_libraries(trading_engine PRIVATE
        trading_core
        trading_strategies
        pthread
    )
endif()

# Tests (optional)
option(BUILD_TESTS "Build test suite" OFF)
if(BUILD_TESTS)
    enable_testing()
    
    # Add test executables here
    # add_executable(test_obi tests/test_obi.cpp)
    # target_link_libraries(test_obi trading_strategies)
    # add_test(NAME test_obi COMMAND test_obi)
endif()

# Installation
install(TARGETS trading_engine DESTINATION bin)
install(DIRECTORY src/ DESTINATION include/trading_engine
    FILES_MATCHING PATTERN "*.hpp"
)

# Print configuration
message(STATUS "Trading Engine v3.1 Configuration:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  Compiler flags: ${CMAKE_CXX_FLAGS}")
