// 多线程网络键值存储服务器。
// 每个客户端由独立线程处理，通过 TCP 端口 8888 接收命令。
// 支持 String / List / Dict 三种数据类型。

#include "KVStore.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Windows 网络编程
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

// ============================================================================
//  全局变量
// ============================================================================

std::atomic<bool> g_running{true};      // 服务器运行标志，SHUTDOWN 时置 false
SOCKET g_serverSocket = INVALID_SOCKET; // 监听 socket（供 SHUTDOWN 跨线程关闭）
std::mutex g_printMutex;                // 保护 std::cout 多线程输出

// ============================================================================
//  辅助函数
// ============================================================================

// 线程安全打印（避免多线程 std::cout 交错输出）。
void safePrint(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << msg;
}

// 去除字符串首尾的空白字符和网络换行符 \r \n。
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// 发送消息给客户端，自动将 \n 转换为 \r\n（适配 telnet 协议）。
void sendMsg(SOCKET s, const std::string& msg) {
    std::string crlf;
    crlf.reserve(msg.size() + 16);
    for (char c : msg) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    send(s, crlf.c_str(), crlf.size(), 0);
}

// 拼接 tokens[start..] 为一个字符串，用空格分隔。
// tokens 后续不再使用，故对首个元素用 move 避免拷贝。
std::string joinTokens(std::vector<std::string>& tokens, size_t start) {
    std::string result = std::move(tokens[start]);
    for (size_t i = start + 1; i < tokens.size(); ++i) {
        result += ' ';
        result += tokens[i];
    }
    return result;
}

// 返回可用命令列表字符串。
std::string getHelpText() {
    return std::string("=== Common Commands (any type) ===\n")
        + "  PUT <key> <value...>  - Store a string value\n"
        + "  GET <key>             - Show all data of a key\n"
        + "                         String  -> \"value\"\n"
        + "                         List    -> 0) \"elem1\" ...\n"
        + "                         Dict    -> \"field\" = \"value\" ...\n"
        + "  DELETE <key>          - Completely remove a key\n"
        + "\n=== List Commands ===\n"
        + "  LPUSH <key> <v...>    - Push each token to front of list\n"
        + "  RPUSH <key> <v...>    - Push each token to back of list\n"
        + "  LPOP <key>            - Pop one element from front\n"
        + "  RPOP <key>            - Pop one element from back\n"
        + "  LRANGE <key> <s> <e>  - Get range (supports negative index)\n"
        + "  LLEN <key>            - Get list length\n"
        + "\n=== Dict Commands ===\n"
        + "  HSET <key> <f> <v...> - Set a dict field\n"
        + "  HGET <key> <field>    - Get a dict field value\n"
        + "  HDEL <key> <field>    - Delete a dict field\n"
        + "  HGETALL <key>         - Get all fields and values\n"
        + "  HKEYS <key>           - Get all field names\n"
        + "  HLEN <key>            - Get field count\n"
        + "\n=== Server ===\n"
        + "  HELP                  - Show this help\n"
        + "  EXIT / QUIT           - Disconnect\n"
        + "  SHUTDOWN              - Shut down server\n";
}

// ============================================================================
//  客户端处理线程：一个客户端对应一个线程
// ============================================================================

void clientHandler(SOCKET clientSocket, KVStore* pStore, std::mutex& dbMutex) {
    // Telnet 协商：告知客户端服务端负责回显，关闭客户端本地回显
    {
        const char IAC  = '\xFF';
        const char WILL = '\xFB';
        const char ECHO = '\x01';
        const char SGA  = '\x03';
        char negotiate[] = {IAC, WILL, ECHO, IAC, WILL, SGA};
        send(clientSocket, negotiate, sizeof(negotiate), 0);
    }

    // 发送欢迎语
    sendMsg(clientSocket, "Welcome to KVStore Network Server (Multi-threaded)!\ndb> ");

    char buffer[1024];
    std::string recvBuf; // 行缓冲区：累积收到的字节，直到遇到 \n 才处理一行

    // 主循环：接收 → 行缓冲 → 解析 → 执行 → 回复
    while (true) {
        // recv 返回实际字节数，后续只处理 [0, bytesReceived)，无需 memset
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived <= 0) {
            safePrint(">>> Client disconnected.\n");
            break;
        }

        // 逐字节处理：回显 + 写入行缓冲区
        for (int i = 0; i < bytesReceived; ) {
            char c = buffer[i];

            // Telnet IAC 协商序列：跳过 IAC + 命令 + 选项（共 3 字节）
            if (c == '\xFF') { i += 3; continue; }
            ++i;

            if (c == '\r') {
                // 使用字面量直接 send，避免每次构造临时 std::string
                send(clientSocket, "\r\n", 2, 0);
                continue;
            }
            if (c == '\b' || c == 0x7F) {
                if (!recvBuf.empty()) {
                    recvBuf.pop_back();
                    send(clientSocket, "\b \b", 3, 0);
                }
                continue; // recvBuf 为空时忽略退格，光标不会越过提示符
            }
            // 普通字符：回显并加入行缓冲区
            send(clientSocket, &c, 1, 0);
            recvBuf += c;
        }

        // 检查是否收到完整一行（以 \n 结尾）
        size_t newlinePos = recvBuf.find('\n');
        if (newlinePos == std::string::npos) continue;

        // 提取第一行并清理，剩余数据保留在缓冲区
        std::string line = trim(recvBuf.substr(0, newlinePos));
        recvBuf.erase(0, newlinePos + 1);
        if (line.empty()) { sendMsg(clientSocket, "db> "); continue; }

        // 按空白字符拆分输入行
        std::istringstream iss(line);
        std::string token;
        std::vector<std::string> tokens;
        while (iss >> token) tokens.push_back(token);
        if (tokens.empty()) continue;

        // 命令转为大写（大小写不敏感），key/value 保持原样
        std::string command = tokens[0];
        for (char& ch : command) ch = std::toupper(static_cast<unsigned char>(ch));

        std::string reply;

        try {
            // ---- String commands ----
            if (command == "PUT") {
                if (tokens.size() < 3) {
                    reply = "Error: PUT <key> <value...>\n";
                } else {
                    std::string key = tokens[1];
                    std::string value = joinTokens(tokens, 2); // 拼接 value（支持空格）
                    std::lock_guard<std::mutex> lock(dbMutex);
                    pStore->put(key, value);
                    reply = "OK\n";
                }
            }
            else if (command == "GET") {
                if (tokens.size() != 2) {
                    reply = "Error: GET <key>\n";
                } else {
                    std::string key = tokens[1];
                    std::lock_guard<std::mutex> lock(dbMutex);
                    if (!pStore->contains(key)) {
                        reply = "(nil)\n";
                    } else {
                        // 根据 key 的类型分发，统一打印全部数据
                        std::string t = pStore->type(key);
                        if (t == "string") {
                            reply = "\"" + pStore->get(key) + "\"\n";
                        } else if (t == "list") {
                            auto vals = pStore->lrange(key, 0, -1);
                            for (size_t i = 0; i < vals.size(); ++i)
                                reply += std::to_string(i) + ") \"" + vals[i] + "\"\n";
                            if (vals.empty()) reply = "(empty list)\n";
                        } else if (t == "dict") {
                            auto fields = pStore->hgetall(key);
                            for (const auto& f : fields)
                                reply += "  \"" + f.first + "\" = \"" + f.second + "\"\n";
                            if (fields.empty()) reply = "(empty dict)\n";
                        }
                    }
                }
            }
            else if (command == "DELETE") {
                if (tokens.size() != 2) {
                    reply = "Error: DELETE <key>\n";
                } else {
                    // 直接删除 key（无论 String / List / Dict 类型）
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = pStore->remove(tokens[1]) ? "OK\n" : "(nil)\n";
                }
            }

            // ---- List commands ----
            else if (command == "LPUSH") {
                if (tokens.size() < 3) {
                    reply = "Error: LPUSH <key> <value...>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    // 每个 token 作为独立元素逐个推入（倒序保证最终顺序与输入一致）
                    for (size_t i = 2; i < tokens.size(); ++i)
                        pStore->lpush(tokens[1], tokens[i]);
                    reply = "OK\n";
                }
            }
            else if (command == "RPUSH") {
                if (tokens.size() < 3) {
                    reply = "Error: RPUSH <key> <value...>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    // 每个 token 作为独立元素逐个追加
                    for (size_t i = 2; i < tokens.size(); ++i)
                        pStore->rpush(tokens[1], tokens[i]);
                    reply = "OK\n";
                }
            }
            else if (command == "LPOP") {
                if (tokens.size() != 2) {
                    reply = "Error: LPOP <key>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = "\"" + pStore->lpop(tokens[1]) + "\"\n";
                }
            }
            else if (command == "RPOP") {
                if (tokens.size() != 2) {
                    reply = "Error: RPOP <key>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = "\"" + pStore->rpop(tokens[1]) + "\"\n";
                }
            }
            else if (command == "LRANGE") {
                if (tokens.size() != 4) {
                    reply = "Error: LRANGE <key> <start> <stop>\n";
                } else {
                    // 用 lambda 作用域确保锁在 DB 操作完成后立即释放，
                    // 避免字符串拼接期间持锁阻塞其他线程
                    auto vals = [&] {
                        std::lock_guard<std::mutex> lock(dbMutex);
                        return pStore->lrange(tokens[1],
                                              std::stoi(tokens[2]),
                                              std::stoi(tokens[3]));
                    }();
                    for (size_t i = 0; i < vals.size(); ++i)
                        reply += std::to_string(i) + ") \"" + vals[i] + "\"\n";
                    if (vals.empty()) reply = "(empty)\n";
                }
            }
            else if (command == "LLEN") {
                if (tokens.size() != 2) {
                    reply = "Error: LLEN <key>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = std::to_string(pStore->llen(tokens[1])) + "\n";
                }
            }

            // ---- Dict commands ----
            else if (command == "HSET") {
                if (tokens.size() < 4) {
                    reply = "Error: HSET <key> <field> <value...>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    // key = tokens[1], field = tokens[2], value = tokens[3..]
                    pStore->hset(tokens[1], tokens[2], joinTokens(tokens, 3));
                    reply = "OK\n";
                }
            }
            else if (command == "HGET") {
                if (tokens.size() != 3) {
                    reply = "Error: HGET <key> <field>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = "\"" + pStore->hget(tokens[1], tokens[2]) + "\"\n";
                }
            }
            else if (command == "HDEL") {
                if (tokens.size() != 3) {
                    reply = "Error: HDEL <key> <field>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = pStore->hdel(tokens[1], tokens[2]) ? "OK\n" : "(nil)\n";
                }
            }
            else if (command == "HGETALL") {
                if (tokens.size() != 2) {
                    reply = "Error: HGETALL <key>\n";
                } else {
                    // 同 LRANGE：锁作用域限定在 DB 操作内
                    auto fields = [&] {
                        std::lock_guard<std::mutex> lock(dbMutex);
                        return pStore->hgetall(tokens[1]);
                    }();
                    for (const auto& f : fields)
                        reply += "  \"" + f.first + "\" = \"" + f.second + "\"\n";
                    if (fields.empty()) reply = "(empty)\n";
                }
            }
            else if (command == "HKEYS") {
                if (tokens.size() != 2) {
                    reply = "Error: HKEYS <key>\n";
                } else {
                    auto keys = [&] {
                        std::lock_guard<std::mutex> lock(dbMutex);
                        return pStore->hkeys(tokens[1]);
                    }();
                    for (size_t i = 0; i < keys.size(); ++i)
                        reply += std::to_string(i) + ") \"" + keys[i] + "\"\n";
                    if (keys.empty()) reply = "(empty)\n";
                }
            }
            else if (command == "HLEN") {
                if (tokens.size() != 2) {
                    reply = "Error: HLEN <key>\n";
                } else {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    reply = std::to_string(pStore->hlen(tokens[1])) + "\n";
                }
            }

            // ---- Server commands ----
            else if (command == "EXIT" || command == "QUIT") {
                safePrint(">>> Client requested disconnect.\n");
                reply = "Goodbye!\n";
                sendMsg(clientSocket, reply);
                break; // 退出 while 循环 → 关闭客户端 socket
            }
            else if (command == "HELP") {
                reply = getHelpText();
            }
            else if (command == "SHUTDOWN") {
                reply = "Server shutting down. Saving data...\n";
                sendMsg(clientSocket, reply);
                {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    pStore->saveToFile("dump.db");
                }
                // 通知主线程停止 + 关闭监听 socket 以解除 accept 阻塞
                g_running = false;
                closesocket(g_serverSocket);
                safePrint("[System] Data saved, server shut down safely.\n");
                closesocket(clientSocket);
                return;
            }
            else {
                reply = "Error: unknown command \"" + command + "\". Type HELP for usage.\n";
            }
        } catch (const std::exception& e) {
            // 兜底捕获未预期的异常（如 WRONGTYPE、Key not found 等）
            reply = std::string("(error) ") + e.what() + "\n";
        }

        // 发送执行结果 + 下一次提示符
        reply += "db> ";
        sendMsg(clientSocket, reply);
    }

    closesocket(clientSocket);
}

// ============================================================================
//  主函数
// ============================================================================

int main() {
    // 1. 初始化数据库引擎
    std::unique_ptr<KVStore> pStore = std::make_unique<KVStore>();
    if (pStore->loadFromFile("dump.db")) {
        std::cout << "[System] Successfully loaded data from dump.db.\n";
    } else {
        std::cout << "[System] No existing data found, starting with empty database.\n";
    }

    std::mutex dbMutex; // 保护 pStore 的互斥锁，所有 DB 操作共享

    // 2. 初始化 Winsock 并启动 TCP 服务器
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    g_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8888);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    bind(g_serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(g_serverSocket, SOMAXCONN);

    std::cout << "=== KVStore (Multi-threaded) server started, listening on port 8888 ===\n"
              << std::endl;

    // 3. 主 accept 循环：等待客户端连接，每个客户端分配独立线程
    while (g_running) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(g_serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (!g_running) break;  // SHUTDOWN 触发 → 正常退出
            continue;               // 意外错误 → 重试
        }

        safePrint(">>> New client connected.\n");
        // detach 后线程自行回收，无需 join
        std::thread(clientHandler, clientSocket, pStore.get(), std::ref(dbMutex)).detach();
    }

    // 4. 服务器关闭：保存数据 → 清理网络资源
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        pStore->saveToFile("dump.db");
    }
    closesocket(g_serverSocket);
    WSACleanup();

    std::cout << "[System] Server stopped.\n";
    return 0;
}
