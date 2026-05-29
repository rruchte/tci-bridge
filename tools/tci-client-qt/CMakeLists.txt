cmake_minimum_required(VERSION 3.20)

project(tci-client-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core WebSockets)

qt_standard_project_setup()

qt_add_executable(tci-client-qt
        main.cpp
        TciClient.h
        TciClient.cpp
        TciStreamFrame.h
        TciStreamFrame.cpp
)

target_link_libraries(tci-client-qt
        PRIVATE
        Qt6::Core
        Qt6::WebSockets
)