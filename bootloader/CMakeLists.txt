# ============================================================
# Bootloader target: at32f421_boot
#
# 独立链接脚本 AT32F421x8_BOOT.ld（FLASH 18KB: 0x08000000 ~ 0x08004800）
# 依赖:
#   - ota_core 静态库（ota/core, 平台无关核心逻辑）
#   - ota/port/at32（Flash/USART/Delay/Printf/Jump 移植层, 无 key）
#   - AT32 标准外设库驱动（与 APP 同一套 libraries）
#   - 顶层 startup_at32f421.s（向量表在 0x08000000, 即 Bootloader 区）
# ============================================================

# 【关键】移除全局 -T（APP 链接脚本）——双 -T 会让两个 SECTIONS 合并,
#   导致 .init_array/.data 等段布局异常, printf 内部跑飞（M1 实测实锤）。
#   本目录 target 只链接 BOOT.ld。
string(REPLACE "-T \"${CMAKE_SOURCE_DIR}/AT32F421x8_APP.ld\"" ""
       CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS}")

set(BOOT_TARGET at32f421_boot)

add_executable(${BOOT_TARGET}
    ${CMAKE_SOURCE_DIR}/startup_at32f421.s
    ${CMAKE_SOURCE_DIR}/libraries/cmsis/cm4/device_support/system_at32f421.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_crm.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_gpio.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_usart.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_dma.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_flash.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_misc.c
    ${CMAKE_SOURCE_DIR}/libraries/drivers/src/at32f421_scfg.c
    ${CMAKE_SOURCE_DIR}/ota/port/at32/ota_flash_hal.c
    ${CMAKE_SOURCE_DIR}/ota/port/at32/ota_usart_hal.c
    ${CMAKE_SOURCE_DIR}/ota/port/at32/ota_delay.c
    ${CMAKE_SOURCE_DIR}/ota/port/at32/ota_printf.c
    ${CMAKE_SOURCE_DIR}/ota/port/at32/ota_jump.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/boot.c
)

# 链接 ota_core（ota/core 核心逻辑）
target_link_libraries(${BOOT_TARGET} PRIVATE ota_core)

# 头文件路径
target_include_directories(${BOOT_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/project/inc
    ${CMAKE_SOURCE_DIR}/libraries/drivers/inc
    ${CMAKE_SOURCE_DIR}/libraries/cmsis/cm4/core_support
    ${CMAKE_SOURCE_DIR}/libraries/cmsis/cm4/device_support
    ${CMAKE_SOURCE_DIR}/ota/core
    ${CMAKE_SOURCE_DIR}/ota/port/at32
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 编译宏（芯片定义）
target_compile_definitions(${BOOT_TARGET} PRIVATE
    USE_STDPERIPH_DRIVER
    AT32F421G8U7
)

# 编译优化: Boot 统一 -Os 压缩体积——全局 Debug 是 -O0, CI GCC 13.2.1 下
#   -O0 编译 Boot .text=20024B 溢出 18KB(1592B), 本地 13.3.1 -O0 为 17512B;
#   -Os 双版本均稳过(实测 13.3.1 -Os 后明显小于 18KB)
target_compile_options(${BOOT_TARGET} PRIVATE -Os)

# 链接脚本：BOOT.ld（后置 -T 覆盖工具链全局的 APP.ld MEMORY 定义）
# nosys.specs: 提供 _close/_lseek/_read/_sbrk/_fstat/_isatty 等空 syscalls
#   （_write 由 ota_printf.c 提供, 其余无实际使用）
target_link_options(${BOOT_TARGET} PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/AT32F421x8_BOOT.ld"
    --specs=nosys.specs
    -Wl,-Map=${BOOT_TARGET}.map
)

# 生成 .bin / .hex（烧录与上位机固件打包用）
# 产物路径用生成器表达式获取（target 在子目录, elf 位于 build/Debug/bootloader/）
set(BOOT_ELF_PATH $<TARGET_FILE:${BOOT_TARGET}>)
add_custom_command(TARGET ${BOOT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex ${BOOT_ELF_PATH} ${BOOT_TARGET}.hex
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating ${BOOT_TARGET}.hex"
)
add_custom_command(TARGET ${BOOT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${BOOT_ELF_PATH} ${BOOT_TARGET}.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating ${BOOT_TARGET}.bin"
)
add_custom_command(TARGET ${BOOT_TARGET} POST_BUILD
    COMMAND ${CMAKE_SIZE} ${BOOT_ELF_PATH}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Section sizes:"
)
