// KVStore.cpp —— KVStore 类实现

#include "KVStore.h"
#include <cstddef>
#include <stdexcept>
#include <fstream>
#include <utility>

// ============================================================================
//  构造 / 析构
// ============================================================================

KVStore::KVStore() = default;   // 使用默认构造，store_ 初始化为空

KVStore::~KVStore() = default;  // 使用默认析构，unordered_map 自动释放

// ============================================================================
//  核心接口
// ============================================================================

void KVStore::put(const std::string& key, const std::string& value) {
    if (key.empty()) {                                      // 校验 key 非空
        throw std::invalid_argument("Key must not be empty");
    }
    store_.insert_or_assign(key, value);                    // C++17: 插入或更新，语义明确
}

std::string KVStore::get(const std::string& key) const {
    auto it = store_.find(key);                             // 查找 key
    if (it != store_.end()) return it->second;              // 找到，返回值
    throw std::runtime_error("Key not found: " + key);      // 未找到，抛异常
}

bool KVStore::remove(const std::string& key) {
    return store_.erase(key) > 0;  // erase 返回删除个数，>0 表示存在并已删除
}

bool KVStore::contains(const std::string& key) const {
    return store_.find(key) != store_.end();
}

size_t KVStore::size() const {
    return store_.size();
}

bool KVStore::empty() const {
    return store_.empty();
}

// ============================================================================
//  持久化
// ============================================================================

// 文件格式 (二进制安全):
//   每个条目三行:
//     <key_len>\n<key 原始字节>\n<val_len>\n<value 原始字节>\n
//   使用 write()/read() 确保 key/value 中的任意字节都不会破坏格式。

bool KVStore::saveToFile(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out.is_open()) return false;

    for (const auto& pair : store_) {
        const std::string& key   = pair.first;
        const std::string& value = pair.second;

        // 写入长度 + 换行
        out << key.size() << '\n';
        // 写入 key 原始字节 + 换行
        out.write(key.data(), key.size());
        out << '\n';
        // 写入 value 长度 + 换行
        out << value.size() << '\n';
        // 写入 value 原始字节 + 换行
        out.write(value.data(), value.size());
        out << '\n';

        if (!out.good()) return false;                      // 写入中途失败
    }
    return true;
}

bool KVStore::loadFromFile(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) return false;

    // 先读入临时 map，全部成功后再替换，保证原子性
    std::unordered_map<std::string, std::string> temp;
    size_t key_len, val_len;

    while (in >> key_len) {
        in.get();  // 吃掉 ' ' 或 '\n'（>> 不消耗换行符）
        if (!in.good()) return false;

        std::string key(key_len, '\0');
        if (key_len > 0) {
            in.read(&key[0], key_len);
            if (!in.good()) return false;
        }

        // 跳过 key 之后的换行符
        in.get();
        if (!in.good()) return false;

        if (!(in >> val_len)) return false;

        in.get();  // 吃掉 ' ' 或 '\n'
        if (!in.good()) return false;

        std::string value(val_len, '\0');
        if (val_len > 0) {
            in.read(&value[0], val_len);
            if (!in.good()) return false;
        }

        // 跳过 value 之后的换行符
        in.get();
        if (!in.good() && !in.eof()) return false;          // EOF 是正常的（最后一条记录）

        temp[std::move(key)] = std::move(value);
    }

    if (in.bad()) return false;                             // 流损坏

    store_.swap(temp);                                      // 全部成功 → 原子替换
    return true;
}
