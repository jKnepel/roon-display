cmake_minimum_required(VERSION 3.20)
project(roon-display VERSION 0.1.0 LANGUAGES CXX)

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
target_link_libraries(roon-display PRIVATE roon_protocol)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol-tests tests/protocol_tests.cpp)
    target_link_libraries(protocol-tests PRIVATE roon_protocol)
    add_test(NAME protocol-tests COMMAND protocol-tests)
endif()

