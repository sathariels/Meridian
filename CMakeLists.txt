cmake_minimum_required(VERSION 3.20)
project(meridian LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Opt-in sanitizer builds, e.g.:
#   cmake -S . -B build-tsan -DMERIDIAN_SANITIZE=thread
# ThreadSanitizer is the tool that actually proves the striping is race-free;
# the hammer test alone can pass by luck.
set(MERIDIAN_SANITIZE "" CACHE STRING "Sanitizer: thread, address, or empty")
if(MERIDIAN_SANITIZE)
    add_compile_options(-fsanitize=${MERIDIAN_SANITIZE} -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${MERIDIAN_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(meridian_cache
    src/cache/lru_cache.cpp
    src/cache/striped_cache.cpp
)
target_include_directories(meridian_cache PUBLIC src)
target_link_libraries(meridian_cache PUBLIC Threads::Threads)

add_executable(load_gen bench/load_gen.cpp)
target_link_libraries(load_gen PRIVATE meridian_cache)

enable_testing()

add_executable(lru_cache_test tests/lru_cache_test.cpp)
target_link_libraries(lru_cache_test PRIVATE meridian_cache)
add_test(NAME lru_cache_test COMMAND lru_cache_test)

add_executable(striped_cache_test tests/striped_cache_test.cpp)
target_link_libraries(striped_cache_test PRIVATE meridian_cache)
add_test(NAME striped_cache_test COMMAND striped_cache_test)
