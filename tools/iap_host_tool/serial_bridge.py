# -*- coding: utf-8 -*-
"""serial_bridge.py — 串口双线代理（根治 OTA 与监视互斥）

背景: Windows 串口同一时刻只允许一个进程独占打开, 服务端 .NET 监视器与
OTA 工具(cli_flash)交接期间有冲突窗口(PermissionError / 触发失败)。
本代理常驻独占 COM 端口, 提供两条互不影响的通道:

  ├─ 监视通道: read loop 把字节按行进内存缓冲(500 行) + 落盘 serial.live
  │            → 服务端 /api/serial 直接读文件, 不再自己打开 COM 口
  └─ OTA 通道: TCP 127.0.0.1:<port>, cli_flash --bridge 连接后
               (写) 客户端字节 → 代理写 COM 口
               (读) 代理把 COM 口字节实时转发给客户端(升级打印/ACK 全可见)

CDC 假死自愈: read 方向连续 <data> 秒无数据且串口开着 → 重开 COM 口
(pyserial 新句柄 = 新 USB 端点, 实测可恢复, 无需物理拔插)。

用法:
    python serial_bridge.py --port COM10 [--baud 115200] [--tcp 5010]
      [--lines-file <abs 路径>] [--stale 30]
"""
import argparse
import os
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("[bridge] pyserial 缺失, 请使用沙箱 python 运行", flush=True)
    sys.exit(2)


class SerialBridge:
    def __init__(self, port, baud, tcp_port, lines_file, stale,
                 ctrl_port=5011, hex_file=None, sniff_file=None):
        self.port = port
        self.baud = baud
        self.tcp_port = tcp_port
        self.lines_file = lines_file
        self.stale_sec = stale
        self.ctrl_port = ctrl_port      # TX 控制端口: 客户端字节 → 写 COM
        self.hex_file = hex_file        # RX HEX 落盘(原始字节 hex, 面板 HEX 模式数据源)
        self.sniff_file = sniff_file    # 嗅探转储分流(外部工具回灌的 "RX: 0x.." 文本)
        self.ser = None
        self.lines = []            # 内存行缓冲(监视通道)
        self.tail = b""            # 半行残尾
        self.lock = threading.Lock()
        self.clients = set()       # TCP 客户端(OTA 通道)
        self.clients_lock = threading.Lock()
        self.last_data = 0.0       # 最近一次读到数据的时间
        self.running = True

    # ---- 串口管理 ----
    def open_port(self):
        try:
            if self.ser and self.ser.is_open:
                try:
                    self.ser.close()
                except Exception:
                    pass
            self.ser = serial.Serial(
                port=self.port, baudrate=self.baud,
                timeout=1.0, write_timeout=2.0)
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            self.last_data = time.time()
            print(f"[bridge] {self.port} @ {self.baud} opened", flush=True)
            return True
        except Exception as e:
            self.ser = None
            print(f"[bridge] open {self.port} 失败: {e}", flush=True)
            return False

    # ---- 落盘辅助: 共享写追加, 超 2MB 截断只留尾部 ----
    def _append_truncated(self, path, text):
        try:
            if os.path.exists(path) and os.path.getsize(path) > 2 * 1024 * 1024:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(-256 * 1024, os.SEEK_END)
                    tail_keep = f.read()
                with open(path, "w", encoding="utf-8") as f:
                    f.write(tail_keep)
            with open(path, "a", encoding="utf-8", errors="replace") as f:
                f.write(text)
        except Exception:
            pass

    # ---- 监视通道: 行缓冲 + 落盘(嗅探转储分流) ----
    def push_bytes(self, data):
        now = time.time()
        self.last_data = now
        buf = self.tail + data
        parts = buf.split(b"\n")
        self.tail = parts[-1]
        new_lines = []
        sniff_lines = []          # "RX: 0x.." 转储文本 = 外部嗅探/回环工具回灌, 单独一路
        for p in parts[:-1]:
            line = p.replace(b"\r", b"").decode("utf-8", errors="replace")
            line = "".join(ch for ch in line if ch.isprintable() or ch in "\t")
            if line.strip():
                if line.startswith("RX: 0x") or line.startswith("TX: 0x"):
                    sniff_lines.append(line)
                else:
                    new_lines.append(line)
        if new_lines:
            with self.lock:
                self.lines.extend(new_lines)
                if len(self.lines) > 500:
                    self.lines = self.lines[-500:]
            # 正常通道落盘供服务端读取
            self._append_truncated(self.lines_file, "\n".join(new_lines) + "\n")
        # 嗅探转储独立落盘(不占正常行缓冲, 前端独立框展示)
        if sniff_lines:
            self._append_truncated(self.sniff_file, "\n".join(sniff_lines) + "\n")
        # RX HEX 落盘(逐块 hex 大写, 供面板 HEX 模式显示; 超 2MB 截断)
        if self.hex_file and data:
            self._append_truncated(self.hex_file, data.hex().upper() + "\n")
        # 转发给所有 OTA 客户端(双路分发)
        self.broadcast(data)

    def broadcast(self, data):
        with self.clients_lock:
            dead = []
            for c in list(self.clients):
                try:
                    c.sendall(data)
                except Exception:
                    dead.append(c)
            for c in dead:
                try:
                    c.close()
                except Exception:
                    pass
                self.clients.discard(c)

    # ---- read loop(常驻线程) ----
    def read_loop(self):
        while self.running:
            if not (self.ser and self.ser.is_open):
                if not self.open_port():
                    time.sleep(2)
                    continue
                time.sleep(0.2)
            try:
                data = self.ser.read(512)
                if data:
                    self.push_bytes(data)
                    continue
                # 读空: 空闲正常; 但长时间无数据 = CDC 读方向假死 → 重开句柄
                if time.time() - self.last_data > self.stale_sec:
                    print(f"[bridge] {self.stale_sec}s 无数据, CDC 读方向疑似假死 → 重开",
                          flush=True)
                    self.open_port()
            except Exception as e:
                print(f"[bridge] read 异常: {e} → 重开", flush=True)
                self.open_port()

    # ---- OTA 通道: TCP 服务(透传) ----
    def handle_client(self, conn):
        conn.settimeout(10.0)
        with self.clients_lock:
            self.clients.add(conn)
        try:
            while self.running:
                try:
                    req = conn.recv(4096)
                except socket.timeout:
                    continue
                except Exception:
                    break
                if not req:
                    break
                # 客户端字节 → 写 COM 口
                if self.ser and self.ser.is_open:
                    try:
                        self.ser.write(req)
                        self.ser.flush()
                    except Exception as e:
                        print(f"[bridge] write 失败: {e}", flush=True)
                        break
                else:
                    print("[bridge] COM 口未就绪, 丢弃写入", flush=True)
                    break
        finally:
            with self.clients_lock:
                self.clients.discard(conn)
            try:
                conn.close()
            except Exception:
                pass

    def tcp_server(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.tcp_port))
        srv.listen(4)
        print(f"[bridge] TCP 透传监听 127.0.0.1:{self.tcp_port}", flush=True)
        while self.running:
            try:
                conn, _ = srv.accept()
            except Exception:
                continue
            t = threading.Thread(target=self.handle_client, args=(conn,), daemon=True)
            t.start()

    # ---- TX 控制通道(驾驶舱发送): 只写 COM, 不参与广播 ----
    def ctrl_server(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.ctrl_port))
        srv.listen(4)
        print(f"[bridge] TX 控制监听 127.0.0.1:{self.ctrl_port}", flush=True)
        while self.running:
            try:
                conn, _ = srv.accept()
            except Exception:
                continue
            try:
                conn.settimeout(5.0)
                req = conn.recv(4096)
                if req and self.ser and self.ser.is_open:
                    self.ser.write(req)
                    self.ser.flush()
                    print(f"[bridge] TX {len(req)}B → COM", flush=True)
            except Exception:
                pass
            finally:
                try:
                    conn.close()
                except Exception:
                    pass

    def run(self):
        # 首行落盘标记(便于服务端判断代理活着)
        try:
            with open(self.lines_file, "a", encoding="utf-8") as f:
                f.write(f"[bridge] serial_bridge started {self.port} @ {self.baud}\n")
        except Exception:
            pass
        self.open_port()
        t1 = threading.Thread(target=self.read_loop, daemon=True)
        t1.start()
        t2 = threading.Thread(target=self.tcp_server, daemon=True)
        t2.start()
        t3 = threading.Thread(target=self.ctrl_server, daemon=True)
        t3.start()
        while self.running:
            time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description="串口双线代理")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--tcp", type=int, default=5010)
    ap.add_argument("--lines-file", default="serial.live")
    ap.add_argument("--hex-file", default=None,
                    help="RX HEX 落盘路径(默认 <lines-file 目录>/serial.hex)")
    ap.add_argument("--ctrl-port", type=int, default=5011,
                    help="TX 控制端口(驾驶舱发送), 默认 5011")
    ap.add_argument("--sniff-file", default=None,
                    help="嗅探转储落盘路径(默认 <lines-file 目录>/serial.sniff)")
    ap.add_argument("--stale", type=int, default=30,
                    help="读方向 N 秒无数据视为假死并重开(默认 30)")
    a = ap.parse_args()
    hex_file = a.hex_file
    if hex_file is None and a.lines_file:
        hex_file = os.path.join(os.path.dirname(a.lines_file), "serial.hex")
    sniff_file = a.sniff_file
    if sniff_file is None and a.lines_file:
        sniff_file = os.path.join(os.path.dirname(a.lines_file), "serial.sniff")
    bridge = SerialBridge(a.port, a.baud, a.tcp, a.lines_file, a.stale,
                          ctrl_port=a.ctrl_port, hex_file=hex_file,
                          sniff_file=sniff_file)
    try:
        bridge.run()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.running = False
        if bridge.ser and bridge.ser.is_open:
            try:
                bridge.ser.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
