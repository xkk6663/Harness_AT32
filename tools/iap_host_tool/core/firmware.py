"""
固件解析：支持 BIN 和 HEX
"""

import sys
from .protocol import APP_SIZE, APP_START_ADDRESS


def parse_firmware(filepath):
    """
    读取固件文件，通过hex和bin首行格式的不同自动识别 BIN/HEX。
    Hex： 以 : 开头的 Intel HEX 文件，提取 APP 区域（0x08004800 ~ 0x08004800+APP_SIZE）。
    Bin： 直接读取，填充到 APP_SIZE。
    返回 (binary_data, actual_size)。
    """
    with open(filepath, 'rb') as f:
        header = f.read(1)

    if header == b':':
        return _parse_hex(filepath)
    else:
        return _parse_bin(filepath)


def _parse_bin(filepath):
    """BIN：直接读取，填充到 APP_SIZE"""
    flash = bytearray(b'\xFF' * APP_SIZE)

    with open(filepath, 'rb') as f:
        data = f.read()

    if len(data) == 0:
        raise ValueError("固件文件为空")

    if len(data) > APP_SIZE:
        print(f"警告: 固件 ({len(data)} 字节) 超过 APP 区域 ({APP_SIZE} 字节)，截断")
        data = data[:APP_SIZE]

    flash[:len(data)] = data
    return flash, len(data)


def _parse_hex(filepath):
    """HEX：解析 Intel HEX，提取 APP 区域"""
    flash = bytearray(b'\xFF' * APP_SIZE)

    with open(filepath, 'r') as f:
        upper_addr = 0
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                continue

            byte_count = int(line[1:3], 16)
            address = int(line[3:7], 16)
            rec_type = int(line[7:9], 16)
            data_str = line[9:9 + byte_count * 2]

            if rec_type == 0x04:  # Extended Linear Address
                upper_addr = int(data_str, 16)
            elif rec_type == 0x00:  # Data Record
                actual_addr = (upper_addr << 16) + address
                app_start = APP_START_ADDRESS
                if app_start <= actual_addr < app_start + APP_SIZE:
                    offset = actual_addr - app_start
                    for i in range(byte_count):
                        flash[offset + i] = int(data_str[i * 2:i * 2 + 2], 16)

    # 找实际数据末尾
    last = APP_SIZE - 1
    while last >= 0 and flash[last] == 0xFF:
        last -= 1

    if last < 0:
        raise ValueError("HEX 文件中没有有效的 APP 数据")

    return flash, last + 1
