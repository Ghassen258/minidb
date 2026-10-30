#include "minidb.hpp"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;

int main(int argc,char** argv) {
    int rows=5000;
    if(argc>1){try{rows=std::stoi(argv[1]);}catch(...){std::cerr<<"Expected row count\n";return 1;}}
    if(rows<10||rows>100000){std::cerr<<"Choose 10..100000 rows\n";return 1;}
    const auto dbFile=fs::temp_directory_path()/"minidb_benchmark.mdb";
    if(fs::exists(dbFile)){std::cerr<<"Refusing to overwrite "<<dbFile<<"; delete it before benchmarking.\n";return 1;}
    try {
        minidb::Database db(dbFile);
        auto created=db.execute("CREATE TABLE bench (id INT PRIMARY KEY, name TEXT, score INT);");
        if(!created.ok)throw std::runtime_error(created.message);
        const auto insertStart=Clock::now();
        for(int i=0;i<rows;++i){
            auto r=db.insert("bench",{static_cast<std::int64_t>(i),std::string("row-")+std::to_string(i),static_cast<std::int64_t>(i%100)});
            if(!r.ok)throw std::runtime_error(r.message);
        }
        const auto insertEnd=Clock::now();
        constexpr int queries=300;
        std::size_t sink=0;
        const auto lookupStart=Clock::now();
        for(int i=0;i<queries;++i){
            auto r=db.select("bench",{},std::make_optional(std::make_pair(std::string("id"),static_cast<std::int64_t>((i*7919)%rows))),false,false);
            sink+=r.rows.size();
        }
        const auto lookupEnd=Clock::now();
        const auto scanStart=Clock::now();
        for(int i=0;i<30;++i){
            auto r=db.select("bench",{},std::nullopt,false,false);
            sink+=r.rows.size();
        }
        const auto scanEnd=Clock::now();
        auto millis=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        std::cout<<"MiniDB benchmark / "<<rows<<" rows\n"
                 <<"Insert + append persistence: "<<std::fixed<<std::setprecision(2)<<millis(insertStart,insertEnd)<<" ms total\n"
                 <<queries<<" primary-key B+tree lookups: "<<millis(lookupStart,lookupEnd)<<" ms total\n"
                 <<"30 full-table scans (all rows materialized): "<<millis(scanStart,scanEnd)<<" ms total\n"
                 <<"Database file: "<<fs::file_size(dbFile)<<" bytes; guard: "<<sink<<"\n"
                 <<"Note: lookup and scan return different result sizes; not an apples-to-apples SQL benchmark.\n";
    } catch(const std::exception& e){std::cerr<<"Benchmark failed: "<<e.what()<<'\n';fs::remove(dbFile);return 1;}
    fs::remove(dbFile);
}
