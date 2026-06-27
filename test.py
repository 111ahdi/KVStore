# -*- coding: utf-8 -*-
"""
KVStore 自动化测试脚本。
先启动 main.exe，连接 8888 端口，顺序测试所有命令，最后 SHUTDOWN。
"""

import socket
import subprocess
import time
import os
import threading

HOST = '127.0.0.1'
PORT = 8888
EXE_PATH = os.path.join(os.path.dirname(__file__), 'main.exe')

passed = 0
failed = 0
lock = threading.Lock()


def recv_until_prompt(sock: socket.socket) -> str:
    """读取直到收到 'db> ' 提示符，返回提示符之前的内容。"""
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
    return data.decode('utf-8', errors='replace')


def send_cmd(sock: socket.socket, cmd: str) -> str:
    """发送一条命令，返回服务器响应（不含末尾 db> 提示符）。"""
    sock.sendall((cmd + '\r\n').encode('utf-8'))
    return recv_until_prompt(sock)


def test(name: str, actual: str, expected_contains: str):
    """检查 actual 是否包含 expected_contains,输出测试结果。"""
    global passed, failed
    GREEN = "\033[92m"
    RED = "\033[91m"
    RESET = "\033[0m"
    with lock:
        if expected_contains in actual:
            passed += 1
            print(f"{GREEN}[PASS]{RESET} {name}")
        else:
            failed += 1
            print(f"{RED}[FAIL]{RESET} {name}")
            print(f"         expected to contain: {repr(expected_contains)}")
            print(f"         actual:              {repr(actual)}")


def connect():
    """连接服务器，跳过欢迎信息。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3)
    sock.connect((HOST, PORT))
    # 吃掉欢迎语
    recv_until_prompt(sock)
    return sock


# -- 启动服务器 --
print("Starting server...")
proc = subprocess.Popen([EXE_PATH],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NO_WINDOW)
time.sleep(1.5)

try:
    # ============================================
    #  String 测试
    # ============================================
    print("\n-- String Commands --")
    s = connect()

    resp = send_cmd(s, 'PUT name Alice')
    test("PUT string", resp, "OK")

    resp = send_cmd(s, 'GET name')
    test("GET string", resp, '"Alice"')

    resp = send_cmd(s, 'PUT greeting Hello World')
    test("PUT multi-word value", resp, "OK")

    resp = send_cmd(s, 'GET greeting')
    test("GET multi-word", resp, '"Hello World"')

    resp = send_cmd(s, 'GET ghost')
    test("GET non-existing", resp, "(nil)")

    resp = send_cmd(s, 'DELETE name')
    test("DELETE string", resp, "OK")

    resp = send_cmd(s, 'GET name')
    test("GET after DELETE", resp, "(nil)")

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  List 测试
    # ============================================
    print("\n-- List Commands --")
    s = connect()

    resp = send_cmd(s, 'RPUSH mylist a b c')
    test("RPUSH multi-token", resp, "OK")

    resp = send_cmd(s, 'LRANGE mylist 0 -1')
    test("LRANGE after RPUSH", resp, '0) "a"\r\n1) "b"\r\n2) "c"')

    resp = send_cmd(s, 'LLEN mylist')
    test("LLEN", resp, "3")

    resp = send_cmd(s, 'LPUSH mylist 0')
    test("LPUSH single", resp, "OK")

    resp = send_cmd(s, 'LRANGE mylist 0 -1')
    test("LRANGE after LPUSH", resp, '0) "0"\r\n1) "a"')

    resp = send_cmd(s, 'LPOP mylist')
    test("LPOP", resp, '"0"')

    resp = send_cmd(s, 'RPOP mylist')
    test("RPOP", resp, '"c"')

    resp = send_cmd(s, 'LLEN mylist')
    test("LLEN after pops", resp, "2")

    resp = send_cmd(s, 'LRANGE mylist 0 0')
    test("LRANGE single element", resp, '0) "a"')

    resp = send_cmd(s, 'LPOP mylist')
    send_cmd(s, 'LPOP mylist')  # 清空
    resp = send_cmd(s, 'GET mylist')
    test("GET empty list", resp, "(empty list)")

    # 类型不匹配测试
    send_cmd(s, 'PUT strkey hello')
    resp = send_cmd(s, 'LPUSH strkey x')
    test("LPUSH on string -> WRONGTYPE", resp, "(error)")

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  Dict 测试
    # ============================================
    print("\n-- Dict Commands --")
    s = connect()

    resp = send_cmd(s, 'HSET user name Alice')
    test("HSET", resp, "OK")

    resp = send_cmd(s, 'HSET user age 30')
    test("HSET second field", resp, "OK")

    resp = send_cmd(s, 'HGET user name')
    test("HGET", resp, '"Alice"')

    resp = send_cmd(s, 'HLEN user')
    test("HLEN", resp, "2")

    resp = send_cmd(s, 'HGETALL user')
    test("HGETALL contains name", resp, '"name"')
    test("HGETALL contains Alice", resp, '"Alice"')

    resp = send_cmd(s, 'HKEYS user')
    test("HKEYS", resp, '"name"')

    resp = send_cmd(s, 'HDEL user age')
    test("HDEL", resp, "OK")

    resp = send_cmd(s, 'HLEN user')
    test("HLEN after HDEL", resp, "1")

    resp = send_cmd(s, 'HGET user age')
    test("HGET deleted field", resp, "(error)")

    # 自动创建 dict
    resp = send_cmd(s, 'HSET newdict k v')
    test("HSET auto-create dict", resp, "OK")

    resp = send_cmd(s, 'HGETALL newdict')
    test("HGETALL auto-created", resp, '"k" = "v"')

    # 类型不匹配
    resp = send_cmd(s, 'HSET strkey x y')
    test("HSET on string -> WRONGTYPE", resp, "(error)")

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  统一 GET 测试
    # ============================================
    print("\n-- Unified GET --")
    s = connect()
    send_cmd(s, 'PUT s hello')
    send_cmd(s, 'RPUSH l x y z')
    send_cmd(s, 'HSET d a 1')

    resp = send_cmd(s, 'GET s')
    test("GET string type", resp, '"hello"')

    resp = send_cmd(s, 'GET l')
    test("GET list type", resp, '0) "x"')

    resp = send_cmd(s, 'GET d')
    test("GET dict type", resp, '"a" = "1"')

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  类型冲突测试
    # ============================================
    print("\n-- Type Conflict --")
    s = connect()

    # String 上用 List/Dict 操作
    send_cmd(s, 'PUT strkey hello')
    resp = send_cmd(s, 'LPUSH strkey x')
    test("LPUSH on string -> error", resp, "(error)")

    resp = send_cmd(s, 'HSET strkey x y')
    test("HSET on string -> error", resp, "(error)")

    resp = send_cmd(s, 'LPOP strkey')
    test("LPOP on string -> error", resp, "(error)")

    resp = send_cmd(s, 'RPOP strkey')
    test("RPOP on string -> error", resp, "(error)")

    resp = send_cmd(s, 'HGET strkey f')
    test("HGET on string -> error", resp, "(error)")

    # PUT 覆盖 List
    send_cmd(s, 'RPUSH lkey a b c')
    resp = send_cmd(s, 'PUT lkey value')
    test("PUT overwrites list", resp, "OK")

    resp = send_cmd(s, 'GET lkey')
    test("GET after PUT overwrite", resp, '"value"')

    # List 上用 Dict 操作
    send_cmd(s, 'RPUSH lkey2 x')
    resp = send_cmd(s, 'HSET lkey2 f v')
    test("HSET on list -> error", resp, "(error)")

    # Dict 上用 List 操作
    send_cmd(s, 'HSET dkey a 1')
    resp = send_cmd(s, 'LPUSH dkey x')
    test("LPUSH on dict -> error", resp, "(error)")

    resp = send_cmd(s, 'GET dkey')
    test("Dict unchanged after error", resp, '"a" = "1"')

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  边界条件 / 非法格式 测试
    # ============================================
    print("\n-- Edge Cases & Invalid Format --")
    s = connect()

    # 多空格会被 tokenizer 压缩，PUT + 空格 + value 只产生 2 个 token
    resp = send_cmd(s, 'PUT ' + ' ' + 'value')
    test("PUT whitespace-only key -> syntax error", resp, "Error")

    # 参数不足
    resp = send_cmd(s, 'PUT')
    test("PUT no args", resp, "Error")

    resp = send_cmd(s, 'PUT onlykey')
    test("PUT key without value", resp, "Error")

    resp = send_cmd(s, 'GET')
    test("GET no args", resp, "Error")

    resp = send_cmd(s, 'DELETE')
    test("DELETE no args", resp, "Error")

    resp = send_cmd(s, 'LPUSH')
    test("LPUSH no args", resp, "Error")

    resp = send_cmd(s, 'LPUSH onlykey')
    test("LPUSH key without value", resp, "Error")

    resp = send_cmd(s, 'HSET')
    test("HSET no args", resp, "Error")

    resp = send_cmd(s, 'HSET k')
    test("HSET no field", resp, "Error")

    resp = send_cmd(s, 'HSET k f')
    test("HSET field without value", resp, "Error")

    resp = send_cmd(s, 'HGET')
    test("HGET no args", resp, "Error")

    resp = send_cmd(s, 'LRANGE mykey')
    test("LRANGE no range args", resp, "Error")

    # 未知命令
    resp = send_cmd(s, 'ASDFGHJKL')
    test("Garbage command", resp, "unknown command")

    # 空行 / 纯空格
    resp = send_cmd(s, '')
    test("Empty line", resp, "")  # 只返回提示符或无内容

    resp = send_cmd(s, '   ')
    test("Whitespace-only", resp, "")

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  大 Value 测试
    # ============================================
    print("\n-- Large Value --")
    s = connect()

    # 100KB value
    big_val = 'X' * (100 * 1024)
    resp = send_cmd(s, f'PUT bigkey {big_val}')
    test("PUT 100KB value", resp, "OK")

    resp = send_cmd(s, 'GET bigkey')
    test("GET bigkey starts with XXX", resp, '"XXX')

    resp = send_cmd(s, 'DELETE bigkey')
    test("DELETE bigkey", resp, "OK")

    # 1MB value
    huge_val = 'Y' * (1024 * 1024)
    resp = send_cmd(s, f'PUT hugekey {huge_val}')
    test("PUT 1MB value", resp, "OK")

    resp = send_cmd(s, 'GET hugekey')
    test("GET hugekey starts with YYY", resp, '"YYY')

    resp = send_cmd(s, 'DELETE hugekey')
    test("DELETE hugekey", resp, "OK")

    send_cmd(s, 'EXIT')
    s.close()

    # ============================================
    #  并发测试：10 线程同时 PUT
    # ============================================
    print("\n-- Concurrent (10 threads x 20 ops) --")
    s_main = connect()
    send_cmd(s_main, 'DELETE concur_key')

    results = []
    errors = []

    def concurrent_put(tid: int):
        try:
            sock = connect()
            for i in range(20):
                resp = send_cmd(sock, f'PUT concur_{tid}_{i} val_{tid}')
                if 'Error' in resp and 'unknown' not in resp:
                    errors.append(f"T{tid}-{i}: {resp}")
            send_cmd(sock, 'EXIT')
            sock.close()
            results.append(tid)
        except Exception as e:
            errors.append(f"T{tid} exception: {e}")

    threads = []
    for tid in range(10):
        t = threading.Thread(target=concurrent_put, args=(tid,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join(timeout=15)

    with lock:
        if len(results) == 10 and len(errors) == 0:
            passed += 1
            print(f"  {chr(27)}[92m[PASS]{chr(27)}[0m Concurrent PUT (10 threads, 200 writes, 0 errors)")
        else:
            failed += 1
            print(f"  {chr(27)}[91m[FAIL]{chr(27)}[0m Concurrent PUT — {len(results)}/10 ok, errors: {errors[:3]}")

    # 验证并发写入数据可读
    resp = send_cmd(s_main, 'GET concur_5_10')
    test("Concurrent data readable", resp, '"val_5"')

    resp = send_cmd(s_main, 'GET concur_9_19')
    test("Concurrent data readable (2)", resp, '"val_9"')

    send_cmd(s_main, 'EXIT')
    s_main.close()

    # ============================================
    #  持久化测试
    # ============================================
    print("\n-- Persistence --")
    s = connect()
    send_cmd(s, 'PUT persist_test hello_world')
    send_cmd(s, 'SHUTDOWN')
    s.close()
    proc.wait(timeout=5)

    # 重启服务器
    time.sleep(1)
    proc2 = subprocess.Popen([EXE_PATH],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(1.5)

    s = connect()
    resp = send_cmd(s, 'GET persist_test')
    test("Persistence after restart", resp, '"hello_world"')

    send_cmd(s, 'SHUTDOWN')
    s.close()
    proc2.wait(timeout=5)

except Exception as e:
    print(f"\n[ERROR] {e}")

finally:
    # 确保进程终止
    for p in [proc] + ([proc2] if 'proc2' in dir() else []):
        try:
            p.kill()
            p.wait(timeout=3)
        except:
            pass

# -- 结果汇总 --
total = passed + failed
print(f"\n{'='*40}")
print(f"Results: {passed}/{total} passed")
if failed == 0:
    print("All tests passed!")
else:
    print(f"{failed} test(s) FAILED.")
print(f"{'='*40}")
