cmake_minimum_required(VERSION 3.15)
project(loopme C)

set(CMAKE_C_STANDARD 11)

add_executable(loopme main.c)
target_link_libraries(loopme pulse)