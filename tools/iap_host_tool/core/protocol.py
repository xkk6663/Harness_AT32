"""
IAP 协议层：常量、CRC32、帧构建 / 解析

v1.1（M3）：Flash 布局常量改为从 config.py 双 profile 读取（PROFILE_AT32 /
PROFILE_STM32），默认 STM32 行为与源工程完全一致。
"""

import struct

from config import get_profile

# ============================================================
# 帧常量（协议帧格式，跨平台不变）
# ============================================================
FRAME_SOF = 0xAA
FRAME_EOF = 0x55

FRAME_TYPE_DATA = 0x01
FRAME_TYPE_CMD = 0x02

# ============================================================
# Flash 布局常量（来自 config.py 激活的 profile）
# ============================================================
_PROFILE = get_profile()
PACKET_SIZE = _PROFILE["packet_size"]
APP_START_ADDRESS = _PROFILE["app_start_address"]
APP_SIZE = _PROFILE["app_size"]
BOOTLOADER_SIZE = _PROFILE["bootloader_size"]

# 命令字
CMD_QUERY_OFFSET = 0x12
CMD_RESET_UPGRADE = 0x13
CMD_QUERY_STATE = 0x14

# 应答字
RSP_ACK = 0x20
RSP_NAK = 0x21
RSP_OFFSET = 0x22
RSP_STATE = 0x23

# ============================================================
# CRC32（标准 CRC-32）
# ============================================================
CRC32_POLY = 0xEDB88320


def crc32_start():
    return 0xFFFFFFFF


def crc32_update(crc, data):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ CRC32_POLY
            else:
                crc >>= 1
    return crc


def crc32_finish(crc):
    return crc ^ 0xFFFFFFFF


def crc32_calc(data):
    return crc32_finish(crc32_update(crc32_start(), data))


# ============================================================
# 帧构建
# ============================================================


def build_data_frame(chunk, seq=0):
    """数据帧: AA 01 SEQ LEN DATA[0~128] CRC32(4B,LE) 55"""
    frame = bytearray()
    frame.append(FRAME_SOF)
    frame.append(FRAME_TYPE_DATA)
    frame.append(seq & 0xFF)
    frame.append(len(chunk))
    frame.extend(chunk)
    crc = crc32_calc(bytes([FRAME_TYPE_DATA, seq & 0xFF, len(chunk)]) + chunk)
    frame.extend(struct.pack('<I', crc))
    frame.append(FRAME_EOF)
    return frame


def build_end_frame():
    """升级结束标记: AA 01 00 00 CRC32 55  (SEQ=0, LEN=0)"""
    frame = bytearray()
    frame.append(FRAME_SOF)
    frame.append(FRAME_TYPE_DATA)
    frame.append(0)  # SEQ = 0
    frame.append(0)  # LEN = 0
    crc = crc32_calc(bytes([FRAME_TYPE_DATA, 0, 0]))
    frame.extend(struct.pack('<I', crc))
    frame.append(FRAME_EOF)
    return frame


def build_cmd_frame(cmd, params=b""):
    """命令帧: AA 02 CMD [PARAM...] CRC32(4B,LE) 55"""
    frame = bytearray()
    frame.append(FRAME_SOF)
    frame.append(FRAME_TYPE_CMD)
    frame.append(cmd)
    frame.extend(params)
    crc = crc32_calc(bytes([FRAME_TYPE_CMD, cmd]) + params)
    frame.extend(struct.pack('<I', crc))
    frame.append(FRAME_EOF)
    return frame


# ============================================================
# 应答帧解析
# ============================================================


def parse_response(data):
    """
    解析应答帧（不含 SOF/EOF 的原始数据） — 含 PARAM_LEN 字段。
    返回 (cmd, params) 或 (None, None) 无效。

    帧结构: [TYPE=0x02, CMD, PARAM_LEN, PARAM..., CRC32(4B)]
    """
    if len(data) < 7:  # TYPE + CMD + PARAM_LEN + CRC32(4B)
        return None, None
    if data[0] != FRAME_TYPE_CMD:
        return None, None

    cmd = data[1]
    param_len = data[2]
    payload = data[3:3 + param_len]
    crc_recv = struct.unpack('<I', bytes(data[3 + param_len:3 + param_len + 4]))[0]
    crc_calc = crc32_calc(bytes([FRAME_TYPE_CMD, cmd, param_len]) + payload)
    if crc_calc != crc_recv:
        return None, None
    return cmd, payload


def precalc_firmware_crc(data, actual_size):
    """预计算完整固件的最终 CRC"""
    crc = crc32_start()
    offset = 0
    while offset < actual_size:
        chunk = data[offset:offset + PACKET_SIZE]
        if len(chunk) < PACKET_SIZE:
            chunk = chunk + b'\xFF' * (PACKET_SIZE - len(chunk))
        crc = crc32_update(crc, chunk)
        offset += PACKET_SIZE
    return crc32_finish(crc)
