cmake_minimum_required(VERSION 3.16)

# wave 1: restrict the build to main and its dependencies; components/gfx and
# components/epd_uc8179 are being written concurrently and are integrated in
# wave 2 (remove this line then)
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eink_weather)
