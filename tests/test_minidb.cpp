#include "minidb.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using minidb::Database;
using minidb::Result;

namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error("Assertion " + std::to_string(checks) + " failed: " + label);
}
void ok(Database& db, const std::string& sql) {
    auto result = db.execute(sql);
    check(result.ok, sql + " -> " + result.message);
}
void bad(Database& db, const std::string& sql) {
    auto result = db.execute(sql);
    check(!result.ok, "should reject: " + sql);
    check(result.message.rfind("ERROR:", 0) == 0, "errors have a meaningful prefix");
}
Result query(Database& db, const std::string& sql) {
    auto r=db.execute(sql);
    check(r.ok, sql + " -> " + r.message);
    return r;
}
struct Temporary {
    fs::path dir;
    Temporary() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        dir=fs::temp_directory_path()/("minidb-tests-"+std::to_string(stamp));
        fs::create_directories(dir);
    }
    ~Temporary() {std::error_code ec;fs::remove_all(dir,ec);}
    fs::path file(const char* name="test.mdb") const {return dir/name;}
};
}

int main() {
    try {
        {
            minidb::BPlusTree index;
            check(!index.find(1).has_value(),"empty lookup");
            check(index.entries().empty(),"empty leaf traversal");
            std::vector<std::int64_t> ids;
            for(int i=0;i<2500;++i)ids.push_back(i-1250);
            std::mt19937 rng(123456);
            std::shuffle(ids.begin(),ids.end(),rng);
            for(auto key:ids)index.upsert(key,static_cast<std::size_t>(key+2000));
            check(index.size()==2500,"B+ tree splits through multiple levels");
            for(std::int64_t key=-1250;key<1250;++key)
                check(index.find(key)==std::optional<std::size_t>(static_cast<std::size_t>(key+2000)),"B+ tree lookup " + std::to_string(key));
            check(!index.find(1250),"missing high key");
            check(!index.find(-1251),"missing low key");
            auto all=index.entries();
            check(all.size()==2500,"sibling leaf scan visits all values");
            check(std::is_sorted(all.begin(),all.end()),"ordered leaf iteration");
            index.upsert(10,55555);
            check(index.size()==2500,"replacing a key does not create duplicates");
            check(index.find(10)==std::optional<std::size_t>(55555),"replaced key searchable");
            auto moved=std::move(index);
            check(moved.find(10)==std::optional<std::size_t>(55555),"move ownership safely");
        }
        Temporary temp;
        auto file=temp.file();
        {
            Database db(file);
            check(fs::exists(file),"database created");
            ok(db,"CREATE TABLE Users (id INT PRIMARY KEY, name TEXT, age INT);");
            ok(db,"CREATE TABLE logs (seq INT PRIMARY KEY, details TEXT);");
            auto tables=query(db,"SHOW TABLES;");
            check(tables.rows.size()==2,"multiple tables");
            auto desc=query(db,"DESCRIBE users;");
            check(desc.rows.size()==3,"schema columns");
            check(desc.rows[0][2]=="YES","primary key metadata");
            ok(db,"insert into USERS values (10, 'Ada', 36);");
            ok(db,"INSERT INTO users VALUES (-2, 'Grace Hopper', 85)");
            ok(db,"INSERT INTO logs VALUES (1, 'Hello');");
            ok(db,"INSERT INTO users VALUES (1000, 'O''Brien', 31);");
            check(query(db,"SELECT COUNT(*) FROM users;").rows[0][0]=="3","count rows");
            auto select=query(db,"SELECT name, age FROM users WHERE id = 10;");
            check(select.rows.size()==1,"indexed SELECT one row");
            check(select.columns[0]=="name" && select.rows[0][0]=="Ada","projection");
            check(select.rows[0][1]=="36","typed value returns text in result");
            check(query(db,"SELECT name FROM users WHERE id = 1000;").rows[0][0]=="O'Brien","escaped SQL apostrophe");
            check(query(db,"SELECT * FROM users WHERE id = 0;").rows.empty(),"missing lookup");
            check(query(db,"SELECT * FROM users;").rows.size()==3,"full scan");
            check(query(db,"EXPLAIN SELECT * FROM users WHERE id = -2;").message.find("B+ TREE")!=std::string::npos,"explain indexed plan");
            check(query(db,"EXPLAIN SELECT * FROM users;").message.find("FULL TABLE SCAN")!=std::string::npos,"explain scan plan");
            ok(db,"UPDATE users SET age = 37, name = 'Ada L.' WHERE id = 10;");
            check(query(db,"SELECT age FROM users WHERE id = 10;").rows[0][0]=="37","update value");
            check(query(db,"SELECT name FROM users WHERE id = 10;").rows[0][0]=="Ada L.","multi field update");
            ok(db,"DELETE FROM users WHERE id = -2;");
            check(query(db,"SELECT COUNT(*) FROM users;").rows[0][0]=="2","count after delete");
            check(query(db,"SELECT * FROM users WHERE id = -2;").rows.empty(),"tombstone hidden by index");
            ok(db,"INSERT INTO users VALUES (-2, 'Reborn', -1);");
            check(query(db,"SELECT name FROM users WHERE id = -2;").rows[0][0]=="Reborn","reinsert after delete");
            check(query(db,"SELECT COUNT(*) FROM users WHERE id = 10;").rows[0][0]=="1","count with index predicate");
            check(query(db,"SELECT COUNT(*) FROM users WHERE id = 9999;").rows[0][0]=="0","count missing");
            ok(db,"DELETE FROM users WHERE id = 999;");
            ok(db,"UPDATE users SET age = 9 WHERE id = 999;");
            check(query(db,"SELECT * FROM logs;").rows.size()==1,"other table stays intact");
            bad(db,"INSERT INTO users VALUES (10, 'Duplicate', 10);");
            bad(db,"INSERT INTO users VALUES (44, 'Missing age');");
            bad(db,"INSERT INTO users VALUES (44, 55, 10);");
            bad(db,"INSERT INTO users VALUES (44, 'X', 'wrong');");
            bad(db,"INSERT INTO unknown VALUES (1);");
            bad(db,"SELECT unknown FROM users;");
            bad(db,"SELECT * FROM users WHERE age = 10;");
            bad(db,"DELETE FROM users;");
            bad(db,"UPDATE users SET age = 8;");
            bad(db,"DELETE FROM users WHERE age = 10;");
            bad(db,"UPDATE users SET id = 4 WHERE id = 10;");
            bad(db,"UPDATE users SET unknown = 4 WHERE id = 10;");
            bad(db,"UPDATE users SET age = 'wrong' WHERE id = 10;");
            bad(db,"UPDATE users SET nonsense = 7 WHERE id = 9999;");
            bad(db,"UPDATE users SET age = 'wrong' WHERE id = 9999;");
            bad(db,"CREATE TABLE users (id INT PRIMARY KEY);");
            bad(db,"CREATE TABLE nope (id INT, data TEXT);");
            bad(db,"CREATE TABLE nope (id TEXT PRIMARY KEY);");
            bad(db,"CREATE TABLE nope (id INT PRIMARY KEY, id TEXT);");
            bad(db,"CREATE TABLE nope (id INT PRIMARY KEY, name INT PRIMARY KEY);");
            bad(db,"SELECT * FROM users; SELECT * FROM logs;");
            bad(db,"SELECT * FROM users WHERE id = 9223372036854775808;");
            bad(db,"INSERT INTO users VALUES (4, 'unterminated, 2);");
            bad(db,"EXPLAIN INSERT INTO users VALUES (3, 'x', 2);");
            bad(db,"DROP TABLE users;");
            const auto stats=query(db,"STATS;");
            check(stats.rows.size()==2,"stats for all tables");
            check(stats.rows[1][1]=="3","live row count accurate");
            check(std::stoull(stats.rows[1][2])>3,"obsolete versions tracked");
            auto before=fs::file_size(file);
            ok(db,"COMPACT;");
            check(fs::file_size(file)<before,"compaction shrinks log");
            check(query(db,"SELECT name FROM users WHERE id = -2;").rows[0][0]=="Reborn","compaction preserves latest version");
            check(query(db,"STATS;").rows[1][2]=="3","compaction removes old row versions");
        }
        {
            Database reopened(file);
            check(reopened.warnings().empty(),"clean recovery");
            check(query(reopened,"SHOW TABLES;").rows.size()==2,"persist table schemas");
            check(query(reopened,"SELECT age FROM users WHERE id = 10;").rows[0][0]=="37","persist updates");
            check(query(reopened,"SELECT name FROM users WHERE id = -2;").rows[0][0]=="Reborn","persist reinsert");
            check(query(reopened,"SELECT * FROM logs WHERE seq = 1;").rows.size()==1,"persist multiple tables");
        }
        {
            std::ofstream out(file,std::ios::binary|std::ios::app);
            out.put('\x04');out.put('\x00');out.put('\x00'); // interrupted record header
        }
        const auto interruptedSize=fs::file_size(file);
        {
            Database recovered(file);
            check(recovered.warnings().size()==1,"partial tail recovery reported");
            check(fs::file_size(file)==interruptedSize-3,"incomplete tail truncated");
            check(query(recovered,"SELECT COUNT(*) FROM users;").rows[0][0]=="3","recovery retains committed rows");
            ok(recovered,"INSERT INTO users VALUES (200, 'After Recovery', 22);");
        }
        {
            Database again(file);
            check(query(again,"SELECT name FROM users WHERE id = 200;").rows[0][0]=="After Recovery","append works after recovery");
        }
        {
            auto badFile=temp.file("corrupt.mdb");
            fs::copy_file(file,badFile);
            std::fstream corrupt(badFile,std::ios::binary|std::ios::in|std::ios::out);
            corrupt.seekp(17);corrupt.put('\x7f');corrupt.close();
            bool threw=false;
            try{Database shouldFail(badFile);}catch(const std::runtime_error&){threw=true;}
            check(threw,"checksum corruption causes explicit error");
        }
        {
            // Simulate power loss after COMPACT renamed the database to .bak,
            // before the replacement .tmp could be installed.
            auto interrupted=temp.file("interrupted.mdb");
            {
                Database original(interrupted);
                ok(original,"CREATE TABLE events (id INT PRIMARY KEY, event TEXT);");
                ok(original,"INSERT INTO events VALUES (1, 'Recover me');");
            }
            auto backup=interrupted;
            backup += ".bak";
            fs::rename(interrupted,backup);
            {
                Database recovered(interrupted);
                check(recovered.warnings().size()==1,"interrupted compaction warning");
                check(query(recovered,"SELECT event FROM events WHERE id = 1;").rows[0][0]=="Recover me","restore from backup after failed compaction");
            }
            check(!fs::exists(backup),"restored backup path removed");
        }
        {
            auto nested=temp.dir/"nested"/"creation.mdb";
            Database nestedDb(nested);
            check(fs::exists(nested),"nested storage directory automatically created");
        }
        std::cout << "PASS: " << checks << " assertions (B+ tree, parser, CRUD, disk recovery, compaction, validation)\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    return 0;
}
