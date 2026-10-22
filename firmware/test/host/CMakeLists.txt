# Host-side unit tests for the protocol parser, using the Unity bundled with ESP-IDF:
#   cmake -S test/host -B test/host/build && cmake --build test/host/build && test/host/build/test_racebox
cmake_minimum_required(VERSION 3.22)
project(racebox_host_test C CXX)
set(CMAKE_CXX_STANDARD 17)

set(UNITY_DIR $ENV{IDF_PATH}/components/unity/unity/src)
add_executable(test_racebox
    test_racebox.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../components/racebox/racebox.cpp
    ${UNITY_DIR}/unity.c)
target_include_directories(test_racebox PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../components/racebox/include ${UNITY_DIR})
target_compile_options(test_racebox PRIVATE -Wall)
