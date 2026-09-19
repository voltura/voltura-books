#include "doc_reader.h"
#include "reader_extract.h"
#include <iostream>
#include <fstream>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    int failures=0;
    auto check=[&](bool value,const char* name){if(!value){std::cerr<<"FAIL: "<<name<<"\n";++failures;}};
    auto no=[] {return false;};
    auto input=std::filesystem::path(argv[1]);
    auto conversion=books::convertDoc(input,no);check(conversion.document!=nullptr,"DOC conversion");
    if(conversion.document){
        auto path=conversion.document->path;
        auto text=books::extractDocxText(path,no);check(text!=nullptr,"DOCX fallback opens delete-on-close output");
        auto stream=Microsoft::WRL::Make<books::ReaderDocumentStream>();check(SUCCEEDED(stream->open(conversion.document)),"response stream opens temporary document");
        BYTE signature[2]{};ULONG read=0;check(stream->Read(signature,2,&read)==S_OK&&signature[0]=='P'&&signature[1]=='K',"DOCX signature");
        conversion.document.reset();check(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES,"stream owns temporary document");
        stream.Reset();check(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES,"temporary document deleted after final consumer");
    }
    constexpr auto helper=L"doc_process_fixture.exe";
    auto run=[&](const wchar_t* mode,DWORD timeout,uint64_t limit,uint64_t budget){return books::convertDoc(mode,no,timeout,limit,budget,helper);};
    auto budget=books::readerMemoryBudget();
    auto timed=run(L"timeout",100,1024,budget);check(!timed.document&&timed.error==6,"deadline kills helper");
    auto large=run(L"output",3000,8,budget);check(!large.document&&large.error==5,"output boundary");
    auto failed=run(L"fail",3000,1024,budget);check(!failed.document,"partial output discarded on helper failure");
    auto memory=run(L"memory",3000,1024,32ull*1024*1024);check(!memory.document&&memory.error==7,"job memory limit");
    auto contained=run(L"ok",3000,1024,budget);check(contained.document!=nullptr,"helper starts inside job");
    auto start=GetTickCount64();auto cancelled=books::convertDoc(L"cancel",[&]{return GetTickCount64()-start>100;},3000,1024,budget,helper);
    check(!cancelled.document&&GetTickCount64()-start<1500,"cancellation interrupts helper");
    check(!books::convertDoc(input,[]{return true;}).document,"cancel before dispatch");
    check(!books::convertDoc(input,no,3000,1024,budget,L"missing-helper.exe").document,"missing helper");
    CoUninitialize();std::cout<<"DOC process boundaries: "<<failures<<" failures\n";return failures?1:0;
}
