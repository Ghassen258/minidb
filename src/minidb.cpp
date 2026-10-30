#include "minidb.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace minidb {
namespace {
constexpr std::string_view MAGIC = "MINIDB1\n";
constexpr std::uint32_t MAX_RECORD = 4 * 1024 * 1024;

std::string normalized(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
void put32(std::string& data, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<char>((n >> (i * 8)) & 255));
}
void put64(std::string& data, std::int64_t n) {
    const auto bits = static_cast<std::uint64_t>(n);
    for (unsigned i = 0; i < 8; ++i) data.push_back(static_cast<char>((bits >> (i * 8)) & 255));
}
void putString(std::string& data, const std::string& s) {
    if (s.size() > 1024 * 1024) throw std::runtime_error("string exceeds 1 MiB limit");
    put32(data, static_cast<std::uint32_t>(s.size()));
    data += s;
}
struct Reader {
    std::string_view data;
    std::size_t pos = 0;
    explicit Reader(std::string_view input) : data(input) {}
    unsigned char byte() {
        if (pos == data.size()) throw std::runtime_error("truncated record field");
        return static_cast<unsigned char>(data[pos++]);
    }
    std::uint32_t u32() {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= static_cast<std::uint32_t>(byte()) << (i * 8);
        return result;
    }
    std::int64_t i64() {
        std::uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(byte()) << (i * 8);
        // C++20 std::bit_cast keeps negative values correct without out-of-range conversions.
        static_assert(sizeof(bits) == sizeof(std::int64_t));
        std::int64_t result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }
    std::string str() {
        const auto size = u32();
        if (size > 1024 * 1024 || size > data.size() - pos) throw std::runtime_error("invalid record string length");
        auto value = std::string(data.substr(pos, size));
        pos += size;
        return value;
    }
    void done() const { if (pos != data.size()) throw std::runtime_error("unexpected bytes at end of record"); }
};
std::uint32_t checksum(std::string_view s) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : s) { hash ^= c; hash *= 16777619u; }
    return hash;
}
std::string frame(std::string_view payload) {
    if (payload.size() > MAX_RECORD) throw std::runtime_error("record exceeds 4 MiB limit");
    std::string out;
    put32(out, static_cast<std::uint32_t>(payload.size()));
    put32(out, checksum(payload));
    out += payload;
    return out;
}
std::string displayType(Type t) { return t == Type::Integer ? "INT" : "TEXT"; }
Result success(std::string message) { return {true, std::move(message), {}, {}}; }
std::int64_t keyOf(const std::vector<Value>& values, std::size_t pk) {
    return std::get<std::int64_t>(values.at(pk));
}
void encodeRow(std::string& data, const std::vector<Value>& values) {
    put32(data, static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) {
        if (std::holds_alternative<std::int64_t>(value)) {
            data.push_back('I'); put64(data, std::get<std::int64_t>(value));
        } else {
            data.push_back('T'); putString(data, std::get<std::string>(value));
        }
    }
}
std::vector<Value> decodeRow(Reader& r) {
    const auto n = r.u32();
    if (n > 64) throw std::runtime_error("too many record values");
    std::vector<Value> values;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto tag = r.byte();
        if (tag == 'I') values.emplace_back(r.i64());
        else if (tag == 'T') values.emplace_back(r.str());
        else throw std::runtime_error("invalid value type in record");
    }
    return values;
}
} // namespace

std::string valueString(const Value& value) {
    if (const auto i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
    return std::get<std::string>(value);
}

Database::Database(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) throw std::invalid_argument("database file path is required");
    const auto parent = file_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    // A previous compaction may have been interrupted after the original file
    // was renamed to .bak but before the compacted replacement was installed.
    auto backup = file_;
    backup += ".bak";
    if (!std::filesystem::exists(file_) && std::filesystem::exists(backup)) {
        std::filesystem::rename(backup, file_);
        warnings_.push_back("restored original database after interrupted compaction");
    }
    if (!std::filesystem::exists(file_) || std::filesystem::file_size(file_) == 0) {
        std::ofstream output(file_, std::ios::binary | std::ios::trunc);
        output.write(MAGIC.data(), static_cast<std::streamsize>(MAGIC.size()));
        output.flush();
        if (!output) throw std::runtime_error("failed to initialize database file");
    }
    load();
}

Database::Table& Database::table(const std::string& name) {
    const auto it = tables_.find(normalized(name));
    if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
    return *it->second;
}
const Database::Table& Database::table(const std::string& name) const {
    const auto it = tables_.find(normalized(name));
    if (it == tables_.end()) throw std::runtime_error("unknown table: " + name);
    return *it->second;
}
void Database::validateRow(const Table& t, const std::vector<Value>& values) {
    if (values.size() != t.columns.size()) throw std::runtime_error("expected " + std::to_string(t.columns.size()) + " values");
    for (std::size_t i = 0; i < values.size(); ++i) {
        const bool integer = std::holds_alternative<std::int64_t>(values[i]);
        if (integer != (t.columns[i].type == Type::Integer))
            throw std::runtime_error("type mismatch for column " + t.columns[i].name + " (expects " + displayType(t.columns[i].type) + ")");
    }
}

std::string Database::recordCreate(const Table& t) {
    std::string payload = "C";
    putString(payload, t.name);
    put32(payload, static_cast<std::uint32_t>(t.columns.size()));
    for (const auto& col : t.columns) {
        putString(payload, col.name);
        payload.push_back(col.type == Type::Integer ? 'I' : 'T');
        payload.push_back(col.primary ? '\1' : '\0');
    }
    return payload;
}
std::string Database::recordValues(char operation, const Table& t, const std::vector<Value>& values) {
    std::string payload(1, operation);
    putString(payload, t.name);
    encodeRow(payload, values);
    return payload;
}
std::string Database::recordDelete(const Table& t, std::int64_t key) {
    std::string payload = "D";
    putString(payload, t.name);
    put64(payload, key);
    return payload;
}

void Database::append(const std::string& payload) {
    const auto encoded = frame(payload);
    std::ofstream output(file_, std::ios::binary | std::ios::app);
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.flush();
    if (!output) throw std::runtime_error("failed to persist record to " + file_.string());
}

void Database::apply(const std::string& payload) {
    Reader r(payload);
    const auto op = r.byte();
    const auto name = r.str();
    if (op == 'C') {
        const auto count = r.u32();
        if (count == 0 || count > 64) throw std::runtime_error("invalid schema size");
        std::vector<Column> columns;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto col = r.str();
            const auto tag = r.byte();
            const auto pk = r.byte();
            if (tag != 'I' && tag != 'T') throw std::runtime_error("invalid column type");
            if (pk > 1) throw std::runtime_error("invalid primary flag");
            columns.push_back({std::move(col), tag == 'I' ? Type::Integer : Type::Text, pk == 1});
        }
        r.done();
        if (tables_.count(name)) throw std::runtime_error("duplicate persisted table: " + name);
        auto t = std::make_unique<Table>();
        t->name = name;
        t->columns = std::move(columns);
        for (std::size_t i = 0; i < t->columns.size(); ++i)
            if (t->columns[i].primary) t->pk = i;
        tables_.emplace(name, std::move(t));
    } else if (op == 'I' || op == 'U') {
        auto values = decodeRow(r);
        r.done();
        Table& t = table(name);
        validateRow(t, values);
        const auto pk = keyOf(values, t.pk);
        const auto current = t.index.find(pk);
        if (op == 'I' && current && t.rows[*current].alive) throw std::runtime_error("duplicate primary key in transaction log");
        if (op == 'U' && (!current || !t.rows[*current].alive)) throw std::runtime_error("update references missing row");
        if (op == 'U') t.rows[*current].alive = false;
        t.rows.push_back({std::move(values), true});
        t.index.upsert(pk, t.rows.size() - 1);
        if (op == 'I') ++t.live;
    } else if (op == 'D') {
        const auto pk = r.i64();
        r.done();
        Table& t = table(name);
        const auto current = t.index.find(pk);
        if (!current || !t.rows[*current].alive) throw std::runtime_error("delete references missing row");
        t.rows[*current].alive = false;
        --t.live;
    } else throw std::runtime_error("unrecognized record operation");
}

void Database::load() {
    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("unable to open database");
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < MAGIC.size() || std::string_view(bytes.data(), MAGIC.size()) != MAGIC)
        throw std::runtime_error("not a MiniDB database or unsupported format");
    std::size_t pos = MAGIC.size();
    std::size_t good = pos;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8) break;
        Reader header(std::string_view(bytes).substr(pos, 8));
        const auto length = header.u32();
        const auto expected = header.u32();
        if (length > MAX_RECORD) throw std::runtime_error("invalid record size at offset " + std::to_string(pos));
        if (length > bytes.size() - pos - 8) break;
        const auto payload = std::string_view(bytes).substr(pos + 8, length);
        if (checksum(payload) != expected) throw std::runtime_error("record checksum mismatch at offset " + std::to_string(pos));
        try { apply(std::string(payload)); }
        catch (const std::exception& e) { throw std::runtime_error("invalid transaction at offset " + std::to_string(pos) + ": " + e.what()); }
        pos += 8 + length;
        good = pos;
    }
    if (good != bytes.size()) {
        std::filesystem::resize_file(file_, good);
        warnings_.push_back("incomplete trailing record discarded during recovery");
    }
}

Result Database::createTable(const std::string& name, const std::vector<Column>& columns) {
    const auto key = normalized(name);
    if (tables_.count(key)) throw std::runtime_error("table already exists: " + key);
    if (columns.empty() || columns.size() > 64) throw std::runtime_error("table must contain 1..64 columns");
    std::size_t primaryCount = 0;
    std::map<std::string, bool> seen;
    auto t = std::make_unique<Table>();
    t->name = key;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        Column col = columns[i];
        col.name = normalized(col.name);
        if (!seen.emplace(col.name, true).second) throw std::runtime_error("duplicate column name: " + col.name);
        if (col.primary) {
            ++primaryCount;
            if (col.type != Type::Integer) throw std::runtime_error("primary key must be INT");
            t->pk = i;
        }
        t->columns.push_back(std::move(col));
    }
    if (primaryCount != 1) throw std::runtime_error("exactly one INT PRIMARY KEY column is required");
    append(recordCreate(*t));
    tables_.emplace(key, std::move(t));
    return success("Table '" + key + "' created.");
}

Result Database::insert(const std::string& name, const std::vector<Value>& values) {
    auto& t = table(name);
    validateRow(t, values);
    const auto pk = keyOf(values, t.pk);
    const auto old = t.index.find(pk);
    if (old && t.rows[*old].alive) throw std::runtime_error("duplicate primary key: " + std::to_string(pk));
    append(recordValues('I', t, values));
    apply(recordValues('I', t, values));
    return success("1 row inserted.");
}

Result Database::erase(const std::string& name, std::int64_t key) {
    auto& t = table(name);
    const auto pos = t.index.find(key);
    if (!pos || !t.rows[*pos].alive) return success("0 rows deleted.");
    const auto payload = recordDelete(t, key);
    append(payload);
    apply(payload);
    return success("1 row deleted.");
}

Result Database::update(const std::string& name, std::int64_t key, const std::vector<std::pair<std::string, Value>>& fields) {
    auto& t = table(name);
    std::vector<std::pair<std::size_t, Value>> changes;
    std::map<std::string, bool> seen;
    for (const auto& [fieldName, value] : fields) {
        const auto field = normalized(fieldName);
        if (!seen.emplace(field, true).second) throw std::runtime_error("duplicate SET column: " + field);
        const auto it = std::find_if(t.columns.begin(), t.columns.end(), [&](const Column& c){return c.name == field;});
        if (it == t.columns.end()) throw std::runtime_error("unknown column: " + field);
        const auto index = static_cast<std::size_t>(it - t.columns.begin());
        if (index == t.pk) throw std::runtime_error("updating a primary key is not supported");
        const bool integer = std::holds_alternative<std::int64_t>(value);
        if (integer != (t.columns[index].type == Type::Integer))
            throw std::runtime_error("type mismatch for column " + field);
        changes.emplace_back(index, value);
    }
    const auto pos = t.index.find(key);
    if (!pos || !t.rows[*pos].alive) return success("0 rows updated.");
    auto updated = t.rows[*pos].values;
    for (const auto& [index, value] : changes) updated[index] = value;
    validateRow(t, updated);
    const auto payload = recordValues('U', t, updated);
    append(payload);
    apply(payload);
    return success("1 row updated.");
}

Result Database::select(const std::string& name, const std::vector<std::string>& projected,
                         const std::optional<std::pair<std::string, std::int64_t>>& predicate,
                         bool count, bool explain) {
    const auto& t = table(name);
    std::vector<std::size_t> indices;
    if (!count) {
        if (projected.empty()) {
            for (std::size_t i = 0; i < t.columns.size(); ++i) indices.push_back(i);
        } else for (const auto& field : projected) {
            const auto it = std::find_if(t.columns.begin(), t.columns.end(), [&](const Column& c){return c.name == normalized(field);});
            if (it == t.columns.end()) throw std::runtime_error("unknown column: " + field);
            indices.push_back(static_cast<std::size_t>(it - t.columns.begin()));
        }
    }
    if (predicate && normalized(predicate->first) != t.columns[t.pk].name)
        throw std::runtime_error("WHERE currently supports only primary-key equality (" + t.columns[t.pk].name + ")");
    if (explain) {
        return success(predicate ? "PLAN: B+ TREE PRIMARY KEY LOOKUP on " + t.name + "." + t.columns[t.pk].name + " = " + std::to_string(predicate->second)
                                 : "PLAN: FULL TABLE SCAN on " + t.name);
    }
    Result out;
    if (count) out.columns = {"count"};
    else for (auto i : indices) out.columns.push_back(t.columns[i].name);
    std::vector<const Row*> selected;
    if (predicate) {
        const auto position = t.index.find(predicate->second);
        if (position && t.rows[*position].alive) selected.push_back(&t.rows[*position]);
    } else {
        for (const auto& row : t.rows) if (row.alive) selected.push_back(&row);
    }
    if (count) out.rows = {{std::to_string(selected.size())}};
    else for (const auto* row : selected) {
        std::vector<std::string> values;
        for (const auto i : indices) values.push_back(valueString(row->values[i]));
        out.rows.push_back(std::move(values));
    }
    out.message = std::to_string(selected.size()) + " row(s) matched.";
    return out;
}

Result Database::tables() const {
    Result out; out.columns = {"table", "rows", "primary_key"};
    for (const auto& [name,t] : tables_)
        out.rows.push_back({name, std::to_string(t->live), t->columns[t->pk].name});
    out.message = std::to_string(out.rows.size()) + " table(s).";
    return out;
}
Result Database::describe(const std::string& name) const {
    const auto& t = table(name);
    Result out; out.columns = {"column", "type", "primary_key"};
    for (const auto& col : t.columns) out.rows.push_back({col.name, displayType(col.type), col.primary ? "YES" : ""});
    out.message = "Schema for " + t.name + '.';
    return out;
}
Result Database::stats() const {
    Result out; out.columns = {"table", "live_rows", "stored_versions", "index_keys"};
    for (const auto& [name,t] : tables_)
        out.rows.push_back({name, std::to_string(t->live), std::to_string(t->rows.size()), std::to_string(t->index.size())});
    out.message = "File size: " + std::to_string(std::filesystem::file_size(file_)) + " bytes. Indexes are rebuilt on startup.";
    return out;
}
Result Database::compact() {
    auto temp = file_;
    temp += ".tmp";
    auto backup = file_;
    backup += ".bak";
    if (std::filesystem::exists(temp) || std::filesystem::exists(backup))
        throw std::runtime_error("refusing to overwrite existing .tmp or .bak; inspect and remove those files first");
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output.write(MAGIC.data(), static_cast<std::streamsize>(MAGIC.size()));
        auto write = [&](const std::string& payload) {
            const auto bytes = frame(payload);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        for (const auto& [_,t] : tables_) {
            write(recordCreate(*t));
            for (const auto& row : t->rows) if (row.alive) write(recordValues('I', *t, row.values));
        }
        output.flush();
        if (!output) throw std::runtime_error("compaction write failed");
    }
    std::filesystem::rename(file_, backup);
    try { std::filesystem::rename(temp, file_); }
    catch (...) { std::filesystem::rename(backup, file_); throw; }
    std::filesystem::remove(backup);
    // Rebuild indexes as well, removing tombstoned keys / obsolete row versions.
    tables_.clear();
    load();
    return success("Compacted storage and rebuilt primary-key indexes.");
}

Result Database::execute(const std::string& sql) {
    try { return executeSql(*this, sql); }
    catch (const std::exception& e) { return {false, std::string("ERROR: ") + e.what(), {}, {}}; }
}
} // namespace minidb
