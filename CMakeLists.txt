cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components.cmake")

if(IDF_TARGET STREQUAL "esp32p4" OR "$ENV{IDF_TARGET}" STREQUAL "esp32p4")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                           "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/boards/m5stack_tab5/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
if(IDF_TARGET STREQUAL "esp32p4")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4")
endif()
project(esp32coupe)

set(rom_image "${CMAKE_CURRENT_LIST_DIR}/roms/coupe.rom")
if(EXISTS "${rom_image}")
    file(SIZE "${rom_image}" rom_size)
    if(NOT rom_size EQUAL 32768)
        message(FATAL_ERROR "roms/coupe.rom must contain exactly 32768 bytes")
    endif()
    partition_table_get_partition_info(rom_offset "--partition-name coupe_rom" "offset")
    esptool_py_flash_target_image(flash "coupe_rom" "${rom_offset}" "${rom_image}" ALWAYS_PLAINTEXT)
endif()
