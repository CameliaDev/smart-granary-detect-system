cmake_minimum_required(VERSION 3.10)

if(DEFINED ENV{CROSS_COMPILE})
    set(CMAKE_C_COMPILER "$ENV{CROSS_COMPILE}gcc")
endif()

project(smart_granary_detect_system VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)
find_package(CURL)

add_executable(smart_granary_detect_system
    main.c
    app/app.c
    app/state.c
    app/alarm.c
    app/display.c
    app/log.c
    app/snapshot.c
    app/multimodal.c
    app/http_server.c
    driver/LED.c
    driver/buzzer.c
    driver/delay.c
    driver/fan.c
    driver/ht_sensor.c
    driver/key.c
    driver/light_sensor.c
    driver/voltage_sensor.c
    driver/pir_sensor.c
    driver/flame_sensor.c
    driver/camera.c
)

# 说明：driver/my_timer.c 当前未被使用（线程节拍由各线程 delay 完成），
# 不参与编译；文件保留供后续需要 POSIX 定时器时启用。

target_include_directories(smart_granary_detect_system PRIVATE
    app
    driver
)

target_compile_options(smart_granary_detect_system PRIVATE
    -Wall
    -Wextra
)

target_link_libraries(smart_granary_detect_system PRIVATE
    Threads::Threads
    m
    rt
)

if(CURL_FOUND)
    target_compile_definitions(smart_granary_detect_system PRIVATE HAVE_LIBCURL=1)
    target_include_directories(smart_granary_detect_system PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(smart_granary_detect_system PRIVATE CURL::libcurl)
    message(STATUS "libcurl found: multimodal HTTP API enabled")
else()
    message(STATUS "libcurl NOT found: multimodal API will use stub analysis")
endif()

# Optional board-specific GPIO paths, e.g.:
# cmake -DPIR_SENSOR_SYSFS_PATH=\"/sys/class/gpio/gpio115/value\" ..
if(DEFINED PIR_SENSOR_SYSFS_PATH)
    target_compile_definitions(smart_granary_detect_system PRIVATE
        PIR_SENSOR_SYSFS_PATH="${PIR_SENSOR_SYSFS_PATH}")
endif()
if(DEFINED FLAME_SENSOR_SYSFS_PATH)
    target_compile_definitions(smart_granary_detect_system PRIVATE
        FLAME_SENSOR_SYSFS_PATH="${FLAME_SENSOR_SYSFS_PATH}")
endif()
