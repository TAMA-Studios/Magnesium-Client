cmake_minimum_required(VERSION 3.15)
project(sodium CXX)

set(CMAKE_CXX_STANDARD 17)

set(FETCHCONTENT_QUIET FALSE CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib
        GIT_TAG        master
        GIT_PROGRESS   TRUE
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(raylib)

find_package(CURL REQUIRED)


add_executable(sodium src/main.cpp)

target_include_directories(sodium PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${raylib_SOURCE_DIR}/src
)


target_link_libraries(sodium PRIVATE
        raylib
        CURL::libcurl
        GL m pthread dl rt X11
)
