#include "history.h"
#include <fstream>
#include <iostream>
static void check(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
int main() {
    using namespace books;
    auto dir=fs::temp_directory_path()/(L"VolturaBooks-history-"+std::to_wstring(GetCurrentProcessId()));
    try {
        fs::create_directories(dir);
        auto book=dir/L"first.epub", renamed=dir/L"renamed.epub", history=dir/L"sent.tsv";
        { std::ofstream out(book,std::ios::binary); out<<"abc"; }
        std::string hash;
        {
            BookIdentity identity(book); hash=identity.hash;
            check(hash=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 does not match known content");
            check(!sentBefore(history,hash,L"reader@example.test"),"Unsent file flagged");
            HANDLE write=CreateFileW(book.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            check(write==INVALID_HANDLE_VALUE,"Identified file could be modified before submission");
            recordSent(history,hash,L"Reader@EXAMPLE.test",L"Title with spaces & Unicode \u00e5.epub");
            check(sentBefore(history,hash,L"reader@example.test"),"Accepted book not recorded");
            check(!sentBefore(history,hash,L"another@example.test"),"Different recipient flagged");
        }
        fs::copy_file(book,renamed);
        { BookIdentity identity(renamed); check(sentBefore(history,identity.hash,L"reader@example.test"),"Renamed book not detected"); }
        { std::ofstream out(renamed,std::ios::binary); out<<"different content"; }
        { BookIdentity identity(renamed); check(!sentBefore(history,identity.hash,L"reader@example.test"),"Different content flagged"); }
        auto partial=dir/L"partial.tsv";
        { std::ofstream out(partial,std::ios::binary); out<<hash<<"\treader@example.test\t2026-01-01T00:00:00Z\tpartial"; }
        check(!sentBefore(partial,hash,L"reader@example.test"),"Partial final record accepted");
        recordSent(partial,"different",L"reader@example.test",L"other.epub");
        check(!sentBefore(partial,hash,L"reader@example.test"),"Append made an incomplete record valid");
        check(sentBefore(partial,"different",L"reader@example.test"),"Append after incomplete record lost");
        fs::remove_all(dir);
        std::cout<<"History checks passed\n"; return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n'; fs::remove_all(dir); return 1;
    }
}
