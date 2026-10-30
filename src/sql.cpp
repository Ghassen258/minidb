#include "minidb.hpp"
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace minidb {
namespace {
struct Token {
    enum Kind { Word, Number, String, Symbol, End } kind;
    std::string text;
};
std::string upper(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> out;
    for (std::size_t i = 0; i < sql.size();) {
        const auto c = static_cast<unsigned char>(sql[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (std::isalpha(c) || c == '_') {
            const auto begin = i++;
            while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
            out.push_back({Token::Word, sql.substr(begin, i - begin)});
        } else if (std::isdigit(c) || (c == '-' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
            const auto begin = i++;
            while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
            out.push_back({Token::Number, sql.substr(begin, i - begin)});
        } else if (c == '\'') {
            ++i;
            std::string value;
            bool closed = false;
            while (i < sql.size()) {
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') { value += '\''; i += 2; }
                    else { ++i; closed = true; break; }
                } else value += sql[i++];
            }
            if (!closed) throw std::runtime_error("unterminated string literal");
            out.push_back({Token::String, std::move(value)});
        } else if (c == '(' || c == ')' || c == ',' || c == '=' || c == '*' || c == ';') {
            out.push_back({Token::Symbol, std::string(1, static_cast<char>(c))});
            ++i;
        } else throw std::runtime_error(std::string("unexpected SQL character: ") + static_cast<char>(c));
        if (out.size() > 10000) throw std::runtime_error("SQL statement is too long");
    }
    out.push_back({Token::End, ""});
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& query) : tokens_(tokenize(query)) {}
    Result run(Database& db) {
        bool explain = keyword("EXPLAIN");
        Result result;
        if (keyword("CREATE")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            needKeyword("TABLE");
            auto name = identifier(); need("(");
            std::vector<Column> columns;
            do {
                auto col = identifier();
                Type type;
                if (keyword("INT") || keyword("INTEGER")) type = Type::Integer;
                else if (keyword("TEXT")) type = Type::Text;
                else fail("expected INT or TEXT type");
                const bool primary = keyword("PRIMARY");
                if (primary) needKeyword("KEY");
                columns.push_back({std::move(col), type, primary});
            } while (match(","));
            need(")");
            done();
            return db.createTable(name, columns);
        }
        if (keyword("INSERT")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            needKeyword("INTO");
            const auto name = identifier(); needKeyword("VALUES"); need("(");
            std::vector<Value> values;
            do { values.push_back(value()); } while (match(","));
            need(")"); done();
            return db.insert(name, values);
        }
        if (keyword("SELECT")) {
            bool count = false;
            std::vector<std::string> projected;
            if (keyword("COUNT")) { need("(");need("*");need(")");count = true; }
            else if (!match("*")) {
                do { projected.push_back(identifier()); } while (match(","));
            }
            needKeyword("FROM");
            const auto name = identifier();
            const auto where = predicate();
            done();
            return db.select(name, projected, where, count, explain);
        }
        if (keyword("DELETE")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            needKeyword("FROM"); const auto name = identifier();
            const auto where = predicate();
            if (!where) fail("DELETE requires WHERE primary_key = integer");
            done();
            // Validate the predicate column against the schema to avoid deleting via a non-key field.
            const auto schema = db.describe(name);
            std::string primary;
            for (const auto& row : schema.rows) if (row[2] == "YES") primary = row[0];
            if (upper(where->first) != upper(primary)) fail("DELETE WHERE must reference primary key " + primary);
            return db.erase(name, where->second);
        }
        if (keyword("UPDATE")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            const auto name = identifier(); needKeyword("SET");
            std::vector<std::pair<std::string,Value>> fields;
            do { auto field = identifier(); need("=");fields.emplace_back(std::move(field), value()); } while (match(","));
            const auto where = predicate();
            if (!where) fail("UPDATE requires WHERE primary_key = integer");
            done();
            const auto schema = db.describe(name);
            std::string primary;
            for (const auto& row : schema.rows) if (row[2] == "YES") primary = row[0];
            if (upper(where->first) != upper(primary)) fail("UPDATE WHERE must reference primary key " + primary);
            return db.update(name, where->second, fields);
        }
        if (keyword("SHOW")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            needKeyword("TABLES"); done(); return db.tables();
        }
        if (keyword("DESCRIBE")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            auto name = identifier();done();return db.describe(name);
        }
        if (keyword("STATS")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            done();return db.stats();
        }
        if (keyword("COMPACT")) {
            if (explain) fail("EXPLAIN supports SELECT only");
            done();return db.compact();
        }
        fail("unknown or unsupported SQL statement");
    }
private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    const Token& current() const { return tokens_[pos_]; }
    [[noreturn]] static void fail(const std::string& msg) { throw std::runtime_error(msg); }
    bool match(std::string_view symbol) {
        if (current().kind == Token::Symbol && current().text == symbol) { ++pos_; return true; }
        return false;
    }
    bool keyword(const std::string& key) {
        if (current().kind == Token::Word && upper(current().text) == key) { ++pos_; return true; }
        return false;
    }
    void need(std::string_view symbol) { if (!match(symbol)) fail("expected '" + std::string(symbol) + "'"); }
    void needKeyword(const std::string& key) { if (!keyword(key)) fail("expected " + key); }
    std::string identifier() {
        if (current().kind != Token::Word) fail("expected identifier");
        auto word = current().text;
        ++pos_;
        return word;
    }
    Value value() {
        if (current().kind == Token::String) { auto result = current().text; ++pos_; return result; }
        if (current().kind == Token::Number) return integer();
        fail("expected integer or single-quoted text value");
    }
    std::int64_t integer() {
        if (current().kind != Token::Number) fail("expected integer literal");
        const auto token = current().text;
        ++pos_;
        std::int64_t result{};
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), result);
        if (error != std::errc{} || end != token.data() + token.size()) fail("integer outside signed 64-bit range");
        return result;
    }
    std::optional<std::pair<std::string, std::int64_t>> predicate() {
        if (!keyword("WHERE")) return std::nullopt;
        auto name = identifier();need("=");auto n = integer();
        return std::make_pair(std::move(name), n);
    }
    void done() {
        match(";");
        if (current().kind != Token::End) fail("unexpected tokens after statement");
    }
};
} // namespace
Result executeSql(Database& db, const std::string& statement) {
    return Parser(statement).run(db);
}
} // namespace minidb
