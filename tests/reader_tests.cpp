#include "interactive_test.h"
#include <windows.h>
static BOOL fixtureCursor(LPPOINT point){*point={-32768,-32768};return TRUE;}
#define GetCursorPos fixtureCursor
#include "../src/reader.cpp"
#undef GetCursorPos
#include "../src/browser.cpp"
#include <iostream>
namespace books { struct ReaderTestAccess { static auto get(Reader& r){return r.impl;} }; }
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<"\n";++failures;}}while(0)
static bool waitFor(const std::function<bool()>& done,int timeout=15000) {
    auto start=GetTickCount64();
    for(;;) {
        if(done())return true;
        if(GetTickCount64()-start>=static_cast<ULONGLONG>(timeout))return false;
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
    }
}
static std::wstring script(ICoreWebView2* web,const wchar_t* source) {
    CHECK(web!=nullptr);if(!web)return {};
    struct Result{bool done=false;std::wstring value;};auto result=std::make_shared<Result>();
    auto hr=web->ExecuteScript(source,Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>([result](HRESULT hr,LPCWSTR value)->HRESULT{if(SUCCEEDED(hr)&&value)result->value=value;result->done=true;return S_OK;}).Get());
    CHECK(SUCCEEDED(hr));if(FAILED(hr))return {};
    CHECK(waitFor([&]{return result->done;}));return result->value;
}
static void capture(ICoreWebView2* web,const wchar_t* name) {
    CHECK(web!=nullptr);if(!web)return;
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    auto file=std::filesystem::path(executable).parent_path()/name;
    Microsoft::WRL::ComPtr<IStream> stream;CHECK(SUCCEEDED(SHCreateStreamOnFileEx(file.c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_EXCLUSIVE,FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,&stream)));
    auto done=std::make_shared<bool>(false);
    CHECK(SUCCEEDED(web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,stream.Get(),Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>([done](HRESULT hr)->HRESULT{CHECK(SUCCEEDED(hr));*done=true;return S_OK;}).Get())));
    CHECK(waitFor([&]{return *done;}));
}
static void captureBitmap(HBITMAP bitmap,const wchar_t* name){
    BITMAP value{};GetObjectW(bitmap,sizeof(value),&value);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),value.bmWidth,-value.bmHeight,1,32,BI_RGB};
    std::vector<BYTE> pixels(static_cast<size_t>(value.bmWidth)*value.bmHeight*4);auto dc=CreateCompatibleDC(nullptr);GetDIBits(dc,bitmap,0,value.bmHeight,pixels.data(),&info,DIB_RGB_COLORS);DeleteDC(dc);
    BITMAPFILEHEADER header{0x4d42,static_cast<DWORD>(sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+pixels.size()),0,0,sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)};
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);std::ofstream file(std::filesystem::path(exe).parent_path()/name,std::ios::binary);file.write(reinterpret_cast<char*>(&header),sizeof(header));file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
}
int wmain(int argc,wchar_t** argv) {
    if(!interactiveTestsEnabled())return 77;
    if(argc!=2&&argc!=3)return 2;
    const bool docxOnly=argc==3&&std::wstring(argv[2])==L"--docx-only";
    const bool docOnly=argc==3&&std::wstring(argv[2])==L"--doc-only";
    if(argc==3&&!docxOnly&&!docOnly)return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    books::Browser state;state.previewOnly=true;
    auto window=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_BROWSER),nullptr,books::proc,reinterpret_cast<LPARAM>(&state));
    CHECK(window!=nullptr);ShowWindow(window,SW_SHOWNOACTIVATE);
    auto reader=books::ReaderTestAccess::get(*state.reader);std::filesystem::path root=argv[1];
    reader->fallbackChoice=[] {return 0;};
    reader->status.show(L"Opening document…");
    CHECK(IsWindowVisible(reader->status.handle()));
    const auto initialFrame=reader->status.animationFrame();
    CHECK(waitFor([&]{return reader->status.animationFrame()!=initialFrame;}));
    reader->status.hide();
    if(!docxOnly){
        reader->select(root/L"read.doc");CHECK(reader->doc&&reader->can(1)&&!reader->converted);
        reader->navigate(1);CHECK(reader->loading&&reader->activity.visible(GetTickCount64()));
        auto identity=reader->activity.identity;reader->navigate(1);CHECK(reader->activity.identity==identity);
        CHECK(waitFor([&]{return !reader->loading;},35000));CHECK(reader->active&&reader->converted);
        if(reader->converted){
            auto temporary=reader->converted->path;CHECK(reader->path==root/L"read.doc");
            reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));
            reader->fallbackChoice=[]{return IDC_READ_AS_TEXT;};reader->formattedFailure();
            CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->plainText&&reader->active);
            reader->select(root/L"read.txt");CHECK(waitFor([&]{return GetFileAttributesW(temporary.c_str())==INVALID_FILE_ATTRIBUTES;}));
        }
        reader->fallbackChoice=[]{return 0;};
        for(auto name:{L"bad.doc",L"encrypted.doc",L"older.doc"}){reader->select(root/name);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->failed&&!reader->converted);}
        reader->select(root/L"read.doc");reader->navigate(1);reader->select(root/L"read.txt");CHECK(!reader->loading&&!reader->converted);
        if(docOnly){DestroyWindow(window);reader.reset();CoUninitialize();return failures?1:0;}
    }
    if(!docxOnly){
    CHECK(books::archivePath(L"https://book.invalid/OPS/a%20b.xhtml")=="OPS/a b.xhtml");
    CHECK(books::archivePath(L"https://book.invalid/%2e%2e/secret").empty());
    CHECK(books::archivePath(L"https://evil.invalid/OPS/a.xhtml").empty());
    CHECK(!books::loadEpubResources(root/L"traversal.epub"));
    CHECK(!books::loadEpubResources(root/L"encrypted.epub"));
    reader->select(root/L"three.pdf");state.reader->previewReady(3,true);
    CHECK(state.reader->canFullscreen());
    // The production theme must not swallow the owner's arrow painting.
    SendMessageW(window,WM_THEMECHANGED,0,0);
    RECT viewport{};GetClientRect(reader->host,&viewport);
    SetFocus(reader->cover);
    reader->hover(viewport.right-2,viewport.bottom/2);
    CHECK(IsWindowVisible(reader->buttons[1])&&!IsWindowVisible(reader->buttons[0]));
    RECT overlay{};GetClientRect(reader->buttons[1],&overlay);
    CHECK(overlay.bottom==viewport.bottom&&overlay.right==reader->edgeWidth());
    auto screen=GetDC(reader->buttons[1]);auto paint=CreateCompatibleDC(screen);
    auto pixels=CreateCompatibleBitmap(screen,overlay.right,overlay.bottom);auto oldPixels=SelectObject(paint,pixels);
    SendMessageW(reader->buttons[1],WM_PRINTCLIENT,reinterpret_cast<WPARAM>(paint),PRF_CLIENT);
    CHECK(GetPixel(paint,2,2)==RGB(0,120,215));
    bool arrow=false;for(int x=overlay.right/4;x<overlay.right*3/4;++x)if(GetPixel(paint,x,overlay.bottom/2)==RGB(255,255,255))arrow=true;
    CHECK(arrow);SelectObject(paint,oldPixels);DeleteObject(pixels);DeleteDC(paint);ReleaseDC(reader->buttons[1],screen);
    reader->hover(2,viewport.bottom/2);CHECK(!IsWindowVisible(reader->buttons[0])&&!IsWindowVisible(reader->buttons[1]));
    reader->hover(viewport.right/2,viewport.bottom/2);CHECK(!IsWindowVisible(reader->buttons[1]));
    CHECK(SendMessageW(reader->tooltip,TTM_GETTIPBKCOLOR,0,0)==(books::usesDarkTheme(window)?RGB(48,48,48):RGB(244,244,244)));
    SendMessageW(reader->cover,WM_KEYDOWN,VK_TAB,0);if(GetFocus()!=reader->buttons[1])std::cerr<<"Focus diagnostic: id="<<GetDlgCtrlID(GetFocus())<<" shift="<<GetKeyState(VK_SHIFT)<<" next="<<reader->can(1)<<" visible="<<IsWindowVisible(reader->buttons[1])<<" enabled="<<IsWindowEnabled(reader->buttons[1])<<"\n";CHECK(GetFocus()==reader->buttons[1]);SetFocus(reader->cover);
    RECT preview{};GetClientRect(reader->host,&preview);
    CHECK(!reader->active&&reader->can(1));reader->hover(preview.right-1,100);CHECK(IsWindowVisible(reader->buttons[1]));reader->hover(-1,-1);CHECK(!IsWindowVisible(reader->buttons[1]));
    SendMessageW(reader->buttons[1],BM_CLICK,0,0);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->page==1);
    RECT before{};GetWindowRect(window,&before);auto style=GetWindowLongPtrW(window,GWL_STYLE);
    SendMessageW(reader->buttons[2],BM_CLICK,0,0);CHECK(state.fullscreen);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->page==1);
    CHECK(IsWindowVisible(reader->fullscreenNotice));CHECK(GetFocus()!=reader->fullscreenNotice);
    RECT notice{},readerBounds{};GetWindowRect(reader->fullscreenNotice,&notice);GetWindowRect(reader->host,&readerBounds);
    CHECK(abs((notice.left+notice.right)-(readerBounds.left+readerBounds.right))<=1);
    CHECK(waitFor([&]{return !IsWindowVisible(reader->fullscreenNotice);},5000));
    RECT full{};GetWindowRect(window,&full);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);CHECK(EqualRect(&full,&monitor.rcMonitor));
    reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->page==2&&!reader->can(1));
    SendMessageW(window,WM_COMMAND,IDCANCEL,0);CHECK(!state.fullscreen&&IsWindow(window));CHECK(waitFor([&]{return !reader->loading;}));
    RECT after{};GetWindowRect(window,&after);CHECK(EqualRect(&before,&after));CHECK(style==GetWindowLongPtrW(window,GWL_STYLE));CHECK(reader->page==2);
    reader->navigate(-1);CHECK(waitFor([&]{return !reader->loading;}));reader->navigate(-1);CHECK(!reader->active&&reader->page==0);
    reader->select(root/L"single.pdf");state.reader->previewReady(1,true);CHECK(!reader->can(1));
    for(auto id:{IDC_FILE_LIST,IDC_FILE_GRID,IDC_SEARCH,IDC_SORT,IDC_FULLSCREEN_READER}) {
        auto control=GetDlgItem(window,id);SetFocus(control);
        PostMessageW(control,WM_KEYDOWN,VK_F11,0);CHECK(waitFor([&]{return state.fullscreen;}));
        PostMessageW(reader->cover,WM_KEYDOWN,VK_F11,0);CHECK(waitFor([&]{return !state.fullscreen;}));
    }
    CHECK(IsWindowEnabled(GetDlgItem(window,IDC_FULLSCREEN_READER)));
    reader->key(VK_F11);CHECK(state.fullscreen&&!reader->active&&reader->page==0);CHECK(GetFocus()==reader->cover);
    CHECK(IsWindowVisible(reader->fullscreenNotice));reader->key(VK_ESCAPE);CHECK(!state.fullscreen);
    SendMessageW(reader->buttons[2],BM_CLICK,0,0);CHECK(state.fullscreen&&!reader->active);
    reader->key(VK_F11);CHECK(!state.fullscreen);
    reader->select(root/L"three.pdf");state.reader->previewReady(3,true);
    CHECK(IsWindowEnabled(GetDlgItem(window,IDC_FULLSCREEN_READER)));
    SendMessageW(window,WM_COMMAND,IDC_FULLSCREEN_READER,0);
    CHECK(waitFor([&]{return state.fullscreen&&!reader->loading;}));CHECK(reader->page==0&&!reader->active);
    CHECK(IsWindowVisible(reader->fullscreenNotice));
    reader->key(VK_ESCAPE);CHECK(!state.fullscreen);
    CHECK(!IsWindowVisible(reader->fullscreenNotice));
    CHECK(waitFor([&]{return !reader->loading;}));
    CHECK(GetWindowLongPtrW(reader->buttons[2],GWL_EXSTYLE)&WS_EX_LAYERED);
    reader->select(root/L"book.epub");CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->available);
    reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;},30000));CHECK(reader->active&&!reader->failed);
    if(reader->web&&!reader->active)std::wcout<<L"Web diagnostics: "<<script(reader->web.Get(),L"JSON.stringify({html:document.getElementById('book').outerHTML,opened:book.isOpen,spine:book.spine.items.map(i=>i.url),body:document.querySelector('iframe')?.contentDocument?.documentElement?.outerHTML})")<<std::endl;
    if(reader->web&&reader->active){
        auto content=script(reader->web.Get(),L"document.querySelector('iframe').contentDocument.body.textContent");std::wcout<<L"EPUB content: "<<content.substr(0,100)<<L"\n";CHECK(content.find(L"First chapter")!=std::wstring::npos);
        CHECK(script(reader->web.Get(),L"document.querySelector('iframe').contentWindow.hacked===undefined")==L"true");
        CHECK(script(reader->web.Get(),L"document.querySelector('iframe').sandbox.contains('allow-scripts')")==L"false");
        CHECK(script(reader->web.Get(),L"getComputedStyle(document.querySelector('iframe').contentDocument.querySelector('h1')).color")==L"\"rgb(12, 34, 56)\"");
        CHECK(script(reader->web.Get(),L"document.querySelector('iframe').contentDocument.querySelector('img').naturalWidth")==L"20");
        capture(reader->web.Get(),L"reader-inline.png");
        script(reader->web.Get(),L"window.networkProbe='pending';fetch('https://example.com/reader-test').then(()=>window.networkProbe='allowed').catch(()=>window.networkProbe='blocked')");
        CHECK(waitFor([&]{return script(reader->web.Get(),L"window.networkProbe")==L"\"blocked\"";}));
        auto cfi=script(reader->web.Get(),L"locationCfi");books::toggleFullscreen(window,state);
        CHECK(waitFor([&]{return !reader->loading;}));
        CHECK(script(reader->web.Get(),L"innerWidth")!=L"0");
        books::toggleFullscreen(window,state);CHECK(reader->active);
        CHECK(waitFor([&]{return !reader->loading;}));
        CHECK(script(reader->web.Get(),L"locationCfi")==cfi);
        reader->navigate(-1);CHECK(!reader->active);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active);
        // Position/state notifications precede the bridge's loading completion.
        // Wait for completion before issuing another user action.
        for(int i=0;i<3;++i){auto previous=script(reader->web.Get(),L"locationCfi");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(script(reader->web.Get(),L"locationCfi")!=previous);}
        script(reader->web.Get(),L"(()=>{const f=document.querySelector('iframe'),r=f.getBoundingClientRect();f.contentDocument.dispatchEvent(new MouseEvent('mousemove',{clientX:(innerWidth-2-r.left)*f.clientWidth/r.width,clientY:(100-r.top)*f.clientHeight/r.height}));})()");
        CHECK(waitFor([&]{return IsWindowVisible(reader->buttons[1])!=FALSE;}));
        auto anchor=script(reader->web.Get(),L"anchorCfi");books::toggleFullscreen(window,state);
        CHECK(waitFor([&]{return !reader->loading;}));
        CHECK(waitFor([&]{return script(reader->web.Get(),L"Math.abs(rendition.manager._stageSize.width - innerWidth) < 1")==L"true";}));
        capture(reader->web.Get(),L"reader-fullscreen.png");
        books::toggleFullscreen(window,state);CHECK(waitFor([&]{return !reader->loading;}));CHECK(script(reader->web.Get(),L"Math.abs(rendition.manager._stageSize.width - innerWidth) < 1")==L"true");CHECK(script(reader->web.Get(),L"anchorCfi")==anchor);
        for(int i=0;i<150&&!reader->atEnd;++i){auto previous=script(reader->web.Get(),L"locationCfi");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->atEnd||script(reader->web.Get(),L"locationCfi")!=previous);}
        CHECK(reader->atEnd&&!reader->can(1));CHECK(script(reader->web.Get(),L"document.querySelector('iframe').contentDocument.body.textContent").find(L"Second chapter")!=std::wstring::npos);
    }
    reader->select(root/L"fixed.epub");CHECK(waitFor([&]{return !reader->loading;}));SendMessageW(window,WM_COMMAND,IDC_FULLSCREEN_READER,0);CHECK(state.fullscreen&&!reader->active);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;},30000));CHECK(reader->active&&state.fullscreen);
    if(reader->active){CHECK(script(reader->web.Get(),L"rendition.layout().name")==L"\"pre-paginated\"");reader->key(VK_ESCAPE);CHECK(!state.fullscreen&&reader->active);}
    reader->select(root/L"book.epub");CHECK(waitFor([&]{return !reader->loading;}));
    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",L"C:\\VolturaBooksMissingRuntimeForTest");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->failed&&!reader->active);SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",nullptr);
    reader->select(root/L"book.epub");reader->select(root/L"single.pdf");state.reader->previewReady(1,true);CHECK(waitFor([&]{return !reader->loading;}));CHECK(!reader->active&&!reader->available);
    for(auto extension:{L"png",L"jpg",L"jpeg",L"gif",L"bmp"}) {
        auto path=root/(std::wstring(L"picture.")+extension);reader->select(path);
        CHECK(!state.reader->canFullscreen());
        auto bitmap=books::loadCover(path,400,600);CHECK(bitmap!=nullptr);
        auto fullscreenBitmap=books::loadCover(path,3840,2160);CHECK(fullscreenBitmap!=nullptr);if(fullscreenBitmap)DeleteObject(fullscreenBitmap);
        state.reader->previewReady(0,bitmap!=nullptr);if(bitmap)DeleteObject(bitmap);
        CHECK(state.reader->canFullscreen()&&!reader->can(0)&&!reader->can(1));
        SendMessageW(window,WM_COMMAND,IDC_FULLSCREEN_READER,0);CHECK(state.fullscreen&&!reader->active);
        CHECK(IsWindowVisible(reader->cover)&&IsWindowVisible(reader->fullscreenNotice));
        reader->key(VK_ESCAPE);CHECK(!state.fullscreen);
        reader->key(VK_F11);CHECK(state.fullscreen);reader->key(VK_F11);CHECK(!state.fullscreen);
    }
    reader->select(root/L"bad.png");state.reader->previewReady(0,false);CHECK(!state.reader->canFullscreen());
    auto imagePath=root/L"picture.png";
    auto largeImage=books::loadCover(imagePath,3840,2160);CHECK(largeImage!=nullptr);
    if(largeImage){BITMAP info{};GetObjectW(largeImage,sizeof(info),&info);CHECK(info.bmHeight==2160);DeleteObject(largeImage);}
    CHECK(!books::loadCover(imagePath,16385,2160));CHECK(!books::loadCover(imagePath,16384,16384));
    // Exercise the real Browse Books refresh worker, not only the reader controls.
    state.current=imagePath;reader->select(imagePath);
    auto initialPreview=std::make_shared<books::Preview>();initialPreview->cover=books::loadCover(imagePath,400,600);
    {std::lock_guard lock(state.worker->mutex);state.worker->result=initialPreview;}
    state.reader->previewReady(0,true);reader->key(VK_F11);CHECK(state.fullscreen);
    CHECK(waitFor([&]{std::lock_guard lock(state.worker->mutex);return state.worker->result!=initialPreview&&state.worker->result&&state.worker->result->cover;}));
    reader->key(VK_ESCAPE);CHECK(!state.fullscreen);state.current.clear();
    reader->select(root/L"notes.txt");state.reader->previewReady(0,true);CHECK(!reader->loading);
    for(auto name:{L"same-cover",L"same-svg-cover",L"different-first",L"cover-with-text"}) {
        reader->select(root/(std::wstring(name)+L".epub"));state.reader->previewReady(0,true);
        CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->available);
        reader->navigate(1);CHECK(waitFor([&]{if(reader->loading)CHECK(IsWindowVisible(reader->cover));return !reader->loading;}));CHECK(reader->active);
        CHECK(!IsWindowVisible(reader->cover));
        CHECK(GetWindowLongPtrW(reader->cover,GWL_EXSTYLE)&WS_EX_LAYERED);
        if(reader->web&&reader->active){
            auto expected=std::wstring(name).starts_with(L"same-")?L"1":L"0";
            CHECK(waitFor([&]{return script(reader->web.Get(),L"rendition.currentLocation().start.index")==expected;}));
            CHECK(script(reader->web.Get(),L"getComputedStyle(document.getElementById('book')).opacity")==L"\"1\"");
            CHECK(reader->atStart);reader->navigate(-1);CHECK(!reader->active);
        }
    }
    reader->select(root/L"same-cover.epub");state.reader->previewReady(0,false);CHECK(waitFor([&]{return !reader->loading;}));reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));
    if(reader->web&&reader->active)CHECK(script(reader->web.Get(),L"rendition.currentLocation().start.index")==L"0");
    CHECK(books::loadReaderText(root/L"utf16.txt")==L"Unicode: caf\u00e9 \u65e5\u672c\u8a9e\r\nSecond line");
    CHECK(books::loadReaderText(root/L"legacy.txt")==L"caf\u00e9\r\nSecond line");
    CHECK(!books::loadReaderText(root/L"binary.txt"));CHECK(!books::loadReaderText(root/L"empty.txt"));CHECK(books::loadReaderText(root/L"oversized.txt")->size()<=books::ReaderTextWindow+4);
    reader->select(root/L"read.txt");CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->available&&!reader->active);
    reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&IsWindowVisible(reader->textView));CHECK(reader->atStart&&!reader->atEnd);
    CHECK(GetWindowLongPtrW(reader->textView,GWL_STYLE)&ES_READONLY);
    CHECK(!(GetWindowLongPtrW(reader->textView,GWL_STYLE)&WS_VSCROLL));CHECK(IsWindowVisible(reader->textBar));
    {RECT rect{};GetClientRect(reader->textBar,&rect);auto dc=GetDC(reader->textBar);auto memory=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,rect.right,rect.bottom);auto old=SelectObject(memory,bitmap);SendMessageW(reader->textBar,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT);CHECK(GetPixel(memory,0,rect.bottom/2)==(books::usesDarkTheme(window)?RGB(32,32,32):GetSysColor(COLOR_WINDOW)));SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(reader->textBar,dc);}
    SendMessageW(reader->host,books::ContentScroll,3,0);CHECK(SendMessageW(reader->textView,EM_GETFIRSTVISIBLELINE,0,0)==3);SendMessageW(reader->host,books::ContentScroll,0,0);
    reader->navigate(1);CHECK(!reader->atStart);
    auto textAnchor=SendMessageW(reader->textView,EM_LINEINDEX,SendMessageW(reader->textView,EM_GETFIRSTVISIBLELINE,0,0),0);
    reader->key(VK_F11);CHECK(state.fullscreen&&reader->active);reader->key(VK_ESCAPE);CHECK(!state.fullscreen);
    CHECK(SendMessageW(reader->textView,EM_LINEINDEX,SendMessageW(reader->textView,EM_GETFIRSTVISIBLELINE,0,0),0)==textAnchor);
    for(int i=0;i<200&&!reader->atEnd;++i)reader->navigate(1);
    CHECK(reader->atEnd&&!reader->can(1));
    for(int i=0;i<200&&!reader->atStart;++i)reader->navigate(-1);
    CHECK(reader->atStart);reader->navigate(-1);CHECK(!reader->active);
    reader->select(root/L"read.txt");reader->navigate(1);reader->select(root/L"binary.txt");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(!reader->available&&!state.reader->canFullscreen());
    reader->select(root/L"read.rtf");CHECK(!reader->loading);reader->navigate(1);
    CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->bitmap&&reader->atStart&&!reader->atEnd);
    if(reader->active){
        captureBitmap(reader->bitmap,L"reader-rtf.bmp");
        reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->rtfAnchor>0);
        auto anchor=reader->rtfAnchor;reader->key(VK_F11);CHECK(waitFor([&]{return !reader->loading;}));reader->key(VK_ESCAPE);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->rtfAnchor==anchor);
        reader->navigate(-1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->atStart);reader->navigate(-1);CHECK(!reader->active&&!reader->bitmap);
    }
    reader->select(root/L"image.rtf");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active);
    if(reader->bitmap){captureBitmap(reader->bitmap,L"reader-rtf-image.bmp");auto dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,reader->bitmap);BITMAP shape{};GetObjectW(reader->bitmap,sizeof(shape),&shape);bool teal=false;for(int y=0;y<shape.bmHeight&&!teal;y+=8)for(int x=0;x<shape.bmWidth;x+=8)if(GetPixel(dc,x,y)==RGB(0,128,128)){teal=true;break;}CHECK(teal);SelectObject(dc,old);DeleteDC(dc);}
    for(auto extension:{L"html",L"htm"}){
        reader->select(root/(std::wstring(L"read.")+extension));CHECK(!reader->loading);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->web);
        if(reader->active&&reader->web){
            CHECK(script(reader->web.Get(),L"htmlFrame.contentWindow.hacked===undefined")==L"true");
            CHECK(script(reader->web.Get(),L"getComputedStyle(htmlFrame.contentDocument.querySelector('h1')).color")==L"\"rgb(0, 128, 128)\"");
            CHECK(script(reader->web.Get(),L"htmlFrame.contentDocument.querySelector('img').naturalWidth")==L"0");
            CHECK(script(reader->web.Get(),L"getComputedStyle(htmlFrame.contentDocument.body).color")!=L"\"rgb(255, 0, 0)\"");
            capture(reader->web.Get(),L"reader-html.png");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(!reader->atStart);
            auto anchor=script(reader->web.Get(),L"htmlFrame.contentDocument.scrollingElement.scrollTop");reader->key(VK_F11);CHECK(waitFor([&]{return !reader->loading;}));CHECK(waitFor([&]{return !reader->web||script(reader->web.Get(),L"htmlFrame.clientHeight===innerHeight")==L"true";}));reader->key(VK_ESCAPE);CHECK(waitFor([&]{return !reader->loading;}));CHECK(waitFor([&]{return !reader->web||script(reader->web.Get(),L"htmlFrame.contentDocument.scrollingElement.scrollTop")==anchor;}));
            reader->navigate(-1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->atStart);reader->navigate(-1);CHECK(!reader->active&&!reader->web);
        }
    }
    }
    reader->select(root/L"read.docx");CHECK(reader->can(1)&&!reader->active&&!reader->web);
    reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->web);
    if(reader->active&&reader->web){
        CHECK(reader->containWebProcesses());
        CHECK(script(reader->web.Get(),L"htmlFrame.getAttribute('sandbox')")==L"\"allow-same-origin\"");
        CHECK(script(reader->web.Get(),L"htmlFrame.contentDocument.body.textContent.includes('caf\u00e9 \u65e5\u672c\u8a9e')")==L"true");
        CHECK(script(reader->web.Get(),L"!!htmlFrame.contentDocument.querySelector('table')")==L"true");
        CHECK(script(reader->web.Get(),L"['Document header','Document footer','Footnote content','List item','Final DOCX paragraph'].every(t=>htmlFrame.contentDocument.body.textContent.includes(t))")==L"true");
        CHECK(script(reader->web.Get(),L"!htmlFrame.contentDocument.querySelector('a[href],iframe,script,object')&&!htmlFrame.contentDocument.body.textContent.includes('UNSAFE CHUNK')")==L"true");
        CHECK(waitFor([&]{return script(reader->web.Get(),L"htmlFrame.contentDocument.querySelector('img')?.naturalWidth>0")==L"true";}));
        capture(reader->web.Get(),L"reader-docx.png");
        reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(!reader->atStart);
        auto anchor=script(reader->web.Get(),L"htmlFrame.contentDocument.scrollingElement.scrollTop");
        reader->key(VK_F11);CHECK(waitFor([&]{return !reader->loading;}));CHECK(waitFor([&]{return script(reader->web.Get(),L"htmlFrame.clientHeight===innerHeight")==L"true";}));
        reader->key(VK_ESCAPE);CHECK(waitFor([&]{return !reader->loading;}));CHECK(waitFor([&]{return script(reader->web.Get(),(L"Math.abs(htmlFrame.contentDocument.scrollingElement.scrollTop-"+anchor+L")<2").c_str())==L"true";}));
        script(reader->web.Get(),L"htmlFrame.contentWindow.scrollTo(0,1e9);htmlState()");CHECK(waitFor([&]{return reader->atEnd;}));CHECK(!reader->can(1));
        capture(reader->web.Get(),L"reader-docx-end.png");
        script(reader->web.Get(),L"htmlFrame.contentWindow.scrollTo(0,0);htmlState()");CHECK(waitFor([&]{return reader->atStart;}));reader->navigate(-1);CHECK(!reader->active&&!reader->web);
    }
    reader->select(root/L"single.docx");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active);
    if(reader->web){reader->fallbackChoice=[]{return IDC_READ_AS_TEXT;};reader->webBudget=1;reader->checkWebMemory();CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->plainText&&reader->extracted&&!reader->web);reader->fallbackChoice=[]{return 0;};}
    for(auto name:{L"bad.docx",L"encrypted.docx",L"xml-bad.docx",L"dtd.docx",L"traversal.docx"}){
        CHECK(!books::extractDocxText(root/name,[]{return false;}));
        reader->select(root/name);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->failed&&!reader->active);
    }
    {
        wchar_t previousRuntime[32768]{};auto runtimeLength=GetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",previousRuntime,32768);
        SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",(root/L"missing-runtime").c_str());
        reader->fallbackChoice=[]{return IDC_READ_AS_TEXT;};reader->select(root/L"read.docx");reader->navigate(1);
        CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->plainText&&reader->extracted&&!reader->web);
        SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",runtimeLength?previousRuntime:nullptr);reader->fallbackChoice=[]{return 0;};
    }
    CHECK(!books::extractDocxText(root/L"read.docx",[]{return true;}));
    {int calls=0;CHECK(!books::extractDocxText(root/L"read.docx",[&]{return ++calls>4;}));}
    for(auto extension:{L"rtf",L"html",L"docx"}){
        if(docxOnly&&std::wstring(extension)!=L"docx")continue;
        auto extracted=books::extractReaderText(root/(std::wstring(L"read.")+extension),std::wstring(extension)==L"rtf",[]{return false;});CHECK(extracted!=nullptr);
        if(extracted){auto text=books::loadReaderText(extracted->path);CHECK(text.has_value());if(text){CHECK(text->find(L"Formatted")!=std::wstring::npos);CHECK(text->find(L"caf\u00e9")!=std::wstring::npos);CHECK(text->find(L"\u65e5\u672c\u8a9e")!=std::wstring::npos);CHECK(text->find(L"window.hacked")==std::wstring::npos);}auto temporary=extracted->path;extracted.reset();CHECK(!std::filesystem::exists(temporary));}
    }
    if(!docxOnly){
    for(auto name:{L"utf-8-sig.txt",L"utf-16.txt"}){
        std::wstring combined;for(uint64_t i=0;;++i){auto chunk=books::loadReaderTextChunk(root/name,i);CHECK(chunk.has_value());if(!chunk)break;combined+=chunk->text;if(chunk->last)break;}
        std::wstring unit(32765,L'A');unit+=L"\U0001f4d6\r\n\u65e5";CHECK(combined==unit+unit+unit);
    }
    CHECK(!books::extractReaderText(root/L"read.html",false,[]{return true;}));
    reader->select(root/L"embedded.html");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active);
    if(reader->web){CHECK(script(reader->web.Get(),L"htmlFrame.contentDocument.querySelector('img').naturalWidth")==L"20");reader->fallbackChoice=[]{return IDC_READ_AS_TEXT;};reader->webBudget=1;reader->checkWebMemory();CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&reader->plainText&&reader->extracted&&!reader->web);reader->fallbackChoice=[]{return 0;};}
    reader->select(root/L"oversized.txt");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active&&!reader->textLast);CHECK(GetWindowTextLengthW(reader->textView)<=books::ReaderTextWindow+4);
    {auto late=books::loadReaderTextChunk(root/L"late-legacy.txt",1);CHECK(late&&late->text==L"caf\u00e9");}
    {
    PROCESS_MEMORY_COUNTERS_EX before{sizeof(before)},after{sizeof(after)};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),sizeof(before));
    reader->select(root/L"large.txt");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->active);
    SendMessageW(reader->textView,EM_LINESCROLL,0,INT_MAX/2);reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->textIndex==1);
    SendMessageW(reader->textView,EM_LINESCROLL,0,-INT_MAX/2);reader->navigate(-1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->textIndex==0);
    GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after),sizeof(after));CHECK(after.PrivateUsage<before.PrivateUsage+16*1024*1024);
    std::cout<<"64 MiB TXT reader memory growth: "<<(static_cast<int64_t>(after.PrivateUsage)-static_cast<int64_t>(before.PrivateUsage))<<" bytes\n";
    }
    }
    for(auto name:{L"read.rtf",L"read.html",L"read.docx",L"large.txt"}){if(docxOnly&&std::wstring(name)!=L"read.docx")continue;reader->select(root/name);reader->navigate(1);reader->select(root/L"binary.txt");reader->navigate(1);CHECK(waitFor([&]{return !reader->loading;}));CHECK(reader->failed&&!reader->active);}
    reader->select(root/L"book.epub");DestroyWindow(window);reader.reset();
    CoUninitialize();std::cout<<(failures?"FAIL":"PASS")<<": reader integration ("<<failures<<" failures)\n";return failures?1:0;
}
