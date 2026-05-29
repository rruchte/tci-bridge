cmake_minimum_required(VERSION 3.20)

project(tci-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core WebSockets Network Multimedia)
find_package(yaml-cpp REQUIRED)

qt_standard_project_setup()

add_executable(tci-bridge
        src/main.cpp
        src/Version.h
        src/AppConfig.h
        src/AppConfig.cpp
        src/AudioBackend.h
        src/QtAudioBackend.h
        src/QtAudioBackend.cpp
        src/RadioBackend.h
        src/NullRadioBackend.h
        src/NullRadioBackend.cpp
        src/RigctldBackend.h
        src/RigctldBackend.cpp
        src/RigctldWorker.h
        src/RigctldWorker.cpp
        src/SignalHandler.h
        src/SignalHandler.cpp
        src/TciServer.h
        src/TciServer.cpp
        src/TciCommand.h
        src/TciCommand.cpp
        src/TciStreamFrame.h
        src/TciStreamFrame.cpp
        src/PcmConvert.h
        src/PcmConvert.cpp
)

target_link_libraries(tci-bridge
        PRIVATE
        Qt6::Core
        Qt6::WebSockets
        Qt6::Network
        Qt6::Multimedia
        yaml-cpp::yaml-cpp
)

include(GNUInstallDirs)

install(TARGETS tci-bridge
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
        examples/tci-bridge.yml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tci-bridge/examples
)

install(FILES
        examples/systemd/tci-bridge.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tci-bridge/examples/systemd
)

install(FILES
        README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

install(FILES
        examples/INSTALL-systemd.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
)