#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <objbase.h>
#include <algorithm>
#include <limits>
#include "rtf_images.h"
#include <shellapi.h>
#include "rtf_protocol.h"
#include "html_bootstrap.h"

namespace {
bool transfer(HANDLE pipe,void* buffer,DWORD length,bool write) {
    auto bytes=static_cast<BYTE*>(buffer);
    while(length){DWORD count=0;bool ok=write?WriteFile(pipe,bytes,length,&count,nullptr):ReadFile(pipe,bytes,length,&count,nullptr);if(!ok||!count)return false;bytes+=count;length-=count;}return true;
}
DWORD CALLBACK stream(DWORD_PTR cookie,LPBYTE bytes,LONG capacity,LONG* count) {
    DWORD read=0;if(!ReadFile(reinterpret_cast<HANDLE>(cookie),bytes,capacity,&read,nullptr))return GetLastError();*count=static_cast<LONG>(read);return 0;
}
// Pictures may use storage, but arbitrary embedded OLE classes are never created.
class Objects final:public IRichEditOleCallback {
    ULONG references=1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=IID_IUnknown&&id!=IID_IRichEditOleCallback)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--references;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetNewStorage(IStorage** storage)override{Microsoft::WRL::ComPtr<ILockBytes> bytes;auto hr=CreateILockBytesOnHGlobal(nullptr,TRUE,&bytes);return FAILED(hr)?hr:StgCreateDocfileOnILockBytes(bytes.Get(),STGM_READWRITE|STGM_SHARE_EXCLUSIVE|STGM_CREATE,0,storage);}
    HRESULT STDMETHODCALLTYPE GetInPlaceContext(IOleInPlaceFrame**,IOleInPlaceUIWindow**,LPOLEINPLACEFRAMEINFO)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE ShowContainerUI(BOOL)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE QueryInsertObject(LPCLSID cls,IStorage*,LONG)override {
        // Static DIB, metafile and enhanced metafile picture classes only.
        return cls&&(*cls==CLSID_StaticDib||*cls==CLSID_StaticMetafile||*cls==CLSID_Picture_Dib||*cls==CLSID_Picture_Metafile||*cls==CLSID_Picture_EnhMetafile)?S_OK:E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE DeleteObject(IOleObject*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE QueryAcceptData(IDataObject*,CLIPFORMAT*,DWORD,BOOL,HGLOBAL)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetClipboardData(CHARRANGE*,DWORD,IDataObject**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDragDropEffect(BOOL,DWORD,LPDWORD effect)override{*effect=DROPEFFECT_NONE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetContextMenu(WORD,IOleObject*,CHARRANGE*,HMENU* menu)override{*menu=nullptr;return S_OK;}
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    bool html=argc==3&&std::wstring(argv[1])==L"--html";
    std::filesystem::path path;if(argc==2)path=argv[1];else if(html)path=argv[2];LocalFree(argv);
    if(path.empty())return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(html)return books::bootstrapHtmlReader(path);
    if(FAILED(OleInitialize(nullptr)))return 3;
    auto library=LoadLibraryExW(L"Msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)return 3;
    auto view=CreateWindowExW(0,MSFTEDIT_CLASS,L"",WS_POPUP|ES_MULTILINE|ES_READONLY,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!view)return 3;
    auto objects=new Objects;SendMessageW(view,EM_SETOLECALLBACK,0,reinterpret_cast<LPARAM>(objects));objects->Release();
    SendMessageW(view,EM_SETUNDOLIMIT,0,0);SendMessageW(view,EM_EXLIMITTEXT,0,LONG_MAX);
    books::RtfImageStream file;if(!file.prepare(path))return 4;
    EDITSTREAM input{reinterpret_cast<DWORD_PTR>(file.get()),0,stream};
    SendMessageW(view,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&input));
    file.close();
    if(input.dwError)return 4;
    GETTEXTLENGTHEX query{GTL_NUMCHARS,1200};auto length=static_cast<LONG>(SendMessageW(view,EM_GETTEXTLENGTHEX,reinterpret_cast<WPARAM>(&query),0));
    if(length<=0)return 4;
    auto in=GetStdHandle(STD_INPUT_HANDLE),out=GetStdHandle(STD_OUTPUT_HANDLE);
    books::RtfRequest request;
    while(transfer(in,&request,sizeof(request),false)) {
        if(request.width<=0||request.height<=0||request.width>8192||request.height>8192||static_cast<uint64_t>(request.width)*request.height*4>64*1024*1024||request.dpi<48||request.dpi>768||request.anchor<0||request.anchor>=length)break;
        auto dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),request.width,-request.height,1,32,BI_RGB};
        void* pixels=nullptr;auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap){DeleteDC(dc);break;}
        SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
        auto old=SelectObject(dc,bitmap);RECT bounds{0,0,request.width,request.height};FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        FORMATRANGE format{};format.hdc=format.hdcTarget=dc;
        // Lay out on the document's paper, then fit that whole page into the
        // available bitmap. Rich Edit's twip conversion uses the actual DC DPI;
        // assuming 96 here double-scales and clips on high-DPI displays.
        const auto scale=(std::min)(static_cast<double>(request.width)/file.pageWidth,static_cast<double>(request.height)/file.pageHeight);
        const int pagePixelsX=(std::max)(1,static_cast<int>(file.pageWidth*scale));
        const int pagePixelsY=(std::max)(1,static_cast<int>(file.pageHeight*scale));
        SetMapMode(dc,MM_ANISOTROPIC);
        SetWindowExtEx(dc,MulDiv(file.pageWidth,GetDeviceCaps(dc,LOGPIXELSX),1440),MulDiv(file.pageHeight,GetDeviceCaps(dc,LOGPIXELSY),1440),nullptr);
        SetViewportExtEx(dc,pagePixelsX,pagePixelsY,nullptr);
        SetViewportOrgEx(dc,(request.width-pagePixelsX)/2,(request.height-pagePixelsY)/2,nullptr);
        format.rcPage={0,0,file.pageWidth,file.pageHeight};
        format.rc=format.rcPage;int pad=240;InflateRect(&format.rc,-pad,-pad);
        if(request.previous){LONG begin=0;while(begin<request.anchor){format.chrg={begin,-1};auto end=static_cast<LONG>(SendMessageW(view,EM_FORMATRANGE,FALSE,reinterpret_cast<LPARAM>(&format)));if(end<=begin)break;if(end>=request.anchor){request.anchor=begin;break;}begin=end;}}
        format.chrg={request.anchor,-1};
        auto next=static_cast<LONG>(SendMessageW(view,EM_FORMATRANGE,TRUE,reinterpret_cast<LPARAM>(&format)));
        next=(std::min)(next,length); // Rich Edit may include its final paragraph mark.
        SendMessageW(view,EM_FORMATRANGE,0,0);
        GdiFlush();books::RtfResponse response{request.width,request.height,next,length,request.anchor};
        bool ok=next>request.anchor&&transfer(out,&response,sizeof(response),true)&&transfer(out,pixels,request.width*request.height*4,true);
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);if(!ok)break;
    }
    DestroyWindow(view);FreeLibrary(library);OleUninitialize();return 0;
}
