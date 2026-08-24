cmake_minimum_required(VERSION 3.10)
project(strategy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

add_executable(strategy main.cpp)
target_link_libraries(strategy nats Threads::Threads)

option(QUOTE_BUILD_TESTS "Build quoter unit tests" OFF)
if(QUOTE_BUILD_TESTS)
    enable_testing()
    add_executable(quote_logic_tests tests/test_quote_logic.cpp)
    target_link_libraries(quote_logic_tests Threads::Threads)
    add_test(NAME quote_logic_tests COMMAND quote_logic_tests)
endif()
