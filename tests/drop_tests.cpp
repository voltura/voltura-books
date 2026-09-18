#include "drop.h"
#include <shlobj.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
static HDROP payload(const std::vector<std::wstring>& files) {
    std::wstring list;
    for(const auto& file:files) { list+=file; list+=L'\0'; }
    list+=L'\0';
    auto memory=GlobalAlloc(GHND,sizeof(DROPFILES)+list.size()*sizeof(wchar_t));
    if(!memory) throw std::bad_alloc();
    auto data=static_cast<DROPFILES*>(GlobalLock(memory));
    data->pFiles=sizeof(DROPFILES); data->fWide=TRUE;
    std::memcpy(reinterpret_cast<char*>(data)+sizeof(DROPFILES),list.data(),list.size()*sizeof(wchar_t));
    GlobalUnlock(memory); return reinterpret_cast<HDROP>(memory);
}
int main() {
    const auto root=books::fs::temp_directory_path()/(L"VolturaBooks-drop-"+std::to_wstring(GetCurrentProcessId()));
    books::fs::create_directories(root);
    const auto book=root/L"A book with spaces and \u65e5\u672c\u8a9e.epub";
    { std::ofstream stream(book,std::ios::binary); stream<<"PK fixture"; }
    int failures=0;
    auto check=[&](bool passed,const char* name) { if(!passed) { std::cerr<<"FAIL "<<name<<'\n'; ++failures; } };
    try { check(books::droppedBooks(payload({book.wstring()}))==std::vector<books::fs::path>{book},"Unicode path preserved"); }
    catch(...) { check(false,"valid drop accepted"); }
    for(const auto& files:std::vector<std::vector<std::wstring>>{{},{(root/L"missing.epub").wstring()},{(root/L"book.pdf").wstring()},{root.wstring()}}) {
        bool rejected=false;
        try { books::droppedBooks(payload(files)); } catch(const std::exception&) { rejected=true; }
        check(rejected,"invalid drop rejected");
    }
    auto multiple=books::droppedBooks(payload({book.wstring(),book.wstring()}));
    check(multiple.size()==2 && multiple[0]==book && multiple[1]==book,"multiple paths preserved in order");
    books::fs::remove(book); books::fs::remove(root);
    std::cout<<(failures ? "FAILED" : "All drop checks passed")<<'\n'; return failures ? 1 : 0;
}
