cmake_minimum_required(VERSION 3.22)

#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Set the project name
set(CMAKE_PROJECT_NAME AT32F421G8U7_WorkBench)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
set(CMAKE_CXX_STANDARD 17)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add at32_workbench generated sources
add_subdirectory(cmake/at32_workbench)

# OTA 平台化核心库（纯 C 零芯片依赖，M1 起由 bootloader/app 链接）
add_subdirectory(ota)

# Bootloader target（独立链接脚本, FLASH 18KB 区）
add_subdirectory(bootloader)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths

)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here

    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_LPF.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_math.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_PID.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_PLL.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/SguanESC.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_printf.c
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_SWPWM.c 
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC/Sguan_MotorStatus.c
    ${CMAKE_CURRENT_LIST_DIR}/project/src/Timer.c
    ${CMAKE_CURRENT_LIST_DIR}/crash/crash.c
    ${CMAKE_CURRENT_LIST_DIR}/log/log.c
    ${CMAKE_CURRENT_LIST_DIR}/project/src/wk_tmr15.c
    ${CMAKE_CURRENT_LIST_DIR}/ota/port/at32/ota_app_hook.c
    ${CMAKE_CURRENT_LIST_DIR}/ota/port/at32/ota_flash_hal.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_LIST_DIR}/project/src
    ${CMAKE_CURRENT_LIST_DIR}/project/inc
    ${CMAKE_CURRENT_LIST_DIR}/SguanESC
    ${CMAKE_CURRENT_LIST_DIR}/crash
    ${CMAKE_CURRENT_LIST_DIR}/log
    ${CMAKE_CURRENT_LIST_DIR}/ota/core
    ${CMAKE_CURRENT_LIST_DIR}/ota/port/at32
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    at32_workbench

    # Add user defined libraries
    ota_core
)

# OTA: APP 固件 .bin 产物（flash.ps1 用 bin+显式地址烧录, 避免 openocd 对 elf
#   擦除起点向下对齐破坏 Bootloader 区 —— M2 实测: program app.elf 从 0x08004000
#   起擦, 覆盖 Bootloader 尾部 0x4000~0x47FF）
set(APP_ELF_PATH $<TARGET_FILE:${CMAKE_PROJECT_NAME}>)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${APP_ELF_PATH} ${CMAKE_PROJECT_NAME}.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
)
