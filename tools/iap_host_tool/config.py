"""
上位机配置：双 profile（AT32 / STM32）一键切换

v1.1 决策（方案文档 §4.5）：
- PROFILE_AT32   -> AT32F421G8U7 BLDC 电调（Harness_AT32 工程）
- PROFILE_STM32  -> STM32F103C8 IAP 老工程（STM32-OTA-QT）
- APP_PROFILE    一键切换，默认保持 STM32（双击即用，行为与源工程完全一致）

切换方法：改 APP_PROFILE 的值即可，例如 APP_PROFILE = "AT32"
"""

# ============================================================
# 双 profile
# ============================================================
PROFILES = {
    "AT32": {
        "label": "AT32F421G8U7 (BLDC 电调)",
        "baud": 115200,
        # —— Flash 布局（与 ota/core common.h 一致）——
        "packet_size": 128,          # PACKET_SIZE
        "app_start_address": 0x08004800,  # APP_START_ADDRESS
        "app_size": 44 * 1024,       # APP_SIZE (0xB000)
        "bootloader_size": 0x4800,   # BOOTLOADER_SIZE (18KB)
        # —— 升级触发模式 ——
        # bang5 : 发 5 连 '!!!!!'（APP ota_app_hook 需 5 连计数才触发停机链,
        #          Boot 窗口期 trigger 同样收 '!!!!!'）
        "trigger": "bang5",
    },
    "STM32": {
        "label": "STM32F103C8 (IAP 老工程)",
        "baud": 115200,
        # —— Flash 布局（与源工程 Common/common.h 一致）——
        "packet_size": 128,
        "app_start_address": 0x08004800,
        "app_size": 44 * 1024,
        "bootloader_size": 0x4800,
        # —— 升级触发模式 ——
        # single: 发单 '!' ×10（源工程 APP 侧 UART 中断扫描单 '!'）
        "trigger": "single",
    },
}

# 默认 profile（v1.1 决策：STM32）
APP_PROFILE = "STM32"


def get_profile():
    """返回当前激活的 profile 字典；不存在则抛错（防拼写错误静默回退）"""
    profile = PROFILES.get(APP_PROFILE)
    if profile is None:
        raise KeyError(
            f"config.py: APP_PROFILE='{APP_PROFILE}' 不在 PROFILES 中"
            f"（可选: {list(PROFILES.keys())}）")
    return profile


def profile_name():
    return APP_PROFILE
