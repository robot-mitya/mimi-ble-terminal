cmake_minimum_required(VERSION 3.10)
project(BleTerminal)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDBUSPP REQUIRED sdbus-c++>=1)

include_directories(${SDBUSPP_INCLUDE_DIRS})
link_directories(${SDBUSPP_LIBRARY_DIRS})
add_definitions(${SDBUSPP_CFLAGS_OTHER})

add_executable(ble_terminal
        src/main.cpp
        src/ble_uart_client.cpp
)

target_link_libraries(ble_terminal ${SDBUSPP_LIBRARIES})
