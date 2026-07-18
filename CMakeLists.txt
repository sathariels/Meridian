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

# Event loop backend is chosen per platform behind the same interface.
if(APPLE)
    set(MERIDIAN_EVENT_LOOP_SRC src/net/kqueue_event_loop.cpp)
else()
    # epoll_event_loop.cpp lands in phase 8 (Linux/Docker validation).
    set(MERIDIAN_EVENT_LOOP_SRC src/net/epoll_event_loop.cpp)
endif()

add_library(meridian_cluster
    src/cluster/hash_ring.cpp
    src/cluster/shard_router.cpp
)
target_include_directories(meridian_cluster PUBLIC src)
target_link_libraries(meridian_cluster PUBLIC meridian_cache)

add_library(meridian_leaderboard
    src/leaderboard/leaderboard.cpp
)
target_include_directories(meridian_leaderboard PUBLIC src)

add_library(meridian_net
    ${MERIDIAN_EVENT_LOOP_SRC}
    src/net/tcp_server.cpp
    src/replication/replica_link.cpp
    src/replication/wal.cpp
    src/server/command_handler.cpp
)
target_include_directories(meridian_net PUBLIC src)
target_link_libraries(meridian_net PUBLIC meridian_cache meridian_cluster
                                          meridian_leaderboard)

add_executable(meridian_server src/server/main.cpp)
target_link_libraries(meridian_server PRIVATE meridian_net)

add_executable(load_gen bench/load_gen.cpp)
target_link_libraries(load_gen PRIVATE meridian_cache)

enable_testing()

add_executable(lru_cache_test tests/lru_cache_test.cpp)
target_link_libraries(lru_cache_test PRIVATE meridian_cache)
add_test(NAME lru_cache_test COMMAND lru_cache_test)

add_executable(striped_cache_test tests/striped_cache_test.cpp)
target_link_libraries(striped_cache_test PRIVATE meridian_cache)
add_test(NAME striped_cache_test COMMAND striped_cache_test)

add_executable(command_handler_test tests/command_handler_test.cpp)
target_link_libraries(command_handler_test PRIVATE meridian_net)
add_test(NAME command_handler_test COMMAND command_handler_test)

add_executable(tcp_server_test tests/tcp_server_test.cpp)
target_link_libraries(tcp_server_test PRIVATE meridian_net)
add_test(NAME tcp_server_test COMMAND tcp_server_test)

add_executable(hash_ring_test tests/hash_ring_test.cpp)
target_link_libraries(hash_ring_test PRIVATE meridian_cluster)
add_test(NAME hash_ring_test COMMAND hash_ring_test)

add_executable(shard_router_test tests/shard_router_test.cpp)
target_link_libraries(shard_router_test PRIVATE meridian_net)
add_test(NAME shard_router_test COMMAND shard_router_test)

add_executable(leaderboard_test tests/leaderboard_test.cpp)
target_link_libraries(leaderboard_test PRIVATE meridian_net)
add_test(NAME leaderboard_test COMMAND leaderboard_test)

add_executable(replication_test tests/replication_test.cpp)
target_link_libraries(replication_test PRIVATE meridian_net)
add_test(NAME replication_test COMMAND replication_test)
