#pragma once

#include "minidb.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace minidbweb {
// The browser filesystem is an Emscripten MEMFS sandbox. The caller decides
// whether to save snapshots to browser storage or download an .mdb file.
class Session {
public:
    explicit Session(std::filesystem::path path);
    std::string execute(const std::string& sql);
    std::string snapshot() const;              // Base64-encoded native MiniDB file
    bool restore(const std::string& base64); // Validates before swapping state
    void reset();
private:
    std::filesystem::path path_;
    std::unique_ptr<minidb::Database> db_;
};
} // namespace minidbweb
