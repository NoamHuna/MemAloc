cmake_minimum_required(VERSION 3.15)
project(allocator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -g -O0)

add_library(my_alloc STATIC src/my_malloc.c)
target_include_directories(my_alloc PUBLIC src)

add_executable(tests tests/tests.c)
target_link_libraries(tests PRIVATE my_alloc)
