cmake_minimum_required(VERSION 3.15)
project(magnesium CXX)

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


add_executable(magnesium src/main.cpp)

include(CTest)
if (BUILD_TESTING)
    add_executable(chat_layout_test tests/chat_layout_test.cpp)
    target_include_directories(chat_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME chat_layout COMMAND chat_layout_test)
endif()

# Keep bundled fonts beside the executable, independent of the working directory.
add_custom_command(TARGET magnesium POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:magnesium>/assets"
        COMMENT "Copying Sodium React fonts"
)


target_include_directories(magnesium PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${raylib_SOURCE_DIR}/src
)


# Base links shared by all operating systems
target_link_libraries(magnesium PRIVATE
        raylib
        CURL::libcurl
)

# Platform-specific adjustments
if (APPLE)
    target_link_libraries(magnesium PRIVATE
            "-framework OpenGL"
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
else()
    target_link_libraries(magnesium PRIVATE
            GL m pthread dl rt X11
    )
endif()
