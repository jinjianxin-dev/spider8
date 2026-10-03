# Spider8 固件工程（ESP32-S3-N16R8）
#
# 里程碑 M0-1：工程骨架最小可运行版本
# 构建：在 firmware 目录下执行  ..\tools\idf.ps1 build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spider8 VERSION 0.1.0)
