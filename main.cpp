// 多线程网络键值存储服务器。
// 每个客户端由独立线程处理，通过 TCP 端口 8888 接收命令。

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

// 线程安全打印（避免多线程 std::cout 交错）。
void safePrint(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << msg;
}

// 去除字符串首尾的空白字符和换行符 \r \n。
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

// 返回可用命令列表字符串。
std::string getHelpText() {
    return std::string("Commands:\n")
        + "  PUT <key> <value...>  - Store a key-value pair (key is a single word, value may contain spaces)\n"
        + "  GET <key>             - Retrieve value by key\n"
        + "  DELETE <key>          - Remove key\n"
        + "  HELP                  - Show this help\n"
        + "  EXIT / QUIT           - Disconnect from server\n"
        + "  SHUTDOWN              - Shut down the server\n";
}

// ============================================================================
//  客户端处理线程：一个客户端对应一个线程
// ============================================================================

void clientHandler(SOCKET clientSocket, KVStore* pStore, std::mutex& dbMutex) {
    // Telnet 协商：告知客户端服务端负责回显
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

    // 内层循环：处理当前客户端的指令
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived <= 0) {
            safePrint(">>> Client disconnected.\n");
            break;
        }

        // 服务端统一回显：逐字节处理，回显 + 追加到行缓冲区
        for (int i = 0; i < bytesReceived; ) {
            char c = buffer[i];

            // Telnet IAC 协商序列：跳过 IAC + 命令 + 选项（共 3 字节）
            if (c == '\xFF') {
                i += 3;
                continue;
            }

            ++i;  // 非 IAC 字节才正常步进

            if (c == '\r') {
                std::string crlf = "\r\n";
                send(clientSocket, crlf.c_str(), crlf.size(), 0);
                continue;
            }
            if (c == '\b' || c == 0x7F) {          // 退格键（BS 或 DEL）
                if (!recvBuf.empty()) {
                    recvBuf.pop_back();
                    std::string erase = "\b \b";
                    send(clientSocket, erase.c_str(), erase.size(), 0);
                }
                continue;                           // recvBuf 空 → 忽略，阻止越界删除提示符
            }
            // 普通字符：回显并加入行缓冲区
            send(clientSocket, &c, 1, 0);
            recvBuf += c;
        }

        // 检查是否收到了完整一行（以 \n 结尾）
        size_t newlinePos = recvBuf.find('\n');
        if (newlinePos == std::string::npos) continue;

        // 提取第一行，剩余数据保留在缓冲区
        std::string line = trim(recvBuf.substr(0, newlinePos));
        recvBuf.erase(0, newlinePos + 1);

        if (line.empty()) {
            sendMsg(clientSocket, "db> ");
            continue;
        }

        // 按空白字符拆分输入行
        std::istringstream iss(line);
        std::string token;
        std::vector<std::string> tokens;
        while (iss >> token) tokens.push_back(token);
        if (tokens.empty()) continue;

        std::string command = tokens[0];

        // 命令转为大写（大小写不敏感），key/value 保持原样
        for (char& ch : command) {
            ch = std::toupper(static_cast<unsigned char>(ch));
        }

        std::string reply;

        try {
            // --- EXIT / QUIT（客户端主动断开）---
            if (command == "EXIT" || command == "QUIT") {
                safePrint(">>> Client requested disconnect.\n");
                reply = "Goodbye!\n";
                sendMsg(clientSocket, reply);
                break;
            }

            // --- PUT ---
            else if (command == "PUT") {
                if (tokens.size() < 3) {
                    reply = "Error: Invalid PUT syntax. Usage: PUT <key> <value...>\n";
                } else {
                    std::string key = tokens[1];
                    std::string value = tokens[2];
                    for (size_t i = 3; i < tokens.size(); ++i) {
                        value += ' ';
                        value += tokens[i];
                    }
                    {
                        std::lock_guard<std::mutex> lock(dbMutex);
                        pStore->put(key, value);
                    }
                    reply = "Success - put [" + key + "] = [" + value + "]\n";
                }
            }

            // --- GET ---
            else if (command == "GET") {
                if (tokens.size() != 2) {
                    reply = "Error: Invalid GET syntax. Usage: GET <key>\n";
                } else {
                    std::string key = tokens[1];
                    std::lock_guard<std::mutex> lock(dbMutex);
                    if (pStore->contains(key)) {
                        reply = "[" + key + "] = [" + pStore->get(key) + "]\n";
                    } else {
                        reply = "(nil)\n";
                    }
                }
            }

            // --- DELETE ---
            else if (command == "DELETE") {
                if (tokens.size() != 2) {
                    reply = "Error: Invalid DELETE syntax. Usage: DELETE <key>\n";
                } else {
                    std::string key = tokens[1];
                    std::lock_guard<std::mutex> lock(dbMutex);
                    if (pStore->remove(key)) {
                        reply = "Success - deleted [" + key + "]\n";
                    } else {
                        reply = "Error: key [" + key + "] not found, cannot delete.\n";
                    }
                }
            }

            // --- HELP ---
            else if (command == "HELP") {
                reply = getHelpText();
            }

            // --- SHUTDOWN（关闭服务器）---
            else if (command == "SHUTDOWN") {
                reply = "Server shutting down. Saving data...\n";
                sendMsg(clientSocket, reply);

                // 加锁保存数据
                {
                    std::lock_guard<std::mutex> lock(dbMutex);
                    pStore->saveToFile("dump.db");
                }

                // 通知主线程停止，关闭监听 socket 以解除 accept 阻塞
                g_running = false;
                closesocket(g_serverSocket);

                safePrint("[System] Data saved, server shut down safely.\n");
                closesocket(clientSocket);
                return;
            }

            // --- Unknown ---
            else {
                reply = "Error: unknown command \"" + command + "\". Type HELP for usage.\n";
            }
        } catch (const std::exception& e) {
            reply = std::string("Error: ") + e.what() + "\n";
        }

        // 发送结果 + 下一次提示符
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

    std::mutex dbMutex;

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

    std::cout << "=== KVStore (Multi-threaded) server started, listening on port 8888 ===\n" << std::endl;

    // 3. 主 accept 循环
    while (g_running) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(g_serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (!g_running) break;  // SHUTDOWN 触发的正常关闭
            continue;               // 意外错误，继续等待
        }

        safePrint(">>> New client connected.\n");

        // 每个客户端一个独立线程，detach 后自行回收
        std::thread(clientHandler, clientSocket, pStore.get(), std::ref(dbMutex)).detach();
    }

    // 4. 关闭服务器
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        pStore->saveToFile("dump.db");
    }
    closesocket(g_serverSocket);
    WSACleanup();

    std::cout << "[System] Server stopped.\n";
    return 0;
}
