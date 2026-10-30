#include "minidb.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
void print(const minidb::Result& result) {
    if (!result.ok) { std::cerr << result.message << '\n'; return; }
    if (!result.columns.empty()) {
        std::vector<std::size_t> widths;
        for (auto& col : result.columns) widths.push_back(std::min<std::size_t>(36, std::max<std::size_t>(col.size(), 3)));
        for (const auto& row : result.rows)
            for (std::size_t i = 0; i < row.size(); ++i) widths[i] = std::min<std::size_t>(36, std::max(widths[i], row[i].size()));
        auto line = [&]{ std::cout << '+';for(auto width:widths) std::cout << std::string(width+2,'-') << '+'; std::cout << '\n'; };
        auto output = [&](const std::vector<std::string>& row) {
            std::cout << '|';
            for (std::size_t i = 0; i < row.size(); ++i)
                std::cout << ' ' << std::left << std::setw(static_cast<int>(widths[i]))
                          << (row[i].size() > widths[i] ? row[i].substr(0, widths[i]-3) + "..." : row[i]) << " |";
            std::cout << '\n';
        };
        line();output(result.columns);line();
        for (const auto& row : result.rows) output(row);
        line();
    }
    if (!result.message.empty()) std::cout << result.message << '\n';
}
void help() {
    std::cout << "MiniDB / C++20 tiny relational storage engine\n\n"
              << "  CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);\n"
              << "  INSERT INTO users VALUES (1, 'Ada', 36);\n"
              << "  SELECT * FROM users WHERE id = 1;\n"
              << "  SELECT name, age FROM users;\n"
              << "  SELECT COUNT(*) FROM users;\n"
              << "  UPDATE users SET age = 37 WHERE id = 1;\n"
              << "  DELETE FROM users WHERE id = 1;\n"
              << "  EXPLAIN SELECT * FROM users WHERE id = 1;\n"
              << "  SHOW TABLES;  DESCRIBE users;  STATS;  COMPACT;\n\n"
              << "  .help  .tables  .stats  .quit\n"
              << "  minidb --db mydata.mdb --execute \"SELECT * FROM users;\"\n"
              << "  minidb --db mydata.mdb --file examples/demo.sql\n"
              << "Note: one statement per line in --file mode; use single-quoted strings.\n";
}
int run(minidb::Database& db, const std::string& statement) {
    if (statement.empty()) return 0;
    const auto result = db.execute(statement);
    print(result);
    return result.ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    std::string path = "minidb.mdb", query, file;
    bool hasQuery = false, hasFile = false;
    for (int i=1; i<argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--db" && i+1<argc) path=argv[++i];
        else if (arg == "--execute" && i+1<argc) { query=argv[++i];hasQuery=true; }
        else if (arg == "--file" && i+1<argc) { file=argv[++i];hasFile=true; }
        else if (arg == "--help" || arg == "-h") { help();return 0; }
        else { std::cerr << "Unknown/incomplete argument: " << arg << '\n';return 2; }
    }
    if (hasQuery && hasFile) {std::cerr<<"Choose either --execute or --file\n";return 2;}
    try {
        minidb::Database db(path);
        for (const auto& warning : db.warnings()) std::cerr << "RECOVERY WARNING: " << warning << '\n';
        if (hasQuery) return run(db,query);
        if (hasFile) {
            std::ifstream stream(file);
            if (!stream) {std::cerr<<"Cannot open script: "<<file<<'\n';return 2;}
            std::string line;int errors=0;
            while (std::getline(stream,line)) {
                if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) continue;
                const auto first=line.find_first_not_of(" \t");
                if (line.compare(first,2,"--") == 0 || line[first]=='#')continue;
                std::cout << "minidb> " << line << '\n';
                errors+=run(db,line);
                if (errors) break;
            }
            return errors?1:0;
        }
        std::cout<<"\nMINIDB / C++20  |  "<<path<<"\nType .help for examples, .quit to exit.\n";
        std::string line;
        while(std::cout << "minidb> " << std::flush,std::getline(std::cin,line)) {
            if (line==".quit" || line==".exit") break;
            if (line==".help") {help();continue;}
            if (line==".tables") {print(db.tables());continue;}
            if (line==".stats") {print(db.stats());continue;}
            run(db,line);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<"MiniDB failed to open: "<<e.what()<<'\n';
        return 2;
    }
}
