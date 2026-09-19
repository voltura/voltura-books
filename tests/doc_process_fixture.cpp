#include <windows.h>
#include <string>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 4;
    BOOL job=FALSE;if(!IsProcessInJob(GetCurrentProcess(),nullptr,&job)||!job)return 9;
    std::wstring mode=argv[1];
    if(mode==L"timeout"||mode==L"cancel"){Sleep(60000);return 0;}
    if(mode==L"memory"){
        void* allocation=VirtualAlloc(nullptr,256ull*1024*1024,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(allocation){VirtualFree(allocation,0,MEM_RELEASE);return 0;}
        return 7;
    }
    DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"PK-test-output",14,&written,nullptr);
    return mode==L"fail"?4:0;
}
