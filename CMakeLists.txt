cmake_minimum_required(VERSION 3.14)
project(kvspace-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── dependencies ─────────────────────────────────────────────────────────────

find_package(hiredis REQUIRED)

# ── library ──────────────────────────────────────────────────────────────────

add_library(kvspace STATIC
    src/xvalue.cpp
    src/redis.cpp
)

target_include_directories(kvspace PUBLIC include)
target_link_libraries(kvspace PUBLIC hiredis::hiredis)

# ── install ──────────────────────────────────────────────────────────────────

install(TARGETS kvspace ARCHIVE DESTINATION lib)
install(DIRECTORY include/kvspace DESTINATION include)
