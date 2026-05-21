# QTgame — 雷霆战机 + ESP32 JOY 串口摇杆；Qt Creator / CMake 构建
# 依赖：Qt 6 Widgets、SerialPort、Bluetooth（摇杆 BLE）
# 构建：在 Qt Creator 中打开本目录，或命令行：
#   cmake -B build -S . -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/msvc2019_64
#   cmake --build build

cmake_minimum_required(VERSION 3.16)
project(QTgame VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort Bluetooth)

qt_add_executable(QTgame
    main.cpp
    MainWindow.cpp
    MainWindow.h
    joystick_setup_widget.cpp
    joystick_setup_widget.h
    joystick_ble_client.cpp
    joystick_ble_client.h
    shared_joy_state.h
    thunder_fighter_window.cpp
    thunder_fighter_window.h
    tetris_window.cpp
    tetris_window.h
    game_2048_window.cpp
    game_2048_window.h
    xiangqi_engine.cpp
    xiangqi_engine.h
    xiangqi_window.cpp
    xiangqi_window.h
    chess_engine.cpp
    chess_engine.h
    chess_window.cpp
    chess_window.h
)

target_link_libraries(QTgame PRIVATE Qt6::Widgets Qt6::SerialPort Qt6::Bluetooth)

if(WIN32)
    set_target_properties(QTgame PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
