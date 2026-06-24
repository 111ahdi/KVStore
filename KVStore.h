// KVStore.h —— 内存键值存储，基于 unordered_map，禁止拷贝、允许移动

#ifndef KVSTORE_H
#define KVSTORE_H

#include <string>
#include <unordered_map>

class KVStore {
public:
    // 构造 / 析构
    KVStore();                                              // 创建一个空的 KVStore 实例
    ~KVStore();                                             // 释放资源

    // 禁止拷贝（避免隐式深拷贝） / 允许移动（高效所有权转移）
    KVStore(const KVStore&) = delete;                       // 拷贝构造 —— 禁止
    KVStore& operator=(const KVStore&) = delete;            // 拷贝赋值 —— 禁止
    KVStore(KVStore&&) noexcept = default;                  // 移动构造 —— 转移 store_ 所有权
    KVStore& operator=(KVStore&&) noexcept = default;       // 移动赋值 —— 转移 store_ 所有权

    // 核心接口
    void put(const std::string& key, const std::string& value);  // 插入或更新键值对，key 为空抛异常
    [[nodiscard]] std::string get(const std::string& key) const; // 查询 key 对应的值，未找到抛异常
    [[nodiscard]] bool remove(const std::string& key);           // 删除键值对，成功返回 true
    [[nodiscard]] bool contains(const std::string& key) const;   // 检查 key 是否存在
    [[nodiscard]] size_t size() const;                           // 返回键值对数量
    [[nodiscard]] bool empty() const;                            // 存储是否为空

    // 持久化
    bool saveToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filename); 

private:
    std::unordered_map<std::string, std::string> store_;  // 内部存储容器
};

#endif // KVSTORE_H
