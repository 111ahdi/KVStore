# -*- coding: utf-8 -*-
"""
KVStore 交互式客户端。
连接 8888 端口，输入命令发送到服务器，打印响应。
输入 EXIT 或 QUIT 断开连接。
"""

import socket

HOST = '127.0.0.1'
PORT = 8888


def strip_iac(data: bytes) -> bytes:
    """过滤 telnet IAC 协商字节（0xFF 开头、共 3 字节的序列）。"""
    result = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFF and i + 2 < len(data):
            i += 3             # 跳过 IAC + CMD + OPT
            continue
        result.append(data[i])
        i += 1
    return bytes(result)


def recv_until_prompt(sock: socket.socket) -> str:
    """读取直到收到 'db> '，过滤 IAC，返回提示符之前的内容。"""
    data = b''
    while True:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
        if b'db> ' in data:
            idx = data.rfind(b'db> ')
            data = data[:idx]
            break
    return strip_iac(data).decode('utf-8', errors='replace')


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)

    try:
        sock.connect((HOST, PORT))
        print(f"Connected to KVStore at {HOST}:{PORT}")

        # 接收并打印欢迎信息（已过滤 IAC）
        welcome = recv_until_prompt(sock)
        print(welcome.strip())

        while True:
            try:
                cmd = input().strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not cmd:
                continue

            # 发送命令
            sock.sendall((cmd + '\r\n').encode('utf-8'))

            if cmd.upper() in ('EXIT', 'QUIT'):
                resp = recv_until_prompt(sock)
                # 去除服务端回显的命令文本
                echo_prefix = cmd + '\r\n'
                if resp.startswith(echo_prefix):
                    resp = resp[len(echo_prefix):]
                print(resp.strip())
                break

            # 接收响应，去除服务端回显的字符后打印
            resp = recv_until_prompt(sock)
            echo_prefix = cmd + '\r\n'
            if resp.startswith(echo_prefix):
                resp = resp[len(echo_prefix):]
            print(resp.strip())

    except socket.timeout:
        print("Connection timed out.")
    except ConnectionRefusedError:
        print(f"Could not connect to {HOST}:{PORT}. Is the server running?")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()
        print("Disconnected.")


if __name__ == '__main__':
    main()
