#include "mail_setup.h"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    auto result=books::discoverMailSetup(argv[1]);
    std::wcout<<result.host<<L':'<<result.port<<L' '<<(result.startTls ? L"STARTTLS" : L"TLS")<<L'\n'<<result.guidance<<L'\n';
    CoUninitialize(); return result.host.empty() ? 1 : 0;
}
