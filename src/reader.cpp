#include "reader.h"
#include "theme.h"
#include "resource.h"
#include "formats.h"
#include "text_reader.h"
#include "list_scrollbar.h"
#include "rtf_reader.h"
#include "reader_extract.h"
#include "doc_reader.h"
#include "reader_status.h"
#include <psapi.h>
#include <shellapi.h>
#include "placeholder.h"
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

namespace books {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
constexpr UINT Loaded=WM_APP+71;
constexpr wchar_t Origin[]=L"https://reader.invalid/";
INT_PTR CALLBACK formattedFailureProc(HWND window,UINT message,WPARAM wp,LPARAM) {
    if(message==WM_INITDIALOG){applyTheme(window);return TRUE;}
    if(message==WM_COMMAND) {
        const auto id=LOWORD(wp);
        if(id==IDC_READ_AS_TEXT || id==IDC_OPEN_DEFAULT){EndDialog(window,id);return TRUE;}
        if(id==IDCANCEL){EndDialog(window,0);return TRUE;}
    }
    if(message==WM_CLOSE){EndDialog(window,0);return TRUE;}
    return FALSE;
}
void cleanupReaderProfile(const std::filesystem::path& folder) {
    if(folder.empty())return;
    std::thread([folder]{
        // WebView2 releases its profile asynchronously after Close, including
        // when environment creation finishes after a selection was cancelled.
        for(int attempt=0;attempt<50;++attempt){Sleep(100);std::error_code error;std::filesystem::remove_all(folder,error);if(!error)break;}
    }).detach();
}
std::vector<BYTE> asset(int id) {
    auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource) return {};
    auto data=static_cast<const BYTE*>(LockResource(LoadResource(nullptr,resource)));
    return {data,data+SizeofResource(nullptr,resource)};
}
std::wstring wideText(const std::string& text) {
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(size,0);
    if(size) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size);
    return result;
}
// Decode once, and reject every ambiguous URL component before archive lookup.
std::string archivePath(const std::wstring& url) {
    constexpr wchar_t prefix[]=L"https://book.invalid/";
    if(!url.starts_with(prefix)) return {};
    auto path=url.substr(std::size(prefix)-1); path=path.substr(0,path.find_first_of(L"?#"));
    std::string result;
    auto hex=[](wchar_t c)->int {if(c>=L'0'&&c<=L'9')return c-L'0'; if(c>=L'a'&&c<=L'f')return c-L'a'+10;if(c>=L'A'&&c<=L'F')return c-L'A'+10;return -1;};
    for(size_t i=0;i<path.size();++i) {
        if(path[i]==L'%') { if(i+2>=path.size() || hex(path[i+1])<0 || hex(path[i+2])<0)return {}; result+=static_cast<char>(hex(path[i+1])*16+hex(path[i+2])); i+=2; }
        else { if(path[i]>127)return {}; result+=static_cast<char>(path[i]); }
    }
    if(result.empty() || result.front()=='/' || result.find_first_of(":\\")!=std::string::npos || result.find('\0')!=std::string::npos)return {};
    size_t start=0;
    while(start<result.size()) {auto end=result.find('/',start);auto part=result.substr(start,end-start);if(part==".."||part==".")return {};if(end==std::string::npos)break;start=end+1;}
    return result;
}
std::wstring encodedPath(const std::string& path) {
    std::wstring out;
    constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(unsigned char c:path) {if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='.'||c=='-'||c=='_'||c=='~')out+=c;else{out+=L'%';out+=hex[c>>4];out+=hex[c&15];}}
    return out;
}
const wchar_t* mime(const std::string& name) {
    auto ext=std::filesystem::path(wideText(name)).extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
    if(ext==L".css")return L"text/css";
    if(ext==L".xhtml"||ext==L".html"||ext==L".htm")return L"application/xhtml+xml";
    if(ext==L".opf"||ext==L".xml"||ext==L".ncx")return L"application/xml";
    if(ext==L".svg")return L"image/svg+xml";
    if(ext==L".png")return L"image/png";
    if(ext==L".jpg"||ext==L".jpeg")return L"image/jpeg";
    if(ext==L".gif")return L"image/gif";
    if(ext==L".webp")return L"image/webp";
    if(ext==L".woff")return L"font/woff";
    if(ext==L".woff2")return L"font/woff2";
    if(ext==L".ttf")return L"font/ttf";
    if(ext==L".otf")return L"font/otf";
    return L"application/octet-stream";
}
}
struct Reader::Impl:std::enable_shared_from_this<Reader::Impl> {
    HWND dialog{},cover{},host{},webHost{},buttons[3]{},tooltip{},fullscreenNotice{};
    std::function<void()> toggle;
    std::function<int()> fallbackChoice;
    std::filesystem::path path;
    bool doc=false;
    std::shared_ptr<ReaderDocument> converted;
    ReaderLoading activity;
    ReaderStatus status;
    uint64_t imageRequest=0;
    int webWidth=0,webHeight=0;
    std::filesystem::path readerPath()const{return converted?converted->path:path;}
    void beginLoading(ReaderOperation kind){loading=true;activity.begin(kind,GetTickCount64());if(activity.visible(GetTickCount64()))status.show(activity.text());else SetTimer(host,7,150,nullptr);SetTimer(host,8,100,nullptr);}
    void endLoading(){loading=false;activity.cancel();KillTimer(host,7);KillTimer(host,8);status.hide();}
    void webOperation(const wchar_t* command){beginLoading(ReaderOperation::Page);auto message=L"op:"+std::to_wstring(activity.identity)+L":"+command;if(FAILED(web->PostWebMessageAsString(message.c_str())))formattedFailure();}

    bool active=false,isFull=false,epub=false,pdf=false,available=false,atStart=true,atEnd=true,loading=false,failed=false;
    bool image=false,imageReady=false;
    bool previewHasCover=false;
    bool plainText=false,rtf=false,html=false,docx=false,textLast=true,textBottom=false,rtfBack=false;
    uint64_t textIndex=0;UINT textEncoding=0;
    int rtfAnchor=0,rtfNext=0;
    bool opening=false;
    std::shared_ptr<ReaderTextFile> extracted;
    ReaderProcess webProcess;
    uint64_t webBudget=0;
    std::filesystem::path htmlProfile;
    HWND textView{},textNotice{},textBar{};
    HFONT textFont{};
    unsigned page=0,pages=0;
    uint64_t generation=0,webGeneration=0;
    HBITMAP bitmap=nullptr;
    int renderedWidth=0,renderedHeight=0;
    std::shared_ptr<EpubResources> resources;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    struct Result {
        uint64_t version=0; unsigned page=0; HBITMAP bitmap=nullptr;
        std::shared_ptr<EpubResources> resources;
        std::optional<TextChunk> text;
        RtfResponse rich;
        std::shared_ptr<ReaderTextFile> extracted;
        DocConversion conversion;
        ~Result(){if(bitmap)DeleteObject(bitmap);}
    };
    struct Work {
        std::mutex mutex; std::condition_variable wake;
        bool stop=false,pending=false,release=false,convert=false,epub=false,plainText=false,rtf=false,extract=false,extractRtf=false,back=false;
        int dpi=96;
        uint64_t chunk=0;UINT encoding=0;
        std::shared_ptr<ReaderTextFile> extracted;
        std::shared_ptr<ReaderDocument> converted;
        HWND host=nullptr; uint64_t version=0; unsigned page=0;
        int width=0,height=0; std::filesystem::path path;
        std::shared_ptr<Result> result;
    };
    std::shared_ptr<Work> work=std::make_shared<Work>();
    ~Impl(){if(bitmap)DeleteObject(bitmap);}
    static LRESULT CALLBACK hostProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
        auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,msg,wp,lp);
        if(msg==WM_TIMER&&wp==7){KillTimer(window,7);if(self->activity.visible(GetTickCount64()))self->status.show(self->activity.text());return 0;}
        if(msg==WM_TIMER&&wp==8){if(self->activity.expired(GetTickCount64())){if(self->image)self->endLoading();else if(self->doc&&!self->converted)self->conversionFailure(6);else if(self->html||self->docx||self->rtf)self->formattedFailure();else self->failure();}return 0;}
        if(msg==Loaded){self->complete();return 0;}
        if(msg==ContentScroll){auto scroll=self->textScroll();auto top=std::clamp(static_cast<int>(wp),0,(std::max)(0,scroll.nMax+1-static_cast<int>(scroll.nPage)));SendMessageW(self->textView,EM_LINESCROLL,0,top-scroll.nPos);self->textState();return 0;}
        if(msg==WM_MOUSEWHEEL&&self->plainText&&self->active)return SendMessageW(self->textView,msg,wp,lp);
        if(msg==WM_SETFOCUS&&self->plainText&&self->active){SetFocus(self->textView);return 0;}
        if(msg==WM_CTLCOLORSTATIC&&(reinterpret_cast<HWND>(lp)==self->textView||reinterpret_cast<HWND>(lp)==self->textNotice)){auto dc=reinterpret_cast<HDC>(wp);bool dark=usesDarkTheme(self->dialog);SetTextColor(dc,dark?RGB(240,240,240):RGB(25,25,25));SetBkColor(dc,dark?RGB(32,32,32):GetSysColor(COLOR_WINDOW));return reinterpret_cast<LRESULT>(panelBackground(self->dialog));}
        if(msg==WM_COMMAND&&reinterpret_cast<HWND>(lp)==self->textView){if(HIWORD(wp)==EN_VSCROLL)self->textState();return 0;}
        if(msg==WM_TIMER&&wp==1){KillTimer(window,1);self->hover(-1,-1);return 0;}
        if(msg==WM_TIMER&&wp==2){KillTimer(window,2);if(self->loading){if(self->html||self->docx)self->formattedFailure();else self->failure();}return 0;}
        if(msg==WM_TIMER&&wp==6){self->bootstrapReady();return 0;}
        if(msg==WM_TIMER&&wp==4){self->checkWebMemory();return 0;}
        if(msg==WM_TIMER&&wp==5){if(self->rtf&&self->active&&readerMemoryPressure())self->formattedFailure();return 0;}
        if(msg==WM_MOUSEWHEEL&&self->rtf&&self->active){self->navigate(GET_WHEEL_DELTA_WPARAM(wp)<0?1:-1);return 0;}
        if(msg==WM_TIMER&&wp==3){KillTimer(window,3);ShowWindow(self->fullscreenNotice,SW_HIDE);return 0;}
        if(msg==WM_DRAWITEM){self->drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;}
        if(msg==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);RECT r{};GetClientRect(window,&r);FillRect(dc,&r,panelBackground(self->dialog));if(self->bitmap)drawBookCover(dc,r,self->bitmap,L".pdf");EndPaint(window,&ps);return 0;}
        if(msg==WM_MOUSEMOVE){self->hover(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);return 0;}
        if(msg==WM_MOUSELEAVE){self->hover(-1,-1);return 0;}
        if(msg==WM_LBUTTONDOWN){SetFocus(window);return 0;}
        if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS|DLGC_WANTTAB;
        if(msg==WM_KEYDOWN){if(self->key(static_cast<UINT>(wp)))return 0;}
        if(msg==WM_COMMAND){int id=LOWORD(wp);if(id==IDC_READER_PREVIOUS)self->navigate(-1);if(id==IDC_READER_NEXT)self->navigate(1);if(id==IDC_READER_FULLSCREEN&&self->can(2))self->toggle();return 0;}
        return DefWindowProcW(window,msg,wp,lp);
    }
    static LRESULT CALLBACK coverProc(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Impl*>(data);
        if(msg==WM_MOUSEMOVE){self->hover(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);}
        if(msg==WM_MOUSELEAVE)self->hover(-1,-1);
        if(msg==WM_LBUTTONDOWN)SetFocus(window);
        if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS|DLGC_WANTTAB;
        if(msg==WM_KEYDOWN&&self->key(static_cast<UINT>(wp)))return 0;
        return DefSubclassProc(window,msg,wp,lp);
    }
    static LRESULT CALLBACK buttonProc(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Impl*>(data);
        if(msg==WM_SETFOCUS){ShowWindow(window,SW_SHOW);InvalidateRect(window,nullptr,TRUE);}
        if(msg==WM_KILLFOCUS)self->hover(-1,-1);
        if(msg==WM_KEYDOWN&&wp!=VK_TAB&&self->key(static_cast<UINT>(wp)))return 0;
        if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS|DLGC_WANTMESSAGE;
        if(msg==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_SPACE)){SendMessageW(self->host,WM_COMMAND,GetDlgCtrlID(window),0);return 0;}
        if(msg==WM_KEYDOWN&&wp==VK_TAB){int current=GetDlgCtrlID(window)-IDC_READER_PREVIOUS;int delta=GetKeyState(VK_SHIFT)<0?-1:1;for(int i=current+delta;i>=0&&i<3;i+=delta)if(self->can(i)){ShowWindow(self->buttons[i],SW_SHOW);SetFocus(self->buttons[i]);return 0;}SetFocus(self->isFull?self->host:GetNextDlgTabItem(self->dialog,self->cover,delta<0));return 0;}
        if(msg==WM_MOUSEMOVE){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};MapWindowPoints(window,self->host,&p,1);self->hover(p.x,p.y);TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);}
        if(msg==WM_MOUSELEAVE)self->hover(-1,-1);
        return DefSubclassProc(window,msg,wp,lp);
    }
    static LRESULT CALLBACK textProc(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Impl*>(data);
        if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS|DLGC_WANTTAB;
        if(msg==WM_KEYDOWN&&self->key(static_cast<UINT>(wp)))return 0;
        if(msg==WM_MOUSEMOVE){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};MapWindowPoints(window,self->host,&p,1);self->hover(p.x,p.y);TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);}
        if(msg==WM_MOUSELEAVE)self->hover(-1,-1);
        if((msg==WM_MOUSEWHEEL||msg==WM_VSCROLL||msg==WM_KEYDOWN)&&self->active&&!self->loading){
            auto scroll=self->textScroll();
            bool down=(msg==WM_MOUSEWHEEL&&GET_WHEEL_DELTA_WPARAM(wp)<0)||(msg==WM_VSCROLL&&(LOWORD(wp)==SB_PAGEDOWN||LOWORD(wp)==SB_LINEDOWN))||(msg==WM_KEYDOWN&&(wp==VK_NEXT||wp==VK_DOWN));
            bool up=(msg==WM_MOUSEWHEEL&&GET_WHEEL_DELTA_WPARAM(wp)>0)||(msg==WM_VSCROLL&&(LOWORD(wp)==SB_PAGEUP||LOWORD(wp)==SB_LINEUP))||(msg==WM_KEYDOWN&&(wp==VK_PRIOR||wp==VK_UP));
            if((down&&!self->textLast&&scroll.nPos+static_cast<int>(scroll.nPage)>scroll.nMax)||(up&&self->textIndex&&scroll.nPos==0)){self->navigate(down?1:-1);return 0;}
        }
        auto result=DefSubclassProc(window,msg,wp,lp);
        if(msg==WM_MOUSEWHEEL||msg==WM_VSCROLL||msg==WM_KEYDOWN)self->textState();
        return result;
    }
    SCROLLINFO textScroll(){
        RECT rect{};SendMessageW(textView,EM_GETRECT,0,reinterpret_cast<LPARAM>(&rect));
        auto dc=GetDC(textView);auto font=reinterpret_cast<HFONT>(SendMessageW(textView,WM_GETFONT,0,0));auto old=SelectObject(dc,font);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(textView,dc);
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};scroll.nMax=(std::max)(0,static_cast<int>(SendMessageW(textView,EM_GETLINECOUNT,0,0))-1);
        scroll.nPage=(std::max)(1L,(rect.bottom-rect.top)/(std::max)(1L,metrics.tmHeight));scroll.nPos=static_cast<int>(SendMessageW(textView,EM_GETFIRSTVISIBLELINE,0,0));return scroll;
    }
    void textState(){if(!plainText||!active)return;auto scroll=textScroll();SetScrollInfo(textBar,SB_CTL,&scroll,FALSE);InvalidateRect(textBar,nullptr,FALSE);atStart=textIndex==0&&scroll.nPos==0;atEnd=textLast&&scroll.nPos+static_cast<int>(scroll.nPage)>scroll.nMax;hover(-1,-1);}
    static LRESULT CALLBACK noticeProc(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
        if(msg==WM_PAINT){
            PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT r{};GetClientRect(window,&r);
            auto background=CreateSolidBrush(RGB(38,43,49));FillRect(dc,&r,background);DeleteObject(background);
            auto font=CreateFontW(-MulDiv(20,GetDpiForWindow(window),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            auto previous=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
            DrawTextW(dc,L"To exit full screen, press Esc",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,previous);DeleteObject(font);EndPaint(window,&paint);return 0;
        }
        if(msg==WM_NCHITTEST)return HTTRANSPARENT;
        return DefSubclassProc(window,msg,wp,lp);
    }
    void init() {
        WNDCLASSW type{};type.lpfnWndProc=hostProc;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"VolturaBooksReader";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&type);
        host=CreateWindowExW(0,type.lpszClassName,L"Book reader",WS_CHILD|WS_CLIPCHILDREN|WS_TABSTOP,0,0,0,0,dialog,nullptr,type.hInstance,this);
        webHost=CreateWindowExW(0,L"Static",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,host,nullptr,type.hInstance,nullptr);
        textView=CreateWindowExW(0,L"Edit",L"",WS_CHILD|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,0,0,host,nullptr,type.hInstance,nullptr);
        textBar=CreateWindowExW(0,L"Scrollbar",L"",WS_CHILD|SBS_VERT,0,0,0,0,host,nullptr,type.hInstance,nullptr);attachContentScrollbar(host,textBar,dialog);
        textNotice=CreateWindowExW(0,L"Static",L"Text-only reading",WS_CHILD|SS_CENTER,0,0,0,0,host,nullptr,type.hInstance,nullptr);
        SendMessageW(textView,EM_SETLIMITTEXT,2*ReaderTextWindow+16,0);
        SetWindowSubclass(textView,textProc,1,reinterpret_cast<DWORD_PTR>(this));
        fullscreenNotice=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,L"Static",L"To exit full screen, press Esc",WS_CHILD,0,0,0,0,host,nullptr,type.hInstance,nullptr);
        SetLayeredWindowAttributes(fullscreenNotice,0,245,LWA_ALPHA);
        SetWindowSubclass(fullscreenNotice,noticeProc,1,0);
        tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,0,0,0,0,dialog,nullptr,type.hInstance,nullptr);
        for(int i=0;i<3;++i){buttons[i]=CreateWindowExW(0,L"Button",i==0?L"Previous page":i==1?L"Next page":L"Fullscreen (F11)",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,host,reinterpret_cast<HMENU>(static_cast<INT_PTR>(i+IDC_READER_PREVIOUS)),type.hInstance,nullptr);SetWindowSubclass(buttons[i],buttonProc,1,reinterpret_cast<DWORD_PTR>(this));TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=dialog;tool.uId=reinterpret_cast<UINT_PTR>(buttons[i]);tool.lpszText=const_cast<LPWSTR>(i==0?L"Previous page":i==1?L"Next page":L"Toggle fullscreen (F11, Esc to exit)");SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));}
        SetWindowLongPtrW(cover,GWL_STYLE,GetWindowLongPtrW(cover,GWL_STYLE)|WS_TABSTOP|SS_NOTIFY);
        // Keep the existing preview above WebView2 while it lays out its opening
        // section. A visible controller is required for EPUB.js animation frames.
        SetWindowLongPtrW(cover,GWL_EXSTYLE,GetWindowLongPtrW(cover,GWL_EXSTYLE)|WS_EX_LAYERED);
        SetLayeredWindowAttributes(cover,0,255,LWA_ALPHA);
        // Layered child windows blend over native covers and WebView2 alike.
        for(int i=0;i<3;++i){SetWindowLongPtrW(buttons[i],GWL_EXSTYLE,GetWindowLongPtrW(buttons[i],GWL_EXSTYLE)|WS_EX_LAYERED);SetLayeredWindowAttributes(buttons[i],0,i==2?255:160,LWA_ALPHA);}
        SetWindowTheme(tooltip,L"",L"");
        SetWindowSubclass(cover,coverProc,71,reinterpret_cast<DWORD_PTR>(this));
        status.init(dialog,cover);
        work->host=host;
        auto job=work;
        std::thread([job]{
            auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            RtfReader richReader;
            for(;;){
                std::unique_lock lock(job->mutex);job->wake.wait(lock,[&]{return job->stop||job->pending||job->release;});if(job->stop)break;if(job->release){richReader.close();job->release=false;}if(!job->pending)continue;
                auto path=job->path;auto version=job->version;auto page=job->page;int width=job->width,height=job->height,dpi=job->dpi;bool convert=job->convert;auto converted=job->converted;bool epub=job->epub,plainText=job->plainText,rtf=job->rtf,extract=job->extract,extractRtf=job->extractRtf,back=job->back;auto chunk=job->chunk;auto encoding=job->encoding;auto extracted=job->extracted;job->pending=false;lock.unlock();
                auto cancelled=[&]{std::lock_guard guard(job->mutex);return job->stop||job->version!=version;};
                if(!rtf)richReader.close();
                auto result=std::make_shared<Result>();result->version=version;result->page=page;
                if(convert){result->conversion=convertDoc(path,cancelled);}
                else if(extract){extracted=extractReaderText(path,extractRtf,cancelled);result->extracted=extracted;}
                if(convert){}
                else if(plainText&&(!extract||extracted))result->text=loadReaderTextChunk(extracted?extracted->path:path,chunk,cancelled,encoding);
                else if(rtf)result->bitmap=richReader.render(path,{width,height,dpi,static_cast<int>(page),back?1:0},result->rich,cancelled);
                else if(SUCCEEDED(com)){if(epub)result->resources=loadEpubResources(path);else result->bitmap=loadPdfPage(path,page,width,height);}
                lock.lock();if(!job->stop&&job->version==version){job->result=result;PostMessageW(job->host,Loaded,0,0);}
            }
            if(SUCCEEDED(com))CoUninitialize();
        }).detach();
        resize();
    }
    void closeWeb(){KillTimer(host,2);KillTimer(host,4);KillTimer(host,5);KillTimer(host,6);++webGeneration;if(controller)controller->Close();web.Reset();controller.Reset();environment.Reset();webProcess.close();if(!htmlProfile.empty()){auto folder=std::move(htmlProfile);htmlProfile.clear();cleanupReaderProfile(folder);}}
    void stop(){endLoading();status.close();closeWeb();converted.reset();{std::lock_guard lock(work->mutex);work->stop=true;work->host=nullptr;work->result.reset();work->extracted.reset();work->converted.reset();}work->wake.notify_one();RemoveWindowSubclass(cover,coverProc,71);DestroyWindow(tooltip);DestroyWindow(fullscreenNotice);for(auto button:buttons)DestroyWindow(button);DestroyWindow(host);DeleteObject(textFont);host=nullptr;}
    void request(unsigned target,bool archive=false,bool extract=false,bool view=false) {
        RECT r{};GetClientRect(host,&r);beginLoading(!active?ReaderOperation::Opening:view?ReaderOperation::View:ReaderOperation::Page);
        {std::lock_guard lock(work->mutex);work->version=++generation;work->path=readerPath();work->converted=converted;work->convert=doc&&!converted&&!extract;work->page=target;work->width=r.right;work->height=r.bottom;work->epub=archive;work->plainText=plainText;work->rtf=rtf;work->back=rtfBack;work->dpi=GetDpiForWindow(dialog);work->chunk=textIndex;work->encoding=textEncoding;work->extract=extract;work->extractRtf=fileFormat(path.extension().wstring())==&FileFormats[2];work->extracted=extracted;work->pending=true;work->result.reset();}
        work->wake.notify_one();hover(-1,-1);
    }
    void select(const std::filesystem::path& next) {
        imageReady=false;
        previewHasCover=false;
        SetWindowTextW(textView,L"");extracted.reset();textIndex=0;textEncoding=0;textLast=true;textBottom=false;rtfAnchor=rtfNext=0;rtfBack=false;opening=false;
        endLoading();closeWeb();converted.reset();imageRequest=0;path=next;active=false;available=false;loading=false;failed=false;page=pages=0;atStart=atEnd=true;resources.reset();
        {std::lock_guard lock(work->mutex);work->version=++generation;work->pending=false;work->rtf=false;work->release=true;work->extracted.reset();work->converted.reset();work->result.reset();}work->wake.notify_one();
        if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}
        renderedWidth=renderedHeight=0;
        auto ext=path.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),towlower);epub=ext==L".epub";pdf=ext==L".pdf";
        auto format=fileFormat(ext);image=format&&std::string(format->mime).starts_with("image/");
        plainText=ext==L".txt";rtf=ext==L".rtf";html=ext==L".html"||ext==L".htm";doc=ext==L".doc";docx=ext==L".docx"||doc;
        SetDlgItemTextW(dialog,IDC_FULLSCREEN_READER,image?L"View in full screen":L"Read in full screen");
        available=epub||plainText||rtf||html||docx;show();
    }
    void show(){
        auto fullscreenLabel=isFull?L"Exit full screen (Esc or F11)":L"Enter full screen (F11)";
        SetWindowTextW(buttons[2],fullscreenLabel);
        TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=dialog;
        tool.uId=reinterpret_cast<UINT_PTR>(buttons[2]);tool.lpszText=const_cast<LPWSTR>(fullscreenLabel);
        SendMessageW(tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
        const bool preparing=(epub||html||docx)&&loading&&controller;
        ShowWindow(cover,active?SW_HIDE:SW_SHOW);
        ShowWindow(host,active||preparing?SW_SHOW:SW_HIDE);
        if(!active)SetWindowPos(cover,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        // Navigation on the cover is parented directly to the dialog.
        for(auto button:buttons)SetParent(button,active?host:dialog);
        SetParent(fullscreenNotice,active?host:dialog);
        ShowWindow(webHost,(active&&(epub||html||docx))||preparing?SW_SHOW:SW_HIDE);
        ShowWindow(textView,active&&plainText?SW_SHOW:SW_HIDE);ShowWindow(textBar,active&&plainText?SW_SHOW:SW_HIDE);
        ShowWindow(textNotice,active&&plainText&&extracted?SW_SHOW:SW_HIDE);
        if(controller)controller->put_IsVisible(active||preparing);
        resize();hover(-1,-1);
    }
    bool can(int i)const {return i==0?active&&(epub||plainText||rtf||html||docx||page>0):i==1?available&&!loading&&(!active||!atEnd):active||isFull||(!failed&&(available||pages>0||imageReady));}
    int edgeWidth()const {RECT r{};GetClientRect(host,&r);return (std::max)(1,static_cast<int>(r.right)/5);}
    void hover(int x,int y) {
        EnableWindow(GetDlgItem(dialog,IDC_FULLSCREEN_READER),can(2));
        RECT r{};GetClientRect(host,&r);int edge=edgeWidth(),top=MulDiv(40,GetDpiForWindow(dialog),96);
        bool dark=usesDarkTheme(dialog);
        SendMessageW(tooltip,TTM_SETTIPBKCOLOR,dark?RGB(48,48,48):RGB(244,244,244),0);
        SendMessageW(tooltip,TTM_SETTIPTEXTCOLOR,dark?RGB(245,245,245):RGB(35,35,35),0);
        POINT cursor{};GetCursorPos(&cursor);
        for(int i=0;i<3;++i){RECT button{};GetWindowRect(buttons[i],&button);bool overButton=IsWindowVisible(buttons[i])&&PtInRect(&button,cursor);bool inside=x>=0&&x<r.right&&y>=0&&y<r.bottom;bool fullCorner=can(2)&&x>=r.right-top&&y<top;bool nearEdge=inside&&(i==0?x<edge:i==1?x>=r.right-edge&&!fullCorner:fullCorner);ShowWindow(buttons[i],can(i)&&(nearEdge||(!inside&&(overButton||GetFocus()==buttons[i])))?SW_SHOW:SW_HIDE);}
        if(x>=0&&y>=0)SetTimer(host,1,1800,nullptr);
    }
    void drawButton(const DRAWITEMSTRUCT& draw) {
        auto dc=draw.hDC;auto r=draw.rcItem;bool dark=usesDarkTheme(dialog);
        bool navigation=draw.CtlID!=IDC_READER_FULLSCREEN;
        auto brush=CreateSolidBrush(navigation?RGB(0,120,215):dark?RGB(48,48,48):RGB(244,244,244));FillRect(dc,&r,brush);DeleteObject(brush);
        auto pen=CreatePen(PS_SOLID,MulDiv(navigation?6:2,GetDpiForWindow(dialog),96),navigation||dark?RGB(255,255,255):RGB(35,35,35));auto previous=SelectObject(dc,pen);
        int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2,d=navigation?(std::min)(static_cast<int>(r.right-r.left)/3,MulDiv(32,GetDpiForWindow(dialog),96)):MulDiv(6,GetDpiForWindow(dialog),96);
        if(draw.CtlID==IDC_READER_FULLSCREEN)drawFullscreenIcon(dc,r,GetDpiForWindow(dialog),dark?RGB(255,255,255):RGB(35,35,35));
        else {int direction=draw.CtlID==IDC_READER_PREVIOUS?-1:1;MoveToEx(dc,cx-direction*d/2,cy-d,nullptr);LineTo(dc,cx+direction*d/2,cy);LineTo(dc,cx-direction*d/2,cy+d);}
        SelectObject(dc,previous);DeleteObject(pen);if(draw.itemState&ODS_FOCUS)drawKeyboardFocus(draw.hwndItem,dc,r,navigation);
    }
    void resize() {
        RECT r{};GetWindowRect(cover,&r);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&r),2);
        SetWindowPos(host,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
        RECT client{};GetClientRect(host,&client);SetWindowPos(webHost,nullptr,0,0,client.right,client.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
        if(plainText){
            auto anchor=SendMessageW(textView,EM_LINEINDEX,SendMessageW(textView,EM_GETFIRSTVISIBLELINE,0,0),0);
            int pad=MulDiv(20,GetDpiForWindow(dialog),96),width=(std::max)(1,(std::min)(static_cast<int>(client.right)-pad*2,MulDiv(900,GetDpiForWindow(dialog),96)));
            auto font=CreateFontW(-MulDiv(18,GetDpiForWindow(dialog),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            SendMessageW(textView,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);SendMessageW(textNotice,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);DeleteObject(textFont);textFont=font;
            int notice=extracted?MulDiv(28,GetDpiForWindow(dialog),96):0;
            SetWindowPos(textNotice,nullptr,(client.right-width)/2,pad,width,notice,SWP_NOZORDER|SWP_NOACTIVATE);
            int barWidth=MulDiv(14,GetDpiForWindow(dialog),96);
            SetWindowPos(textBar,nullptr,(client.right+width)/2-barWidth,pad+notice,barWidth,(std::max)(1,static_cast<int>(client.bottom)-pad*2-notice),SWP_NOZORDER|SWP_NOACTIVATE);
            SetWindowPos(textView,nullptr,(client.right-width)/2,pad+notice,(std::max)(1,width-barWidth),(std::max)(1,static_cast<int>(client.bottom)-pad*2-notice),SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(textView,EM_LINESCROLL,0,SendMessageW(textView,EM_LINEFROMCHAR,anchor,0)-SendMessageW(textView,EM_GETFIRSTVISIBLELINE,0,0));textState();
        }
        int edge=edgeWidth(),top=MulDiv(40,GetDpiForWindow(dialog),96),offsetX=active?0:r.left,offsetY=active?0:r.top;
        for(int i=0;i<3;++i)SetWindowPos(buttons[i],HWND_TOP,offsetX+(i==0?0:client.right-(i==2?top:edge)),offsetY,i==2?top:edge,i==2?top:client.bottom,SWP_NOACTIVATE);
        int margin=MulDiv(16,GetDpiForWindow(dialog),96),noticeWidth=(std::max)(1,(std::min)(static_cast<int>(client.right)-2*margin,MulDiv(390,GetDpiForWindow(dialog),96)));
        SetWindowPos(fullscreenNotice,HWND_TOP,offsetX+(client.right-noticeWidth)/2,offsetY+margin,noticeWidth,MulDiv(64,GetDpiForWindow(dialog),96),SWP_NOACTIVATE);
        bool changed=webWidth!=client.right||webHeight!=client.bottom;
        webWidth=client.right;webHeight=client.bottom;
        if(controller){controller->put_Bounds(client);if(web){web->PostWebMessageAsString(usesDarkTheme(dialog)?L"theme:dark":L"theme:light");if(changed&&active){
            beginLoading(ReaderOperation::View);auto message=L"op:"+std::to_wstring(activity.identity)+L":resize";
            if(FAILED(web->PostWebMessageAsString(message.c_str()))){formattedFailure();return;}
        }}}
        status.position();if(activity.visible(GetTickCount64()))status.show(activity.text());else status.hide();
        if(active&&(pdf||rtf)&&(renderedWidth!=client.right||renderedHeight!=client.bottom)){renderedWidth=client.right;renderedHeight=client.bottom;rtfBack=false;request(rtf?rtfAnchor:page,false,false,true);}
    }
    bool key(UINT key) {
        updateFocusCues(dialog,true);
        if(key==VK_TAB){int delta=GetKeyState(VK_SHIFT)<0?-1:1;for(int i=delta<0?2:0;i>=0&&i<3;i+=delta)if(can(i)){ShowWindow(buttons[i],SW_SHOW);SetFocus(buttons[i]);return true;}SetFocus(GetNextDlgTabItem(dialog,cover,delta<0));return true;}
        if(rtf&&(key==VK_NEXT||key==VK_DOWN)){navigate(1);return true;}
        if(rtf&&(key==VK_PRIOR||key==VK_UP)){navigate(-1);return true;}
        if(key==VK_LEFT){navigate(-1);return true;}
        if(key==VK_RIGHT){navigate(1);return true;}
        if(key==VK_F11&&can(2)){toggle();return true;}
        if(key==VK_ESCAPE&&isFull){toggle();return true;}
        return false;
    }
    void navigate(int direction) {
        if(direction<0&&!active)return;
        if(direction>0&&!can(1))return;
        if(loading)return;
        if(plainText){
            if(!active){opening=true;request(0);}
            else if(direction<0&&atStart){active=false;SetWindowTextW(textView,L"");show();SetFocus(cover);}
            else {
                auto scroll=textScroll();
                if(direction>0&&!textLast&&scroll.nPos+static_cast<int>(scroll.nPage)>scroll.nMax){++textIndex;textBottom=false;request(0);}
                else if(direction<0&&textIndex&&scroll.nPos==0){--textIndex;textBottom=true;request(0);}
                else {SendMessageW(textView,EM_SCROLL,direction>0?SB_PAGEDOWN:SB_PAGEUP,0);textState();SetFocus(textView);}
            }
        }else if(rtf){
            if(!active){opening=true;rtfBack=false;request(rtfAnchor);}
            else if(direction<0&&atStart){active=false;KillTimer(host,5);{std::lock_guard lock(work->mutex);work->rtf=false;work->release=true;}work->wake.notify_one();if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}show();SetFocus(cover);}
            else {rtfBack=direction<0;request(direction>0?rtfNext:rtfAnchor);}
        }else if(epub||html||docx){
            if(!active){opening=true;beginLoading(ReaderOperation::Opening);if(doc&&!converted)request(0);else if(epub&&!resources)request(0,true);else startWeb();}
            else if(direction<0&&atStart){active=false;endLoading();closeWeb();converted.reset();resources.reset();show();SetFocus(cover);}
            else if(web){controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);webOperation(direction>0?L"next":L"previous");}
        }else{
            if(direction<0&&page==1){page=0;active=false;show();SetFocus(cover);}
            else request(direction>0?page+1:page-1);
        }
    }
    void complete() {
        std::shared_ptr<Result> result;{std::lock_guard lock(work->mutex);result=std::move(work->result);}
        if(!result||result->version!=generation)return;
        if(doc&&!converted&&!plainText){if(!result->conversion.document){conversionFailure(result->conversion.error);return;}converted=std::move(result->conversion.document);startWeb();return;}
        if(!epub)endLoading();
        if(plainText){if(result->text){if(result->extracted)extracted=std::move(result->extracted);SetWindowTextW(textView,result->text->text.c_str());textIndex=result->text->index;textEncoding=result->text->encoding;textLast=result->text->last;available=true;active=true;opening=false;show();if(textBottom){SendMessageW(textView,EM_LINESCROLL,0,INT_MAX/2);textBottom=false;}textState();SetFocus(textView);}else failure();}
        else if(rtf){if(result->bitmap){if(bitmap)DeleteObject(bitmap);bitmap=result->bitmap;result->bitmap=nullptr;SetTimer(host,5,1000,nullptr);rtfAnchor=result->rich.anchor;rtfNext=result->rich.next;atStart=rtfAnchor==0;atEnd=rtfNext>=result->rich.length;active=true;opening=false;RECT r{};GetClientRect(host,&r);renderedWidth=r.right;renderedHeight=r.bottom;show();SetFocus(host);InvalidateRect(host,nullptr,TRUE);}else formattedFailure();}
        else if(epub){resources=result->resources;available=resources!=nullptr;if(available&&opening)startWeb();else if(!available)failure();}
        else if(result->bitmap){if(bitmap)DeleteObject(bitmap);bitmap=result->bitmap;result->bitmap=nullptr;page=result->page;atEnd=page+1>=pages;active=page>0;RECT r{};GetClientRect(host,&r);renderedWidth=r.right;renderedHeight=r.bottom;show();SetFocus(host);InvalidateRect(host,nullptr,TRUE);}
        else failure();
        hover(-1,-1);
    }
    void cancelWork(){
        {std::lock_guard lock(work->mutex);work->version=++generation;work->pending=false;work->rtf=false;work->release=true;work->result.reset();work->extracted.reset();work->converted.reset();}
        work->wake.notify_one();
    }
    void failure(){endLoading();cancelWork();opening=false;extracted.reset();KillTimer(host,2);failed=true;loading=false;available=false;active=false;closeWeb();converted.reset();SetWindowTextW(textView,L"");if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}if(isFull)toggle();show();SetWindowTextW(cover,L"Reading unavailable. Use Open with default app.");}
    void conversionFailure(DWORD error){
        failure();
        const wchar_t* reason=error==2?L"This older DOC format is not supported. Save it as Word 97–2003 DOC or DOCX in its original application.":error==3?L"This DOC document is encrypted. Open it in its original application and save an unencrypted copy.":error==5?L"The converted document exceeds the 128 MiB reading limit.":error==6?L"DOC conversion exceeded the 30-second time limit.":L"This DOC document could not be converted. It may be damaged, unsupported, or exceed the available reader memory.";
        auto message=std::wstring(reason)+L"\n\nOpen the original document with its default application?";
        if(!fallbackChoice&&MessageBoxW(dialog,message.c_str(),L"DOC reading unavailable",MB_YESNO|MB_ICONINFORMATION)==IDYES)ShellExecuteW(dialog,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    void formattedFailure(){
        endLoading();closeWeb();cancelWork();
        if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}active=false;loading=false;if(isFull)toggle();show();
        int choice=0;
        if(fallbackChoice)choice=fallbackChoice();else choice=static_cast<int>(DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_FORMATTED_FAILURE),dialog,formattedFailureProc,0));
        if(choice==IDC_READ_AS_TEXT){plainText=true;rtf=false;html=false;docx=false;textIndex=0;textEncoding=0;opening=true;SetWindowTextW(textView,L"");SetDlgItemTextW(dialog,IDC_FULLSCREEN_READER,L"Read text in full screen");request(0,false,true);}
        else {failure();if(choice==IDC_OPEN_DEFAULT)ShellExecuteW(dialog,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
    }
    bool containWebProcesses(){
        ComPtr<ICoreWebView2Environment8> env;if(FAILED(environment.As(&env)))return false;
        ComPtr<ICoreWebView2ProcessInfoCollection> processes;if(FAILED(env->GetProcessInfos(&processes)))return false;
        UINT count=0;processes->get_Count(&count);
        for(UINT i=0;i<count;++i){
            ComPtr<ICoreWebView2ProcessInfo> info;if(FAILED(processes->GetValueAtIndex(i,&info)))return false;
            INT32 pid=0;info->get_ProcessId(&pid);
            ReaderHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
            if(!process.value){if(GetLastError()==ERROR_INVALID_PARAMETER)continue;return false;}
            if(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0)continue;
            BOOL member=FALSE;if(!IsProcessInJob(process.value,webProcess.job.value,&member)||!member)return false;
        }return true;
    }
    void bootstrapReady(){
        DWORD readyBytes=0;
        if(!PeekNamedPipe(webProcess.output.value,nullptr,0,nullptr,&readyBytes,nullptr)){formattedFailure();return;}
        if(readyBytes<sizeof(DWORD))return;
        DWORD ready=0,read=0;KillTimer(host,6);
        if(!ReadFile(webProcess.output.value,&ready,sizeof(ready),&read,nullptr)||read!=sizeof(ready)||ready!=1){formattedFailure();return;}
        startWebEnvironment(htmlProfile,webGeneration);
    }
    void checkWebMemory(){if(!(html||docx)||!environment)return;JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};if(!containWebProcesses()||readerMemoryPressure()||(webProcess.job.value&&QueryInformationJobObject(webProcess.job.value,JobObjectExtendedLimitInformation,&info,sizeof(info),nullptr)&&info.PeakJobMemoryUsed>=webBudget*9/10))formattedFailure();}
    void startWeb();
    void startWebEnvironment(const std::filesystem::path& folder,uint64_t token);
    void configureWeb();
};

void Reader::Impl::startWeb() {
    if(controller){active=true;endLoading();show();return;}
    beginLoading(ReaderOperation::Opening);
    SetTimer(host,2,30000,nullptr);
    auto token=++webGeneration;
    PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){failure();return;}
    auto folder=std::filesystem::path(local)/L"Voltura Books"/L"Reader";CoTaskMemFree(local);
    if(html||docx){
        GUID id{};CoCreateGuid(&id);wchar_t name[40]{};StringFromGUID2(id,name,40);folder/=name;htmlProfile=folder;
        webBudget=readerMemoryBudget();
        if(!webProcess.start(folder,L"--html ",webBudget)){formattedFailure();return;}
        SetTimer(host,6,25,nullptr);return;
    }
    startWebEnvironment(folder,token);
}
void Reader::Impl::startWebEnvironment(const std::filesystem::path& folder,uint64_t token) {
    std::weak_ptr<Impl> weak=shared_from_this();
    auto hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,folder.c_str(),nullptr,Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([weak,token,profile=htmlProfile](HRESULT hr,ICoreWebView2Environment* env)->HRESULT{
        auto self=weak.lock();if(!self||self->webGeneration!=token){cleanupReaderProfile(profile);return S_OK;}
        if(FAILED(hr)||!env){if(self->html||self->docx)self->formattedFailure();else self->failure();return S_OK;}self->environment=env;
        if((self->html||self->docx)&&!self->containWebProcesses()){self->formattedFailure();return S_OK;}
        auto created=env->CreateCoreWebView2Controller(self->webHost,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak,token,profile](HRESULT hr,ICoreWebView2Controller* controller)->HRESULT{
            auto self=weak.lock();if(!self||self->webGeneration!=token){if(controller)controller->Close();cleanupReaderProfile(profile);return S_OK;}
            if(FAILED(hr)||!controller){if(self->html||self->docx)self->formattedFailure();else self->failure();return S_OK;}self->controller=controller;controller->get_CoreWebView2(&self->web);
            if(self->html||self->docx){if(!self->containWebProcesses()){self->formattedFailure();return S_OK;}SetTimer(self->host,4,500,nullptr);}
            self->configureWeb();return S_OK;
        }).Get());if(FAILED(created)){if(self->html||self->docx)self->formattedFailure();else self->failure();}return S_OK;
    }).Get());if(FAILED(hr)){if(html||docx)formattedFailure();else failure();}
}
void Reader::Impl::configureWeb() {
    std::weak_ptr<Impl> weak=shared_from_this();auto token=webGeneration;EventRegistrationToken event{};
    ComPtr<ICoreWebView2Settings> settings;web->get_Settings(&settings);
    settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_AreDevToolsEnabled(FALSE);settings->put_AreHostObjectsAllowed(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;if(SUCCEEDED(settings.As(&settings3)))settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* a)->HRESULT{return a->put_Handled(TRUE);}).Get(),&event);
    web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* a)->HRESULT{return a->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);}).Get(),&event);
    ComPtr<ICoreWebView2_4> web4;if(SUCCEEDED(web.As(&web4)))web4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs* a)->HRESULT{return a->put_Cancel(TRUE);}).Get(),&event);
    web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* a)->HRESULT{PWSTR uri=nullptr;a->get_Uri(&uri);bool allowed=uri&&std::wstring(uri)==L"https://reader.invalid/index.html";CoTaskMemFree(uri);return a->put_Cancel(!allowed);}).Get(),&event);
    web->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([weak,token](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* a)->HRESULT{PWSTR uri=nullptr;a->get_Uri(&uri);std::wstring value=uri?uri:L"";CoTaskMemFree(uri);auto self=weak.lock();bool document=self&&self->webGeneration==token&&self->html&&value==L"https://reader.invalid/document.html";return a->put_Cancel(!document&&value!=L"about:blank"&&value!=L"about:srcdoc");}).Get(),&event);
    web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([weak,token](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT{if(auto s=weak.lock();s&&s->webGeneration==token){if(s->html||s->docx)s->formattedFailure();else s->failure();}return S_OK;}).Get(),&event);
    web->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    web->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([weak,token](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT{
        auto self=weak.lock();if(!self||self->webGeneration!=token)return E_ABORT;
        ComPtr<ICoreWebView2WebResourceRequest> request;args->get_Request(&request);PWSTR raw=nullptr;request->get_Uri(&raw);std::wstring uri=raw?raw:L"";CoTaskMemFree(raw);
        request->get_Method(&raw);bool get=raw&&std::wstring(raw)==L"GET";CoTaskMemFree(raw);
        std::vector<BYTE> bytes;const wchar_t* type=L"text/plain";bool found=false;ComPtr<IStream> fileStream;bool document=self->html&&uri==L"https://reader.invalid/document.html";
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT context;args->get_ResourceContext(&context);
        if(get&&document&&context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT){found=SUCCEEDED(SHCreateStreamOnFileEx(self->path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&fileStream));type=L"text/html";}
        else if(get&&self->docx&&uri==L"https://reader.invalid/document.docx"&&(context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH||context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST)){if(self->converted){auto stream=Microsoft::WRL::Make<ReaderDocumentStream>();found=SUCCEEDED(stream->open(self->converted));if(found)fileStream=stream;}else found=SUCCEEDED(SHCreateStreamOnFileEx(self->path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&fileStream));type=L"application/vnd.openxmlformats-officedocument.wordprocessingml.document";}
        else if(get&&self->docx&&uri==L"https://reader.invalid/docx.js"){bytes=asset(204);type=L"text/javascript";found=true;}
        else if(get&&uri==L"https://reader.invalid/index.html"){bytes=asset(201);type=L"text/html";found=true;}
        else if(get&&(uri==L"https://reader.invalid/epub.js"||uri==L"https://reader.invalid/jszip.js")){bytes=asset(uri.ends_with(L"/epub.js")?202:203);type=L"text/javascript";found=true;}
        else if(get&&self->resources&&context!=COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT&&context!=COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT){auto path=archivePath(uri);auto item=self->resources->files.find(path);if(item!=self->resources->files.end()){bytes=item->second;type=mime(path);found=true;}}
        ComPtr<IStream> stream=fileStream;if(!stream)stream.Attach(SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size())));
        std::wstring headers=L"Content-Type: "+std::wstring(type)+L"\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: https://reader.invalid\r\nX-Content-Type-Options: nosniff\r\n";
        headers+=document?L"Content-Security-Policy: sandbox allow-same-origin; default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; img-src data:; font-src data:; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri 'none'\r\n":uri.starts_with(Origin)?L"Content-Security-Policy: default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline' https://book.invalid; connect-src 'self' https://book.invalid; frame-src 'self' about:; img-src data: https://book.invalid; font-src https://book.invalid data:; object-src 'none'\r\n":L"Content-Security-Policy: default-src 'none'; script-src 'none'; style-src 'unsafe-inline' https://book.invalid; img-src https://book.invalid data:; font-src https://book.invalid data:; frame-src 'none'; object-src 'none'\r\n";
        ComPtr<ICoreWebView2WebResourceResponse> response;self->environment->CreateWebResourceResponse(stream.Get(),found?200:403,found?L"OK":L"Blocked",headers.c_str(),&response);return args->put_Response(response.Get());
    }).Get(),&event);
    web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak,token](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
        auto self=weak.lock();if(!self||self->webGeneration!=token)return S_OK;
        PWSTR raw=nullptr;args->get_Source(&raw);bool trusted=raw&&std::wstring(raw)==L"https://reader.invalid/index.html";CoTaskMemFree(raw);if(!trusted)return S_OK;
        if(FAILED(args->TryGetWebMessageAsString(&raw)))return S_OK;std::wstring message=raw;CoTaskMemFree(raw);
        if(message==L"host-ready")self->web->PostWebMessageAsString(usesDarkTheme(self->dialog)?L"theme:dark":L"theme:light");
        if(message==L"host-ready"&&self->docx)self->web->PostWebMessageAsString(L"open-docx");
        else if(message==L"host-ready"&&self->html)self->web->PostWebMessageAsString(L"open-html");
        else if(message==L"host-ready"&&self->resources){auto open=L"open:https://book.invalid/"+encodedPath(self->resources->package);if(self->previewHasCover&&!self->resources->coverImage.empty())open+=L"\nhttps://book.invalid/"+encodedPath(self->resources->coverImage);self->web->PostWebMessageAsString(open.c_str());}
        else if(message==L"ready"){KillTimer(self->host,2);self->endLoading();self->opening=false;self->active=true;self->show();self->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);}
        else if(message.starts_with(L"done:")||message.starts_with(L"failed:")){
            auto split=message.find(L':');wchar_t* end=nullptr;auto id=wcstoull(message.c_str()+split+1,&end,10);
            if(end&&!*end&&self->loading&&id==self->activity.identity){if(message.starts_with(L"failed:")){if(self->html||self->docx)self->formattedFailure();else self->failure();}else{self->endLoading();self->hover(-1,-1);}}
        }
        else if(message==L"error"){if(self->html||self->docx)self->formattedFailure();else self->failure();}
        else if(message.size()==8&&message.starts_with(L"state:")){self->atStart=message[6]==L'1';self->atEnd=message[7]==L'1';self->hover(-1,-1);}
        else if(message.starts_with(L"pointer:")){int x=0,y=0;if(swscanf_s(message.c_str()+8,L"%d,%d",&x,&y)==2)self->hover(MulDiv(x,GetDpiForWindow(self->dialog),96),MulDiv(y,GetDpiForWindow(self->dialog),96));}
        else if(message==L"key:ArrowLeft")self->navigate(-1);
        else if(message==L"key:ArrowRight")self->navigate(1);
        else if(message==L"key:F11")self->key(VK_F11);
        else if(message==L"key:Escape")self->key(VK_ESCAPE);
        else if(message.starts_with(L"key:Tab")){updateFocusCues(self->dialog,true);for(int i=message.ends_with(L":shift")?2:0;i>=0&&i<3;i+=message.ends_with(L":shift")?-1:1)if(self->can(i)){ShowWindow(self->buttons[i],SW_SHOW);SetFocus(self->buttons[i]);break;}}
        return S_OK;
    }).Get(),&event);
    RECT r{};GetClientRect(host,&r);controller->put_Bounds(r);
    // Layout behind the native preview, revealing only the final opening page.
    show();
    if(FAILED(web->Navigate(L"https://reader.invalid/index.html")))failure();
}
Reader::Reader(HWND dialog,HWND cover,std::function<void()> toggle):impl(std::make_shared<Impl>()){impl->dialog=dialog;impl->cover=cover;impl->toggle=std::move(toggle);impl->init();}
Reader::~Reader(){impl->stop();}
void Reader::select(const std::filesystem::path& path){impl->select(path);}
void Reader::previewReady(unsigned pages,bool hasCover){impl->previewHasCover=hasCover;if(impl->pdf&&!impl->failed){impl->pages=hasCover?pages:0;impl->available=hasCover&&pages>1;}if(impl->image)impl->imageReady=hasCover;impl->hover(-1,-1);}
void Reader::resize(){impl->resize();}
void Reader::imageLoading(uint64_t request){if(impl->image){impl->imageRequest=request;impl->beginLoading(ReaderOperation::View);}}
void Reader::imageLoaded(uint64_t request){if(impl->image&&impl->imageRequest==request)impl->endLoading();}
void Reader::fullscreen(bool enabled){bool entering=enabled&&!impl->isFull;impl->isFull=enabled;impl->show();if(entering){ShowWindow(impl->fullscreenNotice,SW_SHOWNOACTIVATE);SetTimer(impl->host,3,3500,nullptr);}else if(!enabled){KillTimer(impl->host,3);ShowWindow(impl->fullscreenNotice,SW_HIDE);}}
bool Reader::reading()const{return impl->active;}
bool Reader::canFullscreen()const{return impl->can(2);}
HWND Reader::window()const{return impl->host;}
void Reader::navigate(int direction){impl->navigate(direction);}
void Reader::openFullscreen(){if(impl->can(2)&&!impl->isFull)impl->toggle();}
}
