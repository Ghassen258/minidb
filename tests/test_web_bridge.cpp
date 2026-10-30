#include "web_bridge.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "minidb_web_bridge_test.mdb";
    fs::remove(path);
    fs::remove(path.string() + ".import");
    fs::remove(path.string() + ".backup");
    try {
        minidbweb::Session s(path);
        require(s.execute("CREATE TABLE notes (id INT PRIMARY KEY, content TEXT);").find("\"ok\":true") != std::string::npos, "create");
        require(s.execute("INSERT INTO notes VALUES (1, 'a \\\"quote\\\" & \\\\ newline');").find("\"ok\":true") != std::string::npos, "insert");
        const auto output = s.execute("SELECT * FROM notes WHERE id = 1;");
        require(output.find("\"columns\":[\"id\",\"content\"]") != std::string::npos, "columns");
        require(output.find("\\\\") != std::string::npos, "escaping");
        const auto backup = s.snapshot();
        require(!backup.empty(), "backup");
        s.reset();
        require(s.execute("SHOW TABLES;").find("0 table(s)") != std::string::npos, "reset");
        require(!s.restore("not a valid backup"), "invalid backup rejected");
        require(s.restore(backup), "restore");
        require(s.execute("SELECT * FROM notes WHERE id = 1;").find("1 row(s) matched") != std::string::npos, "restored query");
        require(!s.restore("TUlOSURCMQoAAAAA////////"), "corrupt backup rejected");
        require(s.execute("SELECT * FROM notes WHERE id = 1;").find("1 row(s) matched") != std::string::npos, "restore failure did not lose database");
        fs::remove(path);
        std::cout << "Browser bridge backup/restore, JSON, and reset: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        fs::remove(path);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
