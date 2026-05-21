# ESP-IDF 工程入口（与 MicroPython 的 led_on.py 无关：那是固件烧录后在解释器里跑的脚本）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_project)
