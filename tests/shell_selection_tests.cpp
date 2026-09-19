#include "interactive_test.h"
#include "shell_selection.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <thread>
constexpr wchar_t TestId[]=L"{0764E641-B32E-4DF7-90DA-08E55ADAEFA4}";
int wmain() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(!interactiveTestsEnabled())return 77;
    using Microsoft::WRL::ComPtr;
    auto root=books::fs::temp_directory_path()/(L"VolturaBooks-shell-test-"+std::to_wstring(GetCurrentProcessId())); books::fs::create_directories(root);
    std::vector<PIDLIST_ABSOLUTE> ids; std::vector<books::fs::path> files;
    for(int i=0;i<20;++i) {
        auto path=root/(L"Book "+std::to_wstring(i)+L" \u65e5\u672c\u8a9e.epub"); { std::ofstream f(path); f<<"fixture"; }
        files.push_back(path);
        PIDLIST_ABSOLUTE id=nullptr; if(FAILED(SHParseDisplayName(path.c_str(),nullptr,&id,0,nullptr))) return 1; ids.push_back(id);
    }
    auto ready=CreateEventW(nullptr,TRUE,FALSE,nullptr); std::vector<books::fs::path> received; std::exception_ptr serverError;
    std::thread server([&]{
        CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        try { received=books::receiveShellSelection(TestId,ready); } catch(...) { serverError=std::current_exception(); SetEvent(ready); }
        CoUninitialize();
    });
    if(WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) { server.join(); CloseHandle(ready); return 1; }
    ComPtr<IShellItemArray> items; auto hr=SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),const_cast<PCIDLIST_ABSOLUTE*>(ids.data()),&items);
    ComPtr<IDataObject> data; if(SUCCEEDED(hr)) hr=items->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
    CLSID clsid{}; CLSIDFromString(TestId,&clsid); ComPtr<IDropTarget> target;
    if(SUCCEEDED(hr)) hr=CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&target));
    DWORD effect=DROPEFFECT_COPY;
    if(SUCCEEDED(hr)) hr=target->Drop(data.Get(),0,{0,0},&effect);
    target.Reset(); data.Reset(); items.Reset();
    server.join(); CloseHandle(ready); bool matches=!serverError && received==files;
    for(auto id:ids) CoTaskMemFree(id);
    for(const auto& file:files) books::fs::remove(file); books::fs::remove(root);
    std::cout<<"HRESULT: "<<std::hex<<hr<<", matched: "<<matches<<"\n";
    CoUninitialize(); std::cout<<(SUCCEEDED(hr)&&matches ? "PASS: COM delivered 20 Unicode paths in order\n" : "FAIL: COM selection\n");
    return SUCCEEDED(hr)&&matches ? 0 : 1;
}
