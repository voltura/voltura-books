#include "core.h"
#include <shlobj.h>
#include <wincred.h>
#include <fstream>
#include <iostream>

// Exercise production cleanup in a temporary profile without touching credentials.
static std::wstring profile;
static HRESULT fixtureFolder(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR* value) {
    const auto bytes=(profile.size()+1)*sizeof(wchar_t);
    *value=static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if(!*value) return E_OUTOFMEMORY;
    memcpy(*value,profile.c_str(),bytes); return S_OK;
}
static BOOL fixtureDeleteCredential(LPCWSTR,DWORD,DWORD) { return TRUE; }
#define SHGetKnownFolderPath fixtureFolder
#define CredDeleteW fixtureDeleteCredential
#include "../src/settings.cpp"
#undef CredDeleteW
#undef SHGetKnownFolderPath

int main() {
    auto root=books::fs::temp_directory_path()/(L"VolturaBooks-cleanup-"+std::to_wstring(GetCurrentProcessId()));
    profile=root.wstring();
    auto data=books::localData();
    books::fs::create_directories(data/L"Updates");
    for(auto file:{data/L"settings.ini",data/L"settings.pending.ini",data/L"sent-books.tsv",data/L"Updates"/L"VolturaBooks-Setup-0.2.0-win-x64.exe"}) {
        std::ofstream stream(file); stream<<"fixture";
    }
    books::removeSettings();
    bool passed=!books::fs::exists(data);
    books::removeSettings(); // Repeated cleanup and a missing cache are harmless.
    books::fs::remove(root);
    std::cout<<(passed ? "PASS" : "FAIL")<<": settings and downloaded installers removed\n";
    return passed ? 0 : 1;
}
