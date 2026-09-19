#include "reader_extract.h"
#include <objbase.h>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    int failures=0;
    auto check=[&](bool ok,const char* name){if(!ok){std::cerr<<"FAIL: "<<name<<'\n';++failures;}};
    const std::filesystem::path root=argv[1];
    auto text=books::extractReaderText(root/L"read.docx",false,[]{return false;});
    check(text!=nullptr,"DOCX extraction");
    if(text){
        auto content=books::loadReaderText(text->path);check(content.has_value(),"UTF-16 output");
        if(content){
            for(auto expected:{L"caf\u00e9 \u65e5\u672c\u8a9e",L"First cell",L"Second cell",L"List item",L"Final DOCX paragraph"})check(content->find(expected)!=std::wstring::npos,"body text preserved");
            check(content->find(L"UNSAFE CHUNK")==std::wstring::npos,"alternative HTML excluded");
        }
        auto file=text->path;text.reset();check(!std::filesystem::exists(file),"temporary output removed");
    }
    for(auto name:{L"bad.docx",L"encrypted.docx",L"xml-bad.docx",L"dtd.docx",L"traversal.docx",L"large-xml.docx"})
        check(!books::extractDocxText(root/name,[]{return false;}),"unsupported or over-budget document rejected");
    check(!books::extractDocxText(root/L"read.docx",[]{return true;}),"immediate cancellation");
    int calls=0;check(!books::extractDocxText(root/L"read.docx",[&]{return ++calls>4;}),"mid-extraction cancellation");
    CoUninitialize();std::cout<<(failures?"FAIL":"PASS")<<": DOCX text extraction (no windows)\n";return failures?1:0;
}
