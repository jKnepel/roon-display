cmake_minimum_required(VERSION 3.20)
project(roon-display VERSION 0.1.0 LANGUAGES CXX)

option(ROON_DISPLAY_FLATPAK "Install only the self-contained desktop app" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(roon_protocol
    src/display_state.cpp
    src/moo.cpp
    src/sood.cpp
    src/websocket.cpp
    src/roon_client.cpp
)
target_include_directories(roon_protocol PUBLIC include)
target_compile_options(roon_protocol PRIVATE -Wall -Wextra -Wpedantic)

add_executable(roon-display src/main.cpp)
add_executable(state-replay src/state_replay.cpp)
add_executable(live-preview src/live_preview.cpp)
target_link_libraries(live-preview PRIVATE roon_protocol)

target_link_libraries(state-replay PRIVATE roon_protocol)

target_link_libraries(roon-display PRIVATE roon_protocol)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol-tests tests/protocol_tests.cpp)
    target_link_libraries(protocol-tests PRIVATE roon_protocol)
    add_test(NAME protocol-tests COMMAND protocol-tests)
endif()


option(ROON_DISPLAY_BUILD_DESKTOP "Build GTK/WebKitGTK desktop app when dependencies are available" OFF)
if(ROON_DISPLAY_BUILD_DESKTOP)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK_WEBKIT QUIET gtk+-3.0 webkit2gtk-4.0)
        if(NOT GTK_WEBKIT_FOUND)
            pkg_check_modules(GTK_WEBKIT QUIET gtk+-3.0 webkit2gtk-4.1)
        endif()
    endif()
    if(GTK_WEBKIT_FOUND)
        add_executable(roon-display-desktop src/desktop_app.cpp)
        target_compile_options(roon-display-desktop PRIVATE ${GTK_WEBKIT_CFLAGS_OTHER})
        target_include_directories(roon-display-desktop PRIVATE ${GTK_WEBKIT_INCLUDE_DIRS})
        target_link_directories(roon-display-desktop PRIVATE ${GTK_WEBKIT_LIBRARY_DIRS})
        target_link_libraries(roon-display-desktop PRIVATE roon_protocol ${GTK_WEBKIT_LIBRARIES})
        install(TARGETS roon-display-desktop RUNTIME DESTINATION bin)
        message(STATUS "desktop app enabled (GTK/WebKitGTK)")
    else()
        message(STATUS "desktop app disabled: GTK3/WebKitGTK development packages not found")
    endif()
endif()

if(NOT ROON_DISPLAY_FLATPAK)
    install(TARGETS roon-display live-preview state-replay
        RUNTIME DESTINATION bin)
endif()
install(DIRECTORY ui DESTINATION share/roon-display)
