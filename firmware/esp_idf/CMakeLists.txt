# ESP32-H2 node firmware (ADR 0013). Build with ESP-IDF:
#   cd firmware/esp_idf && idf.py set-target esp32h2 build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(talos_node)
