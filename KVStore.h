// KVStore.h —— 内存键值存储，支持 String / List / Dict 三种类型，LRU 淘汰

#ifndef KVSTORE_H
#define KVSTORE_H

#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// 三种值类型
using StringType = std::string;
using ListType   = std::list<std::string>;
using DictType   = std::unordered_map<std::string, std::string>;

using ComplexValue = std::variant<StringType, ListType, DictType>;

class KVStore {
public:
    // 构造 / 析构
    explicit KVStore(size_t capacity = 256);
    ~KVStore();

    // 禁止拷贝 / 允许移动
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;
    KVStore(KVStore&&) noexcept = default;
    KVStore& operator=(KVStore&&) noexcept = default;

    // ---- String 操作 ----
    void put(const std::string& key, const std::string& value);
    [[nodiscard]] std::string get(const std::string& key) const;
    [[nodiscard]] bool remove(const std::string& key);
    [[nodiscard]] bool clear(const std::string& key);         // 清空 key 下的全部内容（保留 key）
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] bool empty() const;

    // 查询 key 的类型：返回 "string" / "list" / "dict" / "none"
    [[nodiscard]] std::string type(const std::string& key) const;

    // ---- List 操作 ----
    void lpush(const std::string& key, const std::string& value);
    void rpush(const std::string& key, const std::string& value);
    std::string lpop(const std::string& key);
    std::string rpop(const std::string& key);
    [[nodiscard]] std::vector<std::string> lrange(const std::string& key, int start, int stop) const;
    [[nodiscard]] size_t llen(const std::string& key) const;

    // ---- Dict 操作 ----
    void hset(const std::string& key, const std::string& field, const std::string& value);
    std::string hget(const std::string& key, const std::string& field) const;
    bool hdel(const std::string& key, const std::string& field);
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> hgetall(const std::string& key) const;
    [[nodiscard]] std::vector<std::string> hkeys(const std::string& key) const;
    [[nodiscard]] size_t hlen(const std::string& key) const;

    // ---- 持久化 ----
    bool saveToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filename);

private:
    size_t capacity_;

    using ListNode = std::pair<std::string, ComplexValue>;
    using MapType  = std::unordered_map<std::string, std::list<ListNode>::iterator>;

    mutable std::list<ListNode> lru_list_;  // mutable：const 方法仍可更新 LRU 顺序
    mutable MapType store_;

    // 内部辅助：查找 key → 提升到 LRU 头部 → 返回 list 迭代器（未找到抛异常）
    std::list<ListNode>::iterator _lookup(const std::string& key) const;

    // 按类型取出 value 引用（类型不匹配抛异常）
    template <typename T>
    T& _getRef(const std::string& key) const {
        auto listIt = _lookup(key);
        auto* ptr = std::get_if<T>(&listIt->second);
        if (!ptr) throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        return *ptr;
    }

    // 淘汰尾部（最久未使用）
    void _evict();

    // lpush / rpush 的公共实现
    template <typename PushOp>
    void _pushList(const std::string& key, const std::string& value, PushOp push);
};

#endif // KVSTORE_H
