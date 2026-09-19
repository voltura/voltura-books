#include "interactive_test.h"
#include "core.h"
namespace books {
static fs::path rememberedByBrowserTest;
static void fixtureSaveBrowseFolder(const fs::path& folder){rememberedByBrowserTest=folder;}
}
#define saveBrowseFolder fixtureSaveBrowseFolder
#include "../src/browser.cpp"
#undef saveBrowseFolder
#include <fstream>
#include <functional>
#include <iostream>
static int mode=0,failures=0;
static void fail(int line) { ++failures; std::cerr<<"Failure at line "<<line<<" in mode "<<mode<<"\n"; }
static int settingsOpened=0,aboutOpened=0,updateOpened=0;
static bool testMenuSettings(HWND){++settingsOpened;return true;}
static bool testMenuAbout(HWND,bool checkForUpdates){if(checkForUpdates)++updateOpened;else ++aboutOpened;return false;}
static HWND expectedOwner=nullptr;
static int ownerTestMode=0;
static bool browserActivated=false;
static LRESULT CALLBACK scrollPaintHook(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto result=DefSubclassProc(window,message,wp,lp);
    if(message==WM_NCCALCSIZE || message==WM_NCPAINT || message==WM_PAINT)
        if(GetWindowLongPtrW(window,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL))fail(__LINE__);
    return result;
}
static void testScrolling(HWND window,books::Browser& state) {
    auto list=GetDlgItem(window,IDC_FILE_LIST),grid=GetDlgItem(window,IDC_FILE_GRID),bar=GetDlgItem(window,IDC_BOOK_SCROLL);
    UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    for(bool tiles:{false,true}) {
        books::setView(window,state,tiles);
        auto view=tiles?grid:list;
        SetWindowSubclass(view,scrollPaintHook,42,0);
        for(int i=3;i<80;++i) {
            if(tiles){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;item.pszText=const_cast<wchar_t*>(L"Scroll fixture");ListView_InsertItem(view,&item);}
            else SendMessageW(view,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Scroll fixture"));
        }
        auto position=[&] { if(!tiles)return static_cast<int>(SendMessageW(view,LB_GETTOPINDEX,0,0));POINT origin{};ListView_GetOrigin(view,&origin);return static_cast<int>(origin.y); };
        auto reset=[&] { if(tiles)ListView_Scroll(view,0,-position());else SendMessageW(view,LB_SETTOPINDEX,0,0); };
        auto wheel=[&](HWND target,int delta) { SendMessageW(target,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),0); };
        auto synced=[&] {
            SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(bar,SB_CTL,&info);
            if(info.nPos!=position() || info.nMax<=static_cast<int>(info.nPage))fail(__LINE__);
            if(GetWindowLongPtrW(view,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL))fail(__LINE__);
            RedrawWindow(view,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_UPDATENOW);
        };
        reset();books::updateListScrollbar(view,bar);synced();
        if(tiles){ListView_SetItemState(view,0,LVIS_SELECTED,LVIS_SELECTED);}else SendMessageW(view,LB_SETSEL,TRUE,0);
        auto selected=tiles?ListView_GetSelectedCount(view):SendMessageW(view,LB_GETSELCOUNT,0,0);
        wheel(view,-WHEEL_DELTA);auto step=position();
        if((lines && step<=0)||(!lines && step!=0))fail(__LINE__);
        synced();wheel(view,WHEEL_DELTA);if(position()!=0)fail(__LINE__);
        // Small device deltas together must produce exactly one wheel step.
        for(int i=0;i<12;++i)wheel(view,-WHEEL_DELTA/12);
        if(position()!=step)fail(__LINE__);
        wheel(bar,WHEEL_DELTA);if(position()!=0)fail(__LINE__);
        wheel(bar,WHEEL_DELTA);if(position()!=0)fail(__LINE__);
        SCROLLINFO extent{sizeof(extent),SIF_ALL};GetScrollInfo(bar,SB_CTL,&extent);
        for(int i=0;i<extent.nMax/(std::max)(1,step)+2;++i)wheel(bar,-WHEEL_DELTA);
        auto bottom=position();wheel(bar,-WHEEL_DELTA);if(position()!=bottom)fail(__LINE__);
        synced();
        if((tiles?ListView_GetSelectedCount(view):SendMessageW(view,LB_GETSELCOUNT,0,0))!=selected)fail(__LINE__);
        if(tiles?!(ListView_GetItemState(view,0,LVIS_SELECTED)&LVIS_SELECTED):SendMessageW(view,LB_GETSEL,0,0)!=1)fail(__LINE__);
        reset();synced();
        RECT bounds{};GetClientRect(bar,&bounds);
        SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bounds.right/2,bounds.bottom-2));
        if(position()<=0)fail(__LINE__);synced();
        reset();
        SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bounds.right/2,2));
        for(int y=10;y<bounds.bottom;y+=10) { SendMessageW(bar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bounds.right/2,y));synced(); }
        SendMessageW(bar,WM_LBUTTONUP,0,0);if(position()<=0 || GetCapture()==bar)fail(__LINE__);
        if(tiles){ListView_SetItemState(view,0,LVIS_FOCUSED,LVIS_FOCUSED);}else SendMessageW(view,LB_SETCARETINDEX,0,FALSE);
        reset();SendMessageW(view,WM_KEYDOWN,VK_NEXT,0);SendMessageW(view,WM_KEYDOWN,VK_NEXT,0);
        if(position()<=0)fail(__LINE__);synced();
        if(tiles) { ListView_EnsureVisible(view,79,FALSE);if(position()<=0)fail(__LINE__);synced(); }
        // Switching the shared bar must discard a partial wheel movement.
        reset();wheel(view,-1);
        books::useListScrollbar(tiles?list:grid,bar);books::useListScrollbar(view,bar);
        reset();for(int i=0;i<12;++i)wheel(view,-WHEEL_DELTA/12);
        if(position()!=step)fail(__LINE__);
        RemoveWindowSubclass(view,scrollPaintHook,42);
        if(!tiles)for(int i=79;i>=3;--i)SendMessageW(list,LB_DELETESTRING,i,0);
    }
    books::setView(window,state,false);
    SendMessageW(list,LB_SETSEL,FALSE,-1);SendMessageW(list,LB_SETTOPINDEX,0,0);
}
static LRESULT CALLBACK ownershipHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_ACTIVATE){auto window=reinterpret_cast<HWND>(wp);
        if(GetDlgItem(window,IDC_FILE_LIST)){
            browserActivated=true;
            wchar_t closeLabel[32]{};GetDlgItemTextW(window,IDCANCEL,closeLabel,32);
            if(std::wstring(closeLabel)!=L"Close")fail(__LINE__);
            if(IsWindowVisible(expectedOwner)||IsWindowEnabled(expectedOwner))fail(__LINE__);
            if(GetWindow(window,GW_OWNER)!=expectedOwner||!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_APPWINDOW))fail(__LINE__);
            if(ownerTestMode==2){SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,0);books::selectFile(window,*reinterpret_cast<books::Browser*>(GetWindowLongPtrW(window,DWLP_USER)));}
            PostMessageW(window,ownerTestMode==1?WM_CLOSE:WM_COMMAND,ownerTestMode==2?IDOK:IDCANCEL,0);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static void testRefresh(HWND window,books::Browser& state) {
    auto folder=state.folder;
    const auto added=folder/L"Added.pdf";
    std::cout<<"Refresh checks at "<<GetDpiForWindow(window)<<" DPI\n";
    wchar_t label[64]{};GetDlgItemTextW(window,IDC_REFRESH,label,64);
    if(std::wstring(label)!=L"Refresh")fail(__LINE__);
    wchar_t tooltipText[128]{};TOOLINFOW tool{sizeof(tool)};tool.lpszText=tooltipText;tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,IDC_REFRESH));
    if(!SendMessageW(state.tooltip,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&tool)) || std::wstring(tool.lpszText)!=L"Refresh (F5)")fail(__LINE__);
    TOOLINFOW menuTool{sizeof(menuTool)};menuTool.lpszText=tooltipText;menuTool.hwnd=window;menuTool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,IDC_BROWSER_MENU));
    if(!SendMessageW(state.tooltip,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&menuTool)) || std::wstring(menuTool.lpszText)!=L"Menu")fail(__LINE__);
    RECT refresh{},choose{};GetWindowRect(GetDlgItem(window,IDC_REFRESH),&refresh);GetWindowRect(GetDlgItem(window,IDC_FOLDER),&choose);
    if(refresh.right>=choose.left || refresh.top!=choose.top || refresh.bottom!=choose.bottom)fail(__LINE__);
    SendDlgItemMessageW(window,IDC_TYPE_FIRST+1,BM_CLICK,0,0);
    SetDlgItemTextW(window,IDC_SEARCH,L".pdf");
    SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,1,0);
    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,0);
    books::selectFile(window,state);
    const auto selected=state.current;
    books::setView(window,state,true);
    auto thumbnails=state.thumbnails;
    {std::ofstream file(added);file<<"new file";}
    {std::ofstream file(selected,std::ios::app);file<<" changed";}
    SendDlgItemMessageW(window,IDC_REFRESH,BM_CLICK,0,0);
    if(state.allFiles.size()!=4 || state.files.size()!=2 || state.allTypes || !state.enabled[1] || !state.tiles || state.current!=selected || state.sizes.at(selected)!=15 || state.thumbnails==thumbnails)fail(__LINE__);
    if(state.files.front()!=selected || SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,0,0)!=1 || SendDlgItemMessageW(window,IDC_SORT,CB_GETCURSEL,0,0)!=1)fail(__LINE__);
    GetDlgItemTextW(window,IDC_SEARCH,label,64);if(std::wstring(label)!=L".pdf")fail(__LINE__);
    // Preserve multiple selections, including an item that moves in the sort order.
    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,1);
    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,0,FALSE);
    SendDlgItemMessageW(window,IDC_REFRESH,BM_CLICK,0,0);
    if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELCOUNT,0,0)!=2)fail(__LINE__);
    books::fs::remove(selected);
    // Exercise the real message hook with focus in the search field.
    SetFocus(GetDlgItem(window,IDC_SEARCH));PostMessageW(GetDlgItem(window,IDC_SEARCH),WM_KEYDOWN,VK_F5,0);
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(state.files.size()!=1 || !state.current.empty() || IsWindowEnabled(GetDlgItem(window,IDC_OPEN_FILE)))fail(__LINE__);
    state.fullscreen=true;books::fs::remove(added);
    SendMessageW(window,books::DialogShortcut,VK_F5,0);
    if(state.files.size()!=1)fail(__LINE__);
    state.fullscreen=false;SendMessageW(window,books::DialogShortcut,VK_F5,0);
    if(!state.files.empty() || !state.current.empty() || IsWindowEnabled(GetDlgItem(window,IDOK)))fail(__LINE__);
    auto oldFiles=state.allFiles;auto oldThumbnails=state.thumbnails;
    state.folder=folder/L"missing";
    bool failed=false;try{books::refreshFolder(window,state);}catch(const std::exception&){failed=true;}
    if(!failed || state.allFiles!=oldFiles || state.thumbnails!=oldThumbnails)fail(__LINE__);
    state.folder=folder;
    {std::ofstream file(selected);file<<"fixture";}
    SetDlgItemTextW(window,IDC_SEARCH,L"");SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_CLICK,0,0);
    SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,0,0);books::setView(window,state,false);
    SendDlgItemMessageW(window,IDC_REFRESH,BM_CLICK,0,0);
}
static void testBrowserMenu(HWND window,books::Browser& state) {
    const auto folder=state.folder,current=state.current;const auto files=state.allFiles;const bool recursive=state.includeSubfolders;
    const int initialSettings=settingsOpened,initialAbout=aboutOpened,initialUpdate=updateOpened;
    RECT menuButton{},heading{};GetWindowRect(GetDlgItem(window,IDC_BROWSER_MENU),&menuButton);GetWindowRect(GetDlgItem(window,IDC_HEADING),&heading);
    if(menuButton.right>=heading.left||menuButton.top!=heading.top)fail(__LINE__);
    SendDlgItemMessageW(window,IDC_BROWSER_MENU,BM_CLICK,0,0);
    if(!state.menu||!IsWindowVisible(state.menu)||GetWindow(state.menu,GW_OWNER)!=window||GetFocus()!=GetDlgItem(state.menu,IDC_MENU_SETTINGS))fail(__LINE__);
    wchar_t label[64]{};GetDlgItemTextW(state.menu,IDC_MENU_SETTINGS,label,64);if(std::wstring(label)!=L"Settings")fail(__LINE__);
    GetDlgItemTextW(state.menu,IDC_MENU_ABOUT,label,64);if(std::wstring(label)!=L"About")fail(__LINE__);
    GetDlgItemTextW(state.menu,IDC_MENU_CHECK_UPDATES,label,64);if(std::wstring(label)!=L"Check for updates")fail(__LINE__);
    SendMessageW(GetDlgItem(state.menu,IDC_MENU_SETTINGS),WM_KEYDOWN,VK_DOWN,0);
    if(GetFocus()!=GetDlgItem(state.menu,IDC_MENU_ABOUT))fail(__LINE__);
    SendMessageW(GetDlgItem(state.menu,IDC_MENU_ABOUT),WM_KEYDOWN,VK_TAB,0);
    if(GetFocus()!=GetDlgItem(state.menu,IDC_MENU_CHECK_UPDATES))fail(__LINE__);
    SendMessageW(GetDlgItem(state.menu,IDC_MENU_CHECK_UPDATES),WM_KEYDOWN,VK_ESCAPE,0);
    if(state.menu||GetFocus()!=GetDlgItem(window,IDC_BROWSER_MENU))fail(__LINE__);
    books::showBrowserMenu(window,state);auto popup=state.menu;
    books::showBrowserMenu(window,state);if(state.menu||!popup)fail(__LINE__);
    books::showBrowserMenu(window,state);SendMessageW(state.menu,WM_COMMAND,IDC_MENU_SETTINGS,0);
    if(state.menu||settingsOpened!=initialSettings+1)fail(__LINE__);
    books::showBrowserMenu(window,state);SendMessageW(state.menu,WM_COMMAND,IDC_MENU_ABOUT,0);
    if(state.menu||aboutOpened!=initialAbout+1||updateOpened!=initialUpdate)fail(__LINE__);
    books::showBrowserMenu(window,state);SendMessageW(state.menu,WM_COMMAND,IDC_MENU_CHECK_UPDATES,0);
    if(state.menu||updateOpened!=initialUpdate+1||state.folder!=folder||state.current!=current||state.allFiles!=files||state.includeSubfolders!=recursive)fail(__LINE__);
}
static bool pumpUntil(HWND window,const std::function<bool()>& ready,DWORD timeout=5000) {
    const auto deadline=GetTickCount64()+timeout;
    while(!ready()&&GetTickCount64()<deadline) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
        Sleep(1);
    }
    return ready();
}
static void testSubfolders(HWND window,books::Browser& state) {
    const auto first=state.folder/L"Shelf A",second=state.folder/L"Shelf B";
    books::fs::create_directories(first);books::fs::create_directories(second/L"Images");
    for(const auto& path:{first/L"Duplicate.epub",second/L"Duplicate.epub",second/L"Images"/L"Cover.png",second/L"Ignored.exe"}){std::ofstream file(path);file<<"fixture";}
    if(SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_GETCHECK,0,0)!=BST_UNCHECKED||state.includeSubfolders)fail(__LINE__);
    SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_CLICK,0,0);
    wchar_t status[128]{};GetDlgItemTextW(window,IDC_SCAN_STATUS,status,128);
    if(!state.includeSubfolders||!state.scanActive||state.allFiles.size()!=state.directFiles.size()||std::wstring(status).find(L"Scanning subfolders")!=0)fail(__LINE__);
    const auto scanGeneration=state.scanGeneration;
    SendDlgItemMessageW(window,IDC_BROWSER_MENU,BM_CLICK,0,0);if(!state.menu)fail(__LINE__);
    SendMessageW(state.menu,WM_CLOSE,0,0);
    if(state.menu||!state.includeSubfolders||state.scanGeneration!=scanGeneration)fail(__LINE__);
    if(!pumpUntil(window,[&]{return !state.scanActive;}))fail(__LINE__);
    if(state.allFiles.size()!=state.directFiles.size()+3||state.subfolderFilesFound!=3||(GetWindowLongPtrW(GetDlgItem(window,IDC_TYPE_FIRST+6),GWL_STYLE)&WS_VISIBLE)==0)fail(__LINE__);
    GetDlgItemTextW(window,IDC_SCAN_STATUS,status,128);if(*status)fail(__LINE__);
    SetDlgItemTextW(window,IDC_SEARCH,L"Shelf A\\Duplicate.epub");
    if(state.files.size()!=1||books::displayedPath(state,state.files.front())!=L"Shelf A\\Duplicate.epub")fail(__LINE__);
    const auto longPath=state.folder/L"More books"/L"Several nested folders"/L"some-book.epub";
    const auto compact=books::compactTilePath(state,longPath,34);
    if(compact!=L"More books\\...\\some-book.epub")fail(__LINE__);
    SetDlgItemTextW(window,IDC_SEARCH,L"");SendDlgItemMessageW(window,IDC_VIEW_THUMBS,BM_CLICK,0,0);
    if(ListView_GetItemCount(GetDlgItem(window,IDC_FILE_GRID))!=static_cast<int>(state.directFiles.size()+3))fail(__LINE__);
    SendDlgItemMessageW(window,IDC_VIEW_LIST,BM_CLICK,0,0);
    SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_CLICK,0,0);
    if(state.includeSubfolders||state.scanActive||state.allFiles!=state.directFiles||state.files.size()!=state.directFiles.size())fail(__LINE__);
    SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_CLICK,0,0);
    SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_CLICK,0,0);
    const auto settle=GetTickCount64()+50;pumpUntil(window,[&]{return GetTickCount64()>=settle;},100);
    if(state.includeSubfolders||state.allFiles!=state.directFiles)fail(__LINE__);
    books::fs::remove_all(first);books::fs::remove_all(second);
}
static INT_PTR CALLBACK testProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_APP+90) {
        auto state=reinterpret_cast<books::Browser*>(GetWindowLongPtrW(window,DWLP_USER));
        if(state->files.size()!=3) fail(__LINE__);
        if(mode==0) {
            testBrowserMenu(window,*state);
            auto switched=state->initialFolder/(L"Folder with spaces \u65e5\u672c\u8a9e");
            auto empty=state->initialFolder/L"Empty folder";
            books::fs::create_directories(switched);books::fs::create_directories(empty);
            {std::ofstream file(switched/L"Switched.docx");file<<"fixture";}
            if(!books::navigateBrowseBooks(switched)||state->folder!=switched||state->files.size()!=1||state->files.front().filename()!=L"Switched.docx"||books::rememberedByBrowserTest!=switched)fail(__LINE__);
            if(!books::navigateBrowseBooks(empty)||state->folder!=empty||!state->files.empty()||books::rememberedByBrowserTest!=empty)fail(__LINE__);
            if(!books::navigateBrowseBooks(state->initialFolder)||state->folder!=state->initialFolder||state->files.size()!=3||books::rememberedByBrowserTest!=state->initialFolder)fail(__LINE__);
            auto remembered=books::rememberedByBrowserTest;
            if(books::navigateBrowseBooks(state->initialFolder/L"Missing")||state->folder!=state->initialFolder||books::rememberedByBrowserTest!=remembered)fail(__LINE__);
            books::fs::remove_all(switched);books::fs::remove_all(empty);
            for(bool testSending:{true,false}) {
                state->previewOnly=testSending;
                for(int count=0;count<=3;++count) {
                    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);
                    for(int i=0;i<count;++i)SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,i);
                    books::selectFile(window,*state);
                    wchar_t label[128]{};GetDlgItemTextW(window,IDOK,label,128);
                    auto expected=testSending?(count<=1?std::wstring(L"Test sending"):L"Test sending "+std::to_wstring(count)+L" books"):(count==0?std::wstring(L"Send books"):count==1?std::wstring(L"Send book"):L"Send "+std::to_wstring(count)+L" books");
                    if(label!=expected || (IsWindowEnabled(GetDlgItem(window,IDOK))!=FALSE)!=(count>0))fail(__LINE__);
                }
            }
            SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);books::selectFile(window,*state);
            books::styleBrowser(window);
            if((GetWindowLongPtrW(GetDlgItem(window,IDC_FILE_LIST),GWL_STYLE)&WS_BORDER)||
               (GetWindowLongPtrW(GetDlgItem(window,IDC_FILE_LIST),GWL_EXSTYLE)&(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE)))fail(__LINE__);
            RECT sortBounds{},viewBounds{};GetWindowRect(GetDlgItem(window,IDC_SORT),&sortBounds);GetWindowRect(GetDlgItem(window,IDC_VIEW_LIST),&viewBounds);
            if(sortBounds.top!=viewBounds.top || sortBounds.bottom!=viewBounds.bottom) fail(__LINE__);
            if(SendDlgItemMessageW(window,IDC_SORT,CB_GETITEMHEIGHT,0,0)!=viewBounds.bottom-viewBounds.top) fail(__LINE__);
            if(SendDlgItemMessageW(window,IDC_SORT,CB_GETCOUNT,0,0)!=4) fail(__LINE__);
            testScrolling(window,*state);
            // Reader controls must not identify Browse Books as the update dialog.
            if(GetDlgItem(window,IDC_UPDATE_CHECK)) fail(__LINE__);
            if(books::usesDarkTheme(window)) {
                auto dc=GetDC(window);
                for(auto id:{IDC_HEADING,-1,IDC_FOLDER_PATH,IDC_FILE_COUNT,IDC_SCAN_STATUS,IDC_BROWSER_HELP}) {
                    auto brush=reinterpret_cast<HBRUSH>(SendMessageW(window,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(GetDlgItem(window,id))));
                    if(brush!=books::dialogBackground(window)) fail(__LINE__);
                }
                auto brush=reinterpret_cast<HBRUSH>(SendMessageW(window,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(GetDlgItem(window,IDC_FILENAME))));
                if(brush!=books::panelBackground(window)) fail(__LINE__);
                ReleaseDC(window,dc);
            }
            if(!state->allTypes || SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_GETCHECK,0,0)!=BST_CHECKED) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST,BM_CLICK,0,0);
            if(state->files.size()!=1 || state->allTypes) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+1,BM_CLICK,0,0);
            if(state->files.size()!=2) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST,BM_CLICK,0,0);
            if(state->files.size()!=1 || state->files[0].extension()!=L".pdf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+1,BM_CLICK,0,0);
            if(state->files.size()!=3 || !state->allTypes || state->enabled[1] || SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_GETCHECK,0,0)!=BST_CHECKED) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_CLICK,0,0);
            if(state->files.size()!=3 || !state->allTypes || state->enabled[1]) fail(__LINE__);
            if((GetWindowLongPtrW(GetDlgItem(window,IDC_TYPE_FIRST+6),GWL_STYLE)&WS_VISIBLE)) fail(__LINE__);
            if(books::groupOf(L"a.png")!=books::groupOf(L"b.jpg") || books::groupOf(L"a.gif")!=books::groupOf(L"b.bmp")) fail(__LINE__);
            auto original=state->allFiles;
            state->allFiles.push_back(state->initialFolder/L"Picture.png"); state->allFiles.push_back(state->initialFolder/L"Photo.jpg");
            books::applyFilters(window,*state);
            if(!(GetWindowLongPtrW(GetDlgItem(window,IDC_TYPE_FIRST+6),GWL_STYLE)&WS_VISIBLE)) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+6,BM_CLICK,0,0);
            if(state->files.size()!=2 || state->allTypes) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+6,BM_CLICK,0,0);
            if(state->files.size()!=5 || !state->allTypes || state->enabled[6] || SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_GETCHECK,0,0)!=BST_CHECKED) fail(__LINE__);
            state->allFiles=original; SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_CLICK,0,0);
            SendDlgItemMessageW(window,IDC_VIEW_THUMBS,BM_CLICK,0,0);
            if(!state->tiles || ListView_GetItemCount(GetDlgItem(window,IDC_FILE_GRID))!=3) fail(__LINE__);
            RedrawWindow(GetDlgItem(window,IDC_FILE_GRID),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            ListView_SetItemState(GetDlgItem(window,IDC_FILE_GRID),1,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            if(!pumpUntil(window,[&]{return !state->gridSelectionPending&&SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,1,0)==1;})) fail(__LINE__);
            ListView_SetSelectionMark(GetDlgItem(window,IDC_FILE_GRID),0);
            books::rebuildGrid(window,*state);
            if(ListView_GetNextItem(GetDlgItem(window,IDC_FILE_GRID),-1,LVNI_FOCUSED)!=1 || ListView_GetSelectionMark(GetDlgItem(window,IDC_FILE_GRID))!=0 || !(ListView_GetItemState(GetDlgItem(window,IDC_FILE_GRID),1,LVIS_SELECTED)&LVIS_SELECTED)) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_VIEW_LIST,BM_CLICK,0,0);
            if(state->tiles) fail(__LINE__);
            SetDlgItemTextW(window,IDC_SEARCH,L"B.PDF");
            if(state->files.size()!=1 || state->files[0].extension()!=L".pdf") fail(__LINE__);
            SetDlgItemTextW(window,IDC_SEARCH,L"no match");
            if(!state->files.empty() || IsWindowEnabled(GetDlgItem(window,IDOK))) fail(__LINE__);
            if(!(GetWindowLongPtrW(GetDlgItem(window,IDC_CLEAR_SEARCH),GWL_STYLE)&WS_VISIBLE))fail(__LINE__);
            SendDlgItemMessageW(window,IDC_CLEAR_SEARCH,BM_CLICK,0,0);
            if(GetWindowTextLengthW(GetDlgItem(window,IDC_SEARCH)) || state->files.size()!=3 || GetFocus()!=GetDlgItem(window,IDC_SEARCH) || (GetWindowLongPtrW(GetDlgItem(window,IDC_CLEAR_SEARCH),GWL_STYLE)&WS_VISIBLE))fail(__LINE__);
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,1,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files[0].filename()!=L"C.rtf") fail(__LINE__);
            state->modified[state->initialFolder/L"A.epub"]=20;
            state->modified[state->initialFolder/L"B.pdf"]=30;
            state->modified[state->initialFolder/L"C.rtf"]=10;
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,2,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files.front().filename()!=L"B.pdf" || state->files.back().filename()!=L"C.rtf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,3,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files.front().filename()!=L"C.rtf" || state->files.back().filename()!=L"B.pdf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,0,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            books::BookDetails details; details.title=L"Example"; details.author=L"Author"; details.publisher=L"Publisher";
            auto text=books::bookInformationText(state->files[0],details);
            if(!(text.find(L"\nTitle:")<text.find(L"\nAuthor:") && text.find(L"\nAuthor:")<text.find(L"\nPublisher:") && text.find(L"\nPublisher:")<text.find(L"\nFormat:") && text.find(L"\nFormat:")<text.find(L"\nSize:"))) fail(__LINE__);
        }
        if(mode==0){testRefresh(window,*state);testSubfolders(window,*state);}
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,0);
        if(mode==1) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,2);
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,mode==1 ? 2 : 0,FALSE);
        books::selectFile(window,*state);
        if(state->current!=state->files[mode==1 ? 2 : 0]) fail(__LINE__);
        wchar_t label[100]{}; GetDlgItemTextW(window,IDOK,label,100);
        if(std::wstring(label)!=(mode==1 ? L"Send 2 books" : L"Send book")) fail(__LINE__);
        PostMessageW(window,WM_COMMAND,mode==2 ? IDCANCEL : IDOK,0); return TRUE;
    }
    auto result=books::proc(window,message,wp,lp);
    if(message==WM_INITDIALOG) PostMessageW(window,WM_APP+90,0,0);
    return result;
}
int wmain(int argc,wchar_t** argv) {
    if(!interactiveTestsEnabled())return 77;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    if(argc==2) { books::browseBooks(nullptr,argv[1]); return 0; } // Preview never sends.
    auto root=books::fs::temp_directory_path()/(L"VolturaBooks-browser-"+std::to_wstring(GetCurrentProcessId())); books::fs::create_directories(root);
    auto remembered=root/L"remembered",downloads=root/L"downloads",documents=root/L"documents";
    books::fs::create_directories(remembered); books::fs::create_directories(downloads); books::fs::create_directories(documents);
    if(books::firstExistingFolder(remembered,downloads,documents)!=remembered) fail(__LINE__);
    books::fs::remove(remembered);
    { std::ofstream file(remembered); file<<"not a folder"; }
    if(books::firstExistingFolder(remembered,downloads,documents)!=downloads) fail(__LINE__);
    books::fs::remove(remembered);
    books::fs::remove(downloads);
    if(books::firstExistingFolder(remembered,downloads,documents)!=documents) fail(__LINE__);
    books::fs::remove(documents);
    if(!books::firstExistingFolder(remembered,downloads,documents).empty()) fail(__LINE__);
    for(auto name:{L"A.epub",L"B.pdf",L"C.rtf",L"Ignored.exe"}) { std::ofstream f(root/name); f<<"fixture"; }
    for(auto dpiContext:{DPI_AWARENESS_CONTEXT_UNAWARE,DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2}) {
    auto previousDpi=SetThreadDpiAwarenessContext(dpiContext);
    for(mode=0;mode<3;++mode) {
        books::rememberedByBrowserTest.clear();
        books::Browser state; state.initialFolder=root; state.rememberInitialFolder=mode==0;state.menuActions={testMenuSettings,testMenuAbout};
        auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_BROWSER),nullptr,testProc,reinterpret_cast<LPARAM>(&state));
        if(mode==0 && books::rememberedByBrowserTest!=root)fail(__LINE__);
        if(mode==2) { if(result!=IDCANCEL || !state.selected.empty()) fail(__LINE__); }
        else if(result!=IDOK || state.selected.size()!=(mode==1 ? 2 : 1) || state.selected.front()!=root/L"A.epub" || (mode==1 && state.selected.back()!=root/L"C.rtf")) fail(__LINE__);
    }
    SetThreadDpiAwarenessContext(previousDpi);
    }
    expectedOwner=CreateWindowExW(WS_EX_APPWINDOW,L"STATIC",L"Sending window test",WS_OVERLAPPEDWINDOW,0,0,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(expectedOwner,SW_SHOW);
    RECT original{};GetWindowRect(expectedOwner,&original);
    auto hook=SetWindowsHookExW(WH_CBT,ownershipHook,nullptr,GetCurrentThreadId());
    if(!hook)fail(__LINE__);
    for(ownerTestMode=0;ownerTestMode<3;++ownerTestMode){
        ShowWindow(expectedOwner,SW_SHOW);
        browserActivated=false;auto selected=books::browseBooks(expectedOwner,root,true);
        if(!browserActivated||IsWindowVisible(expectedOwner)||!IsWindowEnabled(expectedOwner))fail(__LINE__);
        RECT restored{};GetWindowRect(expectedOwner,&restored);if(!EqualRect(&original,&restored))fail(__LINE__);
        if(selected.size()!=(ownerTestMode==2?1:0))fail(__LINE__);
    }
    DestroyWindow(expectedOwner);expectedOwner=nullptr;
    for(ownerTestMode=0;ownerTestMode<3;++ownerTestMode){
        browserActivated=false;auto selected=books::browseBooks(nullptr,root,true);
        if(!browserActivated||selected.size()!=(ownerTestMode==2?1:0))fail(__LINE__);
    }
    UnhookWindowsHookEx(hook);
    // Detached preview work owns its state and does not gate selection/cancellation.
    for(auto name:{L"A.epub",L"B.pdf",L"C.rtf",L"Ignored.exe"}) { std::error_code error; books::fs::remove(root/name,error); }
    std::error_code error; books::fs::remove(root,error);
    std::cout<<(failures ? "FAIL" : "PASS")<<": folder filter, navigation, single/multiple selection, refresh, cancel\n";
    CoUninitialize(); return failures ? 1 : 0;
}
