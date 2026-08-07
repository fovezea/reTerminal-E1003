cmake_minimum_required(VERSION 3.16.0)

# Set the target chip BEFORE including the ESP-IDF project.cmake
set(ENV{IDF_TARGET} esp32s3)

# Extra component directories (BSP lives under components/)
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(reTerminal-E1003)
