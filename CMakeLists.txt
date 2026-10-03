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
find_package(Threads REQUIRED)


add_library(session_store src/session_store.cpp src/session_codec.cpp)
target_include_directories(session_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(session_store PUBLIC CURL::libcurl Threads::Threads)
add_executable(magnesium src/main.cpp)
target_link_libraries(magnesium PRIVATE session_store)

include(CTest)
if (BUILD_TESTING)
    add_executable(chat_layout_test tests/chat_layout_test.cpp)
    target_include_directories(chat_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME chat_layout COMMAND chat_layout_test)
    add_executable(chat_history_test tests/chat_history_test.cpp)
    target_include_directories(chat_history_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME chat_history COMMAND chat_history_test)
    add_executable(client_memory_probe tests/client_memory_probe.cpp src/session_codec.cpp)
    target_include_directories(client_memory_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(client_memory_probe PRIVATE raylib CURL::libcurl Threads::Threads)
    add_custom_command(TARGET client_memory_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:client_memory_probe>/assets")
    add_executable(session_codec_test tests/session_codec_test.cpp)
    target_link_libraries(session_codec_test PRIVATE session_store)
    add_test(NAME session_codec COMMAND session_codec_test)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(session_flow_test tests/session_flow_test.cpp src/session_codec.cpp)
    target_include_directories(session_flow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(session_flow_test PRIVATE raylib CURL::libcurl Threads::Threads)
    add_test(NAME session_flow COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/session_fixture.py $<TARGET_FILE:session_flow_test>)
    add_executable(keyring_probe tests/keyring_probe.cpp)
    add_library(session_store_probe src/session_store.cpp src/session_codec.cpp)
    target_compile_definitions(session_store_probe PRIVATE MAGNESIUM_SESSION_SERVICE="us.tamastudios.magnesium.session.test")
    target_include_directories(session_store_probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(session_store_probe PUBLIC CURL::libcurl Threads::Threads)
    if(APPLE)
        target_link_libraries(session_store_probe PRIVATE "-framework Security" "-framework CoreFoundation")
    elseif(WIN32)
        target_link_libraries(session_store_probe PRIVATE Advapi32)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
        target_link_libraries(session_store_probe PRIVATE PkgConfig::LIBSECRET)
    endif()
    target_link_libraries(keyring_probe PRIVATE session_store_probe)
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
    target_link_libraries(session_store PRIVATE "-framework Security" "-framework CoreFoundation")
    target_link_libraries(magnesium PRIVATE
            "-framework OpenGL"
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
elseif(WIN32)
    target_link_libraries(session_store PRIVATE Advapi32)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    target_link_libraries(session_store PRIVATE PkgConfig::LIBSECRET)
    target_link_libraries(magnesium PRIVATE
            GL m pthread dl rt X11
    )
endif()

if(APPLE)
    set_target_properties(magnesium PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER us.tamastudios.magnesium
        MACOSX_BUNDLE_BUNDLE_NAME "Magnesium Client"
        MACOSX_BUNDLE_BUNDLE_VERSION 1.0.0
        MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0.0)
    install(TARGETS magnesium BUNDLE DESTINATION .)
else()
    install(TARGETS magnesium RUNTIME DESTINATION bin)
    install(DIRECTORY assets DESTINATION bin)
endif()
set(CPACK_PACKAGE_NAME Magnesium-Client)
set(CPACK_PACKAGE_VERSION 1.0.0)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "Magnesium-Client-1.0.0-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
