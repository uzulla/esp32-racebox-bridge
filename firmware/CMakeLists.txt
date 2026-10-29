cmake_minimum_required(VERSION 3.22)

# Variant: which frontend (display) is built in. The bridge in main/ is the same for all of them
# and only talks to the `frontend` component through components/bridge_status.
#   none  no display (default)
#   lcd   status display on the Waveshare ESP32-C6-GEEK's ST7789 LCD
# Select with -DRACEBOX_FRONTEND=lcd (mise run does this from VARIANT).
set(RACEBOX_FRONTEND "none" CACHE STRING "Frontend: none | lcd")
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/frontends/${RACEBOX_FRONTEND}/frontend)
    message(FATAL_ERROR "Unknown RACEBOX_FRONTEND '${RACEBOX_FRONTEND}'")
endif()
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/frontends/${RACEBOX_FRONTEND}/frontend)

# Variants with a microSD slot (c6-geek) log to it: -DRACEBOX_SDLOG=1. The LCD then shares the SPI bus
# that components/sdlog initialises.
if(RACEBOX_SDLOG)
    add_compile_definitions(RACEBOX_SDLOG=1)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(racebox-udp)
