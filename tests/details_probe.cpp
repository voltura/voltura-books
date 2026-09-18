#include "cover.h"
#include <objbase.h>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    auto details=books::loadBookDetails(argv[1]);
    std::wcout<<details.title<<L"\n"<<details.author<<L"\n"<<details.publisher<<L"\n"<<details.pages<<L"\n"<<(details.savedPageCount ? 1 : 0)<<L"\n";
    CoUninitialize(); return 0;
}
