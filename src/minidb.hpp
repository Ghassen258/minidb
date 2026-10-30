#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace minidb {

using Value = std::variant<std::int64_t, std::string>;
enum class Type { Integer, Text };

struct Column {
    std::string name;
    Type type;
    bool primary = false;
};

struct Result {
    bool ok = true;
    std::string message;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

class BPlusTree {
public:
    BPlusTree();
    BPlusTree(BPlusTree&&) noexcept;
    BPlusTree& operator=(BPlusTree&&) noexcept;
    ~BPlusTree();
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    void upsert(std::int64_t key, std::size_t value);
    std::optional<std::size_t> find(std::int64_t key) const;
    std::size_t size() const { return size_; }
    std::vector<std::pair<std::int64_t, std::size_t>> entries() const;
private:
    struct Node;
    struct Split;
    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
    std::optional<Split> insert(Node& node, std::int64_t key, std::size_t value);
};

class Database {
public:
    explicit Database(std::filesystem::path file);
    Result execute(const std::string& sql);
    Result createTable(const std::string& name, const std::vector<Column>& columns);
    Result insert(const std::string& name, const std::vector<Value>& values);
    Result update(const std::string& name, std::int64_t key, const std::vector<std::pair<std::string, Value>>& fields);
    Result erase(const std::string& name, std::int64_t key);
    Result select(const std::string& name, const std::vector<std::string>& projected,
                  const std::optional<std::pair<std::string, std::int64_t>>& predicate,
                  bool count, bool explain);
    Result tables() const;
    Result describe(const std::string& name) const;
    Result stats() const;
    Result compact();
    const std::vector<std::string>& warnings() const { return warnings_; }
private:
    struct Row { std::vector<Value> values; bool alive = true; };
    struct Table {
        std::string name;
        std::vector<Column> columns;
        std::size_t pk = 0;
        std::vector<Row> rows;
        BPlusTree index;
        std::size_t live = 0;
    };
    std::filesystem::path file_;
    std::map<std::string, std::unique_ptr<Table>> tables_;
    std::vector<std::string> warnings_;
    void load();
    void append(const std::string& payload);
    void apply(const std::string& payload);
    Table& table(const std::string& name);
    const Table& table(const std::string& name) const;
    static void validateRow(const Table& t, const std::vector<Value>& values);
    static std::string recordCreate(const Table& t);
    static std::string recordValues(char operation, const Table& t, const std::vector<Value>& values);
    static std::string recordDelete(const Table& t, std::int64_t key);
    friend Result executeSql(Database&, const std::string&);
};

Result executeSql(Database&, const std::string&);
std::string valueString(const Value&);
} // namespace minidb
