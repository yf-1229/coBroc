set(PICO_BOARD pico CACHE STRING "Board type")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

cmake_minimum_required(VERSION 3.31)
include(pico_sdk_import.cmake)
project(urban)
pico_sdk_init()

# add compiled files subdirectory
add_subdirectory(external/Config)
add_subdirectory(external/LCD)
add_subdirectory(external/Fonts)
add_subdirectory(external/GUI)
add_subdirectory(external/Infrared)


# add header file directory
include_directories(./external/Config)
include_directories(./external/GUI)
include_directories(./external/LCD)
include_directories(./external/Fonts)
include_directories(./external/Infrared)


# generate executable file
add_executable(codeS
        main.h
        main.cpp

)

# enable usb output, disable uart output
pico_enable_stdio_usb(codeS 1)
pico_enable_stdio_uart(codeS 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(codeS)

include(pico_sdk_import.cmake)
include(FetchContent)

# Add the standard library to the build
target_link_libraries(codeS PRIVATE
        pico_stdlib
        pico_aon_timer
        pico_multicore
        hardware_timer
        hardware_adc
        hardware_spi
        LCD
        GUI
        Fonts
        Config
        Infrared
)


# Add the standard include files to the build
target_include_directories(codeS PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)