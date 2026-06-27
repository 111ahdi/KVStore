// KVStore.cpp —— 带 LRU 淘汰策略、支持 String / List / Dict 的键值存储实现

#include "KVStore.h"
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <utility>

// ============================================================================
//  构造 / 析构
// ============================================================================

KVStore::KVStore(size_t capacity) : capacity_(capacity) {}

KVStore::~KVStore() = default;

// ============================================================================
//  内部辅助
// ============================================================================

// 查找 key → 提升到 LRU 头部（splice 不使迭代器失效）→ 返回 list 迭代器。
// 未找到时抛异常。
std::list<KVStore::ListNode>::iterator KVStore::_lookup(const std::string& key) const {
    auto mapIt = store_.find(key);
    if (mapIt == store_.end()) {
        throw std::runtime_error("Key not found: " + key);
    }
    auto listIt = mapIt->second;
    lru_list_.splice(lru_list_.begin(), lru_list_, listIt);
    return listIt;
}

// 淘汰链表尾部节点（最久未使用）。
void KVStore::_evict() {
    const auto& back = lru_list_.back();
    store_.erase(back.first);
    lru_list_.pop_back();
}

// lpush / rpush 的公共实现：PushOp 为 push_front 或 push_back。
// key 存在且为 ListType → 压入并提升；key 不存在 → 自动创建列表。
template <typename PushOp>
void KVStore::_pushList(const std::string& key, const std::string& value, PushOp push) {
    auto mapIt = store_.find(key);
    if (mapIt != store_.end()) {
        auto listIt = mapIt->second;
        auto* lst = std::get_if<ListType>(&listIt->second);
        if (!lst) throw std::runtime_error("WRONGTYPE: key holds non-list value");
        push(*lst, value);
        lru_list_.splice(lru_list_.begin(), lru_list_, listIt);
    } else {
        if (store_.size() >= capacity_) _evict();
        ListType lst;
        push(lst, value);
        lru_list_.emplace_front(key, std::move(lst));
        store_[key] = lru_list_.begin();
    }
}

// ============================================================================
//  String 操作
// ============================================================================

void KVStore::put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        throw std::invalid_argument("Key must not be empty");
    }

    auto mapIt = store_.find(key);
    if (mapIt != store_.end()) {
        // 键已存在 → 更新值 + 提升到 LRU 头部
        auto listIt = mapIt->second;
        listIt->second = StringType(value);
        lru_list_.splice(lru_list_.begin(), lru_list_, listIt);
    } else {
        // 新建键值对，容量满则先淘汰
        if (store_.size() >= capacity_) _evict();
        lru_list_.emplace_front(key, StringType(value));
        store_[key] = lru_list_.begin();
    }
}

std::string KVStore::get(const std::string& key) const {
    return _getRef<StringType>(key);  // _getRef 内含 _lookup + 类型检查
}

bool KVStore::remove(const std::string& key) {
    auto mapIt = store_.find(key);
    if (mapIt == store_.end()) return false;
    lru_list_.erase(mapIt->second);   // 先删链表节点（迭代器失效前）
    store_.erase(mapIt);              // 再删 map 条目
    return true;
}

bool KVStore::clear(const std::string& key) {
    auto mapIt = store_.find(key);
    if (mapIt == store_.end()) return false;

    // 提升到 LRU 头部 + 根据类型清空内容，保留 key
    auto listIt = mapIt->second;
    lru_list_.splice(lru_list_.begin(), lru_list_, listIt);

    ComplexValue& val = listIt->second;
    if (auto* s = std::get_if<StringType>(&val))
        s->clear();
    else if (auto* lst = std::get_if<ListType>(&val))
        lst->clear();
    else if (auto* dict = std::get_if<DictType>(&val))
        dict->clear();

    return true;
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

std::string KVStore::type(const std::string& key) const {
    auto mapIt = store_.find(key);
    if (mapIt == store_.end()) return "none";
    // 不调用 _lookup：type 查询不应影响 LRU 顺序
    const auto& val = mapIt->second->second;
    if (std::holds_alternative<StringType>(val)) return "string";
    if (std::holds_alternative<ListType>(val))   return "list";
    if (std::holds_alternative<DictType>(val))   return "dict";
    return "none";
}

// ============================================================================
//  List 操作
// ============================================================================

void KVStore::lpush(const std::string& key, const std::string& value) {
    _pushList(key, value, [](ListType& lst, const std::string& v) { lst.push_front(v); });
}

void KVStore::rpush(const std::string& key, const std::string& value) {
    _pushList(key, value, [](ListType& lst, const std::string& v) { lst.push_back(v); });
}

std::string KVStore::lpop(const std::string& key) {
    auto& lst = _getRef<ListType>(key);
    if (lst.empty()) throw std::runtime_error("LPOP on empty list");
    std::string val = std::move(lst.front());
    lst.pop_front();
    return val;
}

std::string KVStore::rpop(const std::string& key) {
    auto& lst = _getRef<ListType>(key);
    if (lst.empty()) throw std::runtime_error("RPOP on empty list");
    std::string val = std::move(lst.back());
    lst.pop_back();
    return val;
}

std::vector<std::string> KVStore::lrange(const std::string& key, int start, int stop) const {
    const auto& lst = _getRef<ListType>(key);
    int sz = static_cast<int>(lst.size());
    if (sz == 0) return {};

    // 负索引：从末尾向前计数（-1 = 最后一个元素）
    if (start < 0) start += sz;
    if (stop  < 0) stop  += sz;
    if (start < 0) start = 0;
    if (stop >= sz) stop = sz - 1;
    if (start > stop) return {};

    std::vector<std::string> result;
    result.reserve(stop - start + 1);
    auto it = lst.begin();
    std::advance(it, start);                                  // list 不支持随机访问，O(n)
    for (int i = start; i <= stop && it != lst.end(); ++i, ++it) {
        result.push_back(*it);
    }
    return result;
}

size_t KVStore::llen(const std::string& key) const {
    return _getRef<ListType>(key).size();
}

// ============================================================================
//  Dict 操作
// ============================================================================

void KVStore::hset(const std::string& key, const std::string& field, const std::string& value) {
    auto mapIt = store_.find(key);
    if (mapIt != store_.end()) {
        auto listIt = mapIt->second;
        auto* dict = std::get_if<DictType>(&listIt->second);
        if (!dict) throw std::runtime_error("WRONGTYPE: key holds non-dict value");
        (*dict)[field] = value;                               // operator[] 自动 insert_or_assign
        lru_list_.splice(lru_list_.begin(), lru_list_, listIt);
    } else {
        // 自动创建空 dict
        if (store_.size() >= capacity_) _evict();
        DictType dict;
        dict[field] = value;
        lru_list_.emplace_front(key, std::move(dict));
        store_[key] = lru_list_.begin();
    }
}

std::string KVStore::hget(const std::string& key, const std::string& field) const {
    const auto& dict = _getRef<DictType>(key);
    auto it = dict.find(field);
    if (it == dict.end()) throw std::runtime_error("Field not found: " + field);
    return it->second;
}

bool KVStore::hdel(const std::string& key, const std::string& field) {
    auto& dict = _getRef<DictType>(key);
    return dict.erase(field) > 0;
}

std::vector<std::pair<std::string, std::string>> KVStore::hgetall(const std::string& key) const {
    const auto& dict = _getRef<DictType>(key);
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(dict.size());
    for (const auto& p : dict) result.push_back(p);
    return result;
}

std::vector<std::string> KVStore::hkeys(const std::string& key) const {
    const auto& dict = _getRef<DictType>(key);
    std::vector<std::string> result;
    result.reserve(dict.size());
    for (const auto& p : dict) result.push_back(p.first);
    return result;
}

size_t KVStore::hlen(const std::string& key) const {
    return _getRef<DictType>(key).size();
}

// ============================================================================
//  持久化
// ============================================================================

// 辅助：写入一个带长度前缀的字符串块  <len>\n<data>\n
static void saveString(std::ofstream& out, const std::string& s) {
    out << s.size() << '\n';
    out.write(s.data(), s.size());
    out << '\n';
}

// 文件格式（每条记录）：
//   <type>\n<key_len>\n<key>\n<序列化值>\n
//   type: S=String  L=List  D=Dict
//   String: <val_len>\n<val>\n
//   List:   <count>\n {<elem_len>\n<elem>\n} ...
//   Dict:   <count>\n {<field_len>\n<field>\n<val_len>\n<val>\n} ...

bool KVStore::saveToFile(const std::string& filename) const {
    // 先写入临时文件，成功后原子重命名，避免写入失败破坏旧数据
    std::string tmp = filename + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out.is_open()) return false;

        for (const auto& node : lru_list_) {
            const std::string& key = node.first;
            const ComplexValue& val = node.second;

            // 根据 variant 实际类型写入不同标记和序列化内容
            if (auto* s = std::get_if<StringType>(&val)) {
                out << "S\n";
                saveString(out, key);
                saveString(out, *s);
            } else if (auto* lst = std::get_if<ListType>(&val)) {
                out << "L\n";
                saveString(out, key);
                out << lst->size() << '\n';
                for (const auto& elem : *lst) saveString(out, elem);
            } else if (auto* dict = std::get_if<DictType>(&val)) {
                out << "D\n";
                saveString(out, key);
                out << dict->size() << '\n';
                for (const auto& p : *dict) {
                    saveString(out, p.first);
                    saveString(out, p.second);
                }
            }

            if (!out.good()) return false;                    // 写入中途失败 → 临时文件丢弃
        }
    }                                                         // out 析构，close 文件

    // 替换旧文件：Windows 下 rename 无法覆盖已存在文件，先删除再重命名
    std::remove(filename.c_str());
    if (std::rename(tmp.c_str(), filename.c_str()) != 0)
        return false;
    return true;
}

// 单个字符串 / 字段的最大长度（防止损坏文件导致内存耗尽）
static constexpr size_t MAX_STR_LEN = 1024 * 1024;  // 1 MB
static constexpr size_t MAX_COUNT    = 100 * 10000;  // 100 万（List/Dict 元素上限）

// 辅助：从文件读取一个带长度前缀的字符串块
static bool loadString(std::ifstream& in, std::string& s) {
    size_t len;
    if (!(in >> len)) return false;
    in.get();                                                 // 跳过 len 后的换行符
    if (!in.good()) return false;
    if (len > MAX_STR_LEN) return false;                      // 长度过大 → 拒绝
    s.resize(len);
    if (len > 0) {
        in.read(&s[0], len);
        if (!in.good()) return false;
    }
    in.get();                                                 // 跳过 value 后的换行符
    return in.good() || in.eof();
}

bool KVStore::loadFromFile(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) return false;

    decltype(lru_list_) tempList;
    MapType tempMap;
    char type;

    while (in >> type) {
        in.get();                                             // 跳过 type 后的换行符
        if (!in.good()) break;

        std::string key;
        if (!loadString(in, key)) break;

        ComplexValue val;

        // 根据类型标记反序列化
        if (type == 'S') {
            std::string s;
            if (!loadString(in, s)) break;
            val = std::move(s);
        } else if (type == 'L') {
            size_t count;
            if (!(in >> count)) break;
            in.get();                                         // 跳过 count 后的换行符
            if (count > MAX_COUNT) return false;               // 元素数量过大 → 拒绝
            ListType lst;
            for (size_t i = 0; i < count; ++i) {
                std::string elem;
                if (!loadString(in, elem)) break;
                lst.push_back(std::move(elem));
            }
            val = std::move(lst);
        } else if (type == 'D') {
            size_t count;
            if (!(in >> count)) break;
            in.get();
            if (count > MAX_COUNT) return false;
            DictType dict;
            for (size_t i = 0; i < count; ++i) {
                std::string field, fval;
                if (!loadString(in, field)) break;
                if (!loadString(in, fval)) break;
                dict[std::move(field)] = std::move(fval);
            }
            val = std::move(dict);
        } else {
            break;                                            // 未知类型标记，停止读取
        }

        tempList.emplace_back(std::move(key), std::move(val));
        tempMap[tempList.back().first] = --tempList.end();    // 指向刚插入的尾节点
    }

    if (in.bad()) return false;                               // 流损坏

    // 文件中的条目数可能超出当前 capacity，截断尾部的超额条目
    while (tempList.size() > capacity_) {
        tempMap.erase(tempList.back().first);
        tempList.pop_back();
    }

    lru_list_.swap(tempList);                                 // 原子替换
    store_.swap(tempMap);
    return true;
}
