cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(waveshare_smoke)

target_sources(app PRIVATE src/main.c src/ws2812_gpio38.c)
