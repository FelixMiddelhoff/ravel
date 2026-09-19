cmake_minimum_required(VERSION 3.20)
project(ravel_test_package LANGUAGES CXX)

find_package(ravel CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ravel::ravel)
target_compile_features(consumer PRIVATE cxx_std_20)
