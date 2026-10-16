cmake_minimum_required(VERSION 3.21)

project(qtwin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Bluetooth)
qt_standard_project_setup()

# Hardware-independent code: record parsing and UDDF export.
add_library(qtwin_core STATIC
    src/protocol.cpp
    src/protocol.h
    src/uddfwriter.cpp
    src/uddfwriter.h
)
target_include_directories(qtwin_core PUBLIC src)
target_link_libraries(qtwin_core PUBLIC Qt6::Core)
target_compile_definitions(qtwin_core PUBLIC QTWIN_VERSION="${PROJECT_VERSION}")

if(WIN32)
    set(QTWIN_TRANSPORT_SOURCES src/qtrfcommsocket.cpp src/qtrfcommsocket.h)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(QTWIN_TRANSPORT_SOURCES src/rfcommsocket.cpp src/rfcommsocket.h)
else()
    message(FATAL_ERROR "QTwin currently supports Linux and Windows only.")
endif()

qt_add_executable(qtwin
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/dtwinclient.cpp
    src/dtwinclient.h
    ${QTWIN_TRANSPORT_SOURCES}
)
target_link_libraries(qtwin PRIVATE qtwin_core Qt6::Widgets Qt6::Bluetooth)

qt_add_executable(qtwin-bt-probe src/qtbluetoothprobe.cpp)
target_link_libraries(qtwin-bt-probe PRIVATE Qt6::Core Qt6::Bluetooth)

option(QTWIN_BUILD_TESTS "Build unit tests" ON)
if(QTWIN_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    qt_add_executable(tst_protocol tests/tst_protocol.cpp)
    target_link_libraries(tst_protocol PRIVATE qtwin_core Qt6::Test)
    add_test(NAME tst_protocol COMMAND tst_protocol)

    # Compile the Windows adapter on Linux too to catch Qt API errors.
    if(NOT WIN32)
        add_library(qtwin_qt_transport_check OBJECT
            src/qtrfcommsocket.cpp src/qtrfcommsocket.h)
        target_link_libraries(qtwin_qt_transport_check PRIVATE Qt6::Bluetooth)
    endif()
endif()
