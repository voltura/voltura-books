#include "interactive_test.h"
#include "shell_selection.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
constexpr wchar_t TestId[]=L"{0764E641-B32E-4DF7-90DA-08E55ADAEFA4}";
int wmain(int argc,wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(argc>=3 && std::wstring(argv[1])==L"--server") {
        auto paths=books::receiveShellSelection(TestId);
        std::ofstream out(argv[2],std::ios::binary);
        for(const auto& path:paths) out<<books::utf8(path.wstring())<<'\n';
        return 0;
    }
    if(!interactiveTestsEnabled())return 77;
    using Microsoft::WRL::ComPtr;
    auto root=books::fs::temp_directory_path()/(L"VolturaBooks-shell-test-"+std::to_wstring(GetCurrentProcessId())); books::fs::create_directories(root);
    auto output=root/L"selection.txt"; std::wstring expected; std::vector<PIDLIST_ABSOLUTE> ids; std::vector<books::fs::path> files;
    for(int i=0;i<20;++i) {
        auto path=root/(L"Book "+std::to_wstring(i)+L" \u65e5\u672c\u8a9e.epub"); { std::ofstream f(path); f<<"fixture"; }
        files.push_back(path); expected+=path.wstring()+L"\n";
        PIDLIST_ABSOLUTE id=nullptr; if(FAILED(SHParseDisplayName(path.c_str(),nullptr,&id,0,nullptr))) return 1; ids.push_back(id);
    }
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
    auto key=std::wstring(L"Software\\Classes\\CLSID\\")+TestId;
    auto command=L"\""+std::wstring(exe)+L"\" --server \""+output.wstring()+L"\"";
    HKEY reg; RegCreateKeyExW(HKEY_CURRENT_USER,(key+L"\\LocalServer32").c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&reg,nullptr);
    RegSetValueExW(reg,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))); RegCloseKey(reg);
    ComPtr<IShellItemArray> items; auto hr=SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),const_cast<PCIDLIST_ABSOLUTE*>(ids.data()),&items);
    ComPtr<IDataObject> data; if(SUCCEEDED(hr)) hr=items->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
    CLSID clsid{}; CLSIDFromString(TestId,&clsid); ComPtr<IDropTarget> target;
    if(SUCCEEDED(hr)) hr=CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&target));
    DWORD effect=DROPEFFECT_COPY;
    if(SUCCEEDED(hr)) hr=target->Drop(data.Get(),0,{0,0},&effect);
    target.Reset(); data.Reset(); items.Reset();
    bool matches=false;
    for(int i=0;i<100 && !matches;++i) { std::ifstream in(output,std::ios::binary); std::string text((std::istreambuf_iterator<char>(in)),{}); matches=text==books::utf8(expected); if(!matches) Sleep(50); }
    RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str()); for(auto id:ids) CoTaskMemFree(id);
    for(const auto& file:files) books::fs::remove(file); books::fs::remove(output); books::fs::remove(root);
    std::cout<<"HRESULT: "<<std::hex<<hr<<", matched: "<<matches<<"\n";
    CoUninitialize(); std::cout<<(SUCCEEDED(hr)&&matches ? "PASS: COM delivered 20 Unicode paths in order\n" : "FAIL: COM selection\n");
    return SUCCEEDED(hr)&&matches ? 0 : 1;
}
