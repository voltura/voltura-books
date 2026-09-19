#pragma once
#include <windows.h>
#include <WebView2.h>
#include <wrl.h>
#include <filesystem>

namespace books {
// Create the private browser in the already-contained helper. The UI process
// subsequently connects to this same profile/browser and owns its visible view.
inline int bootstrapHtmlReader(const std::filesystem::path& profile) {
    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::Callback;
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 3;
    auto window=CreateWindowExW(0,L"Static",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    auto signal=[](HRESULT code){DWORD result=SUCCEEDED(code)?1:static_cast<DWORD>(code),written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&result,sizeof(result),&written,nullptr);};
    auto hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr,ICoreWebView2Environment* env)->HRESULT{
        if(FAILED(hr)||!env){signal(hr);return S_OK;}environment=env;
        auto created=env->CreateCoreWebView2Controller(window,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT hr,ICoreWebView2Controller* value)->HRESULT{
            if(FAILED(hr)||!value){signal(hr);return S_OK;}
            controller=value;controller->put_IsVisible(FALSE);signal(S_OK);return S_OK;
        }).Get());if(FAILED(created))signal(created);return S_OK;
    }).Get());
    if(FAILED(hr))signal(hr);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(controller)controller->Close();controller.Reset();environment.Reset();DestroyWindow(window);CoUninitialize();return 0;
}
}
