#include "web_bridge.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
#endif

namespace minidbweb {
namespace {
constexpr std::size_t MAX_SNAPSHOT_BYTES = 2 * 1024 * 1024;
constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string quoted(const std::string& value) {
    std::string out = "\"";
    constexpr char HEX[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32) {
                    out += "\\u00";
                    out += HEX[c >> 4];
                    out += HEX[c & 15];
                } else out.push_back(static_cast<char>(c));
        }
    }
    return out + '"';
}

std::string encode(const minidb::Result& result) {
    std::string json = "{\"ok\":";
    json += result.ok ? "true" : "false";
    json += ",\"message\":" + quoted(result.message) + ",\"columns\":[";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i) json.push_back(',');
        json += quoted(result.columns[i]);
    }
    json += "],\"rows\":[";
    for (std::size_t i = 0; i < result.rows.size(); ++i) {
        if (i) json.push_back(',');
        json.push_back('[');
        for (std::size_t j = 0; j < result.rows[i].size(); ++j) {
            if (j) json.push_back(',');
            json += quoted(result.rows[i][j]);
        }
        json.push_back(']');
    }
    return json + "]}";
}

std::string base64Encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += ALPHABET[a >> 2];
        out += ALPHABET[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? ALPHABET[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? ALPHABET[c & 63] : '=';
    }
    return out;
}

std::string base64Decode(const std::string& encoded) {
    if (encoded.empty() || encoded.size() % 4 || encoded.size() / 4 * 3 > MAX_SNAPSHOT_BYTES + 3)
        throw std::invalid_argument("invalid or oversized Base64 snapshot");
    std::string decoded;
    decoded.reserve(encoded.size() / 4 * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        unsigned values[4]{};
        unsigned pad = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = encoded[i + j];
            if (c == '=') { if (i + 4 != encoded.size() || j < 2) throw std::invalid_argument("invalid Base64 padding");++pad;continue; }
            if (pad) throw std::invalid_argument("invalid Base64 padding");
            if (c >= 'A' && c <= 'Z') values[j] = static_cast<unsigned>(c - 'A');
            else if (c >= 'a' && c <= 'z') values[j] = static_cast<unsigned>(c - 'a' + 26);
            else if (c >= '0' && c <= '9') values[j] = static_cast<unsigned>(c - '0' + 52);
            else if (c == '+') values[j] = 62;
            else if (c == '/') values[j] = 63;
            else throw std::invalid_argument("invalid Base64 symbol");
        }
        if (pad > 2 || (pad == 2 && (values[1] & 15)) || (pad == 1 && (values[2] & 3)))
            throw std::invalid_argument("invalid Base64 tail");
        const std::uint32_t bits = values[0] << 18 | values[1] << 12 | values[2] << 6 | values[3];
        decoded.push_back(static_cast<char>((bits >> 16) & 255));
        if (pad < 2) decoded.push_back(static_cast<char>((bits >> 8) & 255));
        if (!pad) decoded.push_back(static_cast<char>(bits & 255));
    }
    if (decoded.size() > MAX_SNAPSHOT_BYTES) throw std::invalid_argument("snapshot too large");
    return decoded;
}
} // namespace

Session::Session(std::filesystem::path path) : path_(std::move(path)), db_(std::make_unique<minidb::Database>(path_)) {}

std::string Session::execute(const std::string& sql) {
    try { return encode(db_->execute(sql)); }
    catch (const std::exception& e) { return encode({false, std::string("ERROR: ") + e.what(), {}, {}}); }
}

std::string Session::snapshot() const {
    std::ifstream file(path_, std::ios::binary);
    if (!file) throw std::runtime_error("database file cannot be opened");
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() > MAX_SNAPSHOT_BYTES) throw std::runtime_error("browser snapshot exceeds 2 MiB limit; use native MiniDB for larger files");
    return base64Encode(bytes);
}

bool Session::restore(const std::string& data) {
    auto candidatePath = path_;
    candidatePath += ".import";
    auto backupPath = path_;
    backupPath += ".backup";
    try {
        const std::string decoded = base64Decode(data);
        if (decoded.size() < 8 || decoded.compare(0, 8, "MINIDB1\n") != 0) return false;
        std::filesystem::remove(candidatePath);
        std::filesystem::remove(backupPath);
        {
            std::ofstream out(candidatePath, std::ios::binary | std::ios::trunc);
            out.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
            out.flush();
            if (!out) throw std::runtime_error("failed to write imported data");
        }
        auto validated = std::make_unique<minidb::Database>(candidatePath);
        if (!validated->warnings().empty()) { validated.reset(); std::filesystem::remove(candidatePath); return false; }
        // Preserve the current database until after the replacement parses.
        db_.reset();
        std::filesystem::rename(path_, backupPath);
        try { std::filesystem::rename(candidatePath, path_); }
        catch (...) { std::filesystem::rename(backupPath, path_); throw; }
        std::filesystem::remove(backupPath);
        db_ = std::make_unique<minidb::Database>(path_);
        return true;
    } catch (...) {
        std::filesystem::remove(candidatePath);
        if (!std::filesystem::exists(path_) && std::filesystem::exists(backupPath))
            std::filesystem::rename(backupPath, path_);
        if (!db_) db_ = std::make_unique<minidb::Database>(path_);
        return false;
    }
}

void Session::reset() {
    db_.reset();
    std::filesystem::remove(path_);
    auto backup = path_; backup += ".bak";
    auto temp = path_; temp += ".tmp";
    std::filesystem::remove(backup);
    std::filesystem::remove(temp);
    db_ = std::make_unique<minidb::Database>(path_);
}
} // namespace minidbweb

#ifdef __EMSCRIPTEN__
namespace {
minidbweb::Session& browser() {
    static minidbweb::Session session("/minidb-browser.mdb");
    return session;
}
std::string runSql(const std::string& sql) { return browser().execute(sql); }
std::string saveSnapshot() { return browser().snapshot(); }
bool loadSnapshot(const std::string& snapshot) { return browser().restore(snapshot); }
void resetDatabase() { browser().reset(); }
}
EMSCRIPTEN_BINDINGS(minidb_browser) {
    emscripten::function("runSql", &runSql);
    emscripten::function("saveSnapshot", &saveSnapshot);
    emscripten::function("loadSnapshot", &loadSnapshot);
    emscripten::function("resetDatabase", &resetDatabase);
}
#endif
