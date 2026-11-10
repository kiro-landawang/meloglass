cmake_minimum_required(VERSION 3.22.1)
project(meloglass LANGUAGES C CXX)

add_library(meloglass SHARED
        meloglass.cpp
        cJSON.c)

target_include_directories(meloglass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(meloglass PRIVATE cxx_std_17)
target_compile_options(meloglass PRIVATE -O2 -fvisibility=hidden)
