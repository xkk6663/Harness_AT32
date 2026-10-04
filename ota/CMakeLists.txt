# ============================================================
# OTA 平台化 — 核心库
#
# ota/core 为纯 C 零芯片依赖的 OTA 核心逻辑：
#   ota_crc32 / ota_transport / ota_protocol / ota_common
#   ota_flash_hal.h(接口) / ota_flash_store / ota_offset / ota_upgrade_state
#
# 移植层实现放 ota/port/<platform>/（M1 起按需加入）。
# ============================================================

file(GLOB OTA_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/core/*.c)

add_library(ota_core STATIC ${OTA_CORE_SOURCES})

target_include_directories(ota_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

# 核心库不依赖任何芯片头文件；-Wall 显式开以暴露移植期隐患
target_compile_options(ota_core PRIVATE -Wall -Wextra)
