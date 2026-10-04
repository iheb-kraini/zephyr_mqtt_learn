
cmake_minimum_required(VERSION 3.10.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)
FILE(GLOB app_sources src/*.c)

target_sources(app PRIVATE ${app_sources})
