# Top-level project CMake file for the FallSena DATA-LOGGING firmware.
#
# This is a SEPARATE ESP-IDF app from the main firmware in ../firmware.
# It records labeled IMU data (MPU6050 at 100 Hz, +/-8g / +/-1000dps) and
# flushes each trial to a host PC over TCP/WiFi for fall-detection tuning.
#
# Requires ESP-IDF (>= v5.x recommended for ESP32-C6 support).
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fallsena_datalog)
