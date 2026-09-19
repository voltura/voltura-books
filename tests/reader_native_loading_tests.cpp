#include "../src/reader.cpp"
#include <iostream>
namespace books {struct ReaderTestAccess{static auto get(Reader& reader){return reader.impl;}};}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    auto foreground=GetForegroundWindow();
    // Deliberately hidden parent: these tests never show or activate a window.
    HWND parent=CreateWindowW(L"Static",L"Non-UI reader test",WS_OVERLAPPED,0,0,600,800,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND cover=CreateWindowW(L"Static",L"Cover",WS_CHILD,0,0,580,760,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    int failures=0;
    auto check=[&](bool value,const char* label){if(!value){++failures;std::cerr<<"FAIL: "<<label<<"\n";}};
    auto pump=[&](const std::function<bool()>& done){auto start=GetTickCount64();while(!done()&&GetTickCount64()-start<15000){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);}return done();};
    {
        books::Reader reader(parent,cover,[]{});auto state=books::ReaderTestAccess::get(reader);state->fallbackChoice=[]{return 0;};
        auto status=state->status.handle();
        check((GetWindowLongPtrW(status,GWL_STYLE)&WS_POPUP)!=0,"status owns an independent popup surface");
        check((GetWindowLongPtrW(status,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,"status cannot steal focus");
        check(GetWindow(status,GW_OWNER)==parent,"status remains owned by reader dialog");
        bool statusShown=false;
        auto statusObserver=+[](HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)->LRESULT{
            if(message==WM_SHOWWINDOW&&wp)*reinterpret_cast<bool*>(data)=true;
            if(message==WM_WINDOWPOSCHANGING&&(reinterpret_cast<WINDOWPOS*>(lp)->flags&SWP_SHOWWINDOW))*reinterpret_cast<bool*>(data)=true;
            return DefSubclassProc(window,message,wp,lp);
        };
        SetWindowSubclass(status,statusObserver,99,reinterpret_cast<DWORD_PTR>(&statusShown));
        state->status.show(L"Opening document…");
        check(!IsWindowVisible(status)&&!statusShown,"hidden owner never presents status popup");
        state->status.hide();
        std::filesystem::path folder=argv[1];
        for(auto file:{L"read.txt",L"read.rtf",L"read.pdf"}){
            reader.select(folder/file);if(state->pdf)reader.previewReady(3,true);
            reader.navigate(1);check(state->loading&&state->activity.visible(GetTickCount64()),"opening immediate across native readers");
            auto version=state->generation;reader.navigate(1);check(state->generation==version,"busy navigation rejected");
            check(pump([&]{return !state->loading;}),"native completion");check(state->active,"native content ready");
            check(!state->activity.visible(GetTickCount64()),"completion removes indicator");
            auto token=state->activity.identity;
            state->request(state->page,false,false,true);check(state->activity.kind==books::ReaderOperation::View,"view loading category");
            check(!state->activity.finish(token),"stale completion cannot finish view");
            reader.fullscreen(false);check(pump([&]{return !state->loading;}),"fullscreen exit remains available while loading");
            state->request(state->page);reader.select(folder/L"read.txt");check(!state->loading,"selection cancels native loading");
        }
        reader.select(folder/L"read.doc");reader.navigate(1);auto operation=state->activity.identity;
        reader.select(folder/L"read.txt");check(!state->activity.finish(operation)&&!state->loading,"selection cancels DOC conversion identity");
        state->beginLoading(books::ReaderOperation::Page);state->failure();check(!state->loading&&!state->activity.visible(GetTickCount64()),"failure clears status before recovery");
        reader.select(folder/L"image.png");reader.previewReady(0,true);reader.imageLoading(10);reader.imageLoading(11);reader.imageLoaded(10);check(state->loading,"stale image decode ignored");reader.imageLoaded(11);check(!state->loading,"current image decode completes");
        reader.select(folder/L"read.txt");reader.fullscreen(true);check(!state->loading&&!state->active,"cover fullscreen does not open reader");
        check(!IsWindowVisible(parent)&&!IsWindowVisible(status)&&!statusShown&&GetForegroundWindow()==foreground,"desktop remains untouched throughout loading");
        RemoveWindowSubclass(status,statusObserver,99);
    }
    DestroyWindow(parent);CoUninitialize();std::cout<<"Native loading integration: "<<failures<<" failures\n";return failures?1:0;
}
