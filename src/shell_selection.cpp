#include "shell_selection.h"
#include "drop.h"
#include <oleidl.h>
#include <memory>
namespace books {
namespace {
struct Selection { std::vector<fs::path> paths; bool done=false; HRESULT error=S_OK; };
class Target final : public IDropTarget {
    LONG references=1;
    std::shared_ptr<Selection> selection;
public:
    explicit Target(std::shared_ptr<Selection> value):selection(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(id==IID_IUnknown || id==IID_IDropTarget) { *out=static_cast<IDropTarget*>(this); AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { auto left=InterlockedDecrement(&references); if(!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL,DWORD* effect) override {
        FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        *effect=SUCCEEDED(data->QueryGetData(&format)) ? (*effect&DROPEFFECT_COPY) : DROPEFFECT_NONE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD* effect) override { *effect&=DROPEFFECT_COPY; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL,DWORD* effect) override {
        *effect=DROPEFFECT_NONE;
        if(selection->done) return HRESULT_FROM_WIN32(ERROR_BUSY);
        FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL}; STGMEDIUM medium{};
        auto hr=data->GetData(&format,&medium);
        if(SUCCEEDED(hr)) {
            try { selection->paths=droppedBooks(reinterpret_cast<HDROP>(medium.hGlobal),false); *effect=DROPEFFECT_COPY; }
            catch(...) { hr=E_INVALIDARG; }
            ReleaseStgMedium(&medium);
        }
        selection->error=hr; selection->done=true; return hr;
    }
};
class Factory final : public IClassFactory {
    LONG references=1; std::shared_ptr<Selection> selection;
public:
    explicit Factory(std::shared_ptr<Selection> value):selection(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(id==IID_IUnknown || id==IID_IClassFactory) { *out=static_cast<IClassFactory*>(this); AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { auto left=InterlockedDecrement(&references); if(!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID id,void** out) override {
        if(outer) return CLASS_E_NOAGGREGATION;
        auto target=new(std::nothrow) Target(selection); if(!target) return E_OUTOFMEMORY;
        auto hr=target->QueryInterface(id,out); target->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
}
std::vector<fs::path> receiveShellSelection(const wchar_t* classId) {
    CLSID id{}; if(FAILED(CLSIDFromString(classId,&id))) throw std::runtime_error("Invalid Explorer integration identifier.");
    auto selection=std::make_shared<Selection>(); auto factory=new Factory(selection); DWORD cookie=0;
    auto hr=CoRegisterClassObject(id,factory,CLSCTX_LOCAL_SERVER,REGCLS_SINGLEUSE,&cookie); factory->Release();
    if(FAILED(hr)) throw std::runtime_error("Could not receive the Explorer selection.");
    // A bounded wait covers a cancelled Shell activation without a resident process.
    const auto deadline=GetTickCount64()+60000;
    while(!selection->done && GetTickCount64()<deadline) {
        MsgWaitForMultipleObjects(0,nullptr,FALSE,100,QS_ALLINPUT);
        MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT) { selection->done=true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    CoRevokeClassObject(cookie);
    if(FAILED(selection->error)) throw std::runtime_error("Could not read the selected books. Select readable supported files and try again.");
    return std::move(selection->paths);
}
}
