cmake_minimum_required(VERSION 3.10)

if(DEFINED ENV{CROSS_COMPILE})
    set(CMAKE_C_COMPILER "$ENV{CROSS_COMPILE}gcc")
endif()

project(smart_granary_detect_system VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(smart_granary_detect_system
    main.c
    app/app.c
    driver/LED.c
    driver/buzzer.c
    driver/delay.c
    driver/fan.c
    driver/ht_sensor.c
    driver/key.c
    driver/light_sensor.c
    driver/my_timer.c
    driver/voltage_sensor.c
)

target_include_directories(smart_granary_detect_system PRIVATE
    app
    driver
)

target_compile_options(smart_granary_detect_system PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(smart_granary_detect_system PRIVATE
    Threads::Threads
    m
    rt
)
