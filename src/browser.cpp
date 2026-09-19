#include "browser.h"
#include "book_information.h"
#include "cover.h"
#include "reader.h"
#include "formats.h"
#include "placeholder.h"
#include "resource.h"
#include "theme.h"
#include "list_scrollbar.h"
#include "file_icons.h"
#include <array>
#include <deque>
#include <set>
#include <map>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <algorithm>
#include <uxtheme.h>
namespace books {
namespace {
constexpr UINT PreviewReady=WM_APP+20;
constexpr UINT ThumbnailsReady=WM_APP+21;
constexpr const wchar_t* GroupNames[]={L"EPUB",L"PDF",L"RTF",L"TXT",L"HTML",L"Word",L"Images"};
int groupOf(const fs::path& file) {
    auto format=fileFormat(file.extension().wstring());
    if(!format) return -1;
    auto index=format-FileFormats;
    return index<4 ? static_cast<int>(index) : index<6 ? 4 : index<8 ? 5 : 6;
}
struct Thumbnails {
    std::mutex mutex; std::condition_variable wake; bool stop=false;
    std::deque<fs::path> requests,order; std::set<fs::path> pending;
    std::map<fs::path,HBITMAP> images;
    ~Thumbnails(){for(auto [path,image]:images) if(image) DeleteObject(image);}
};
struct Preview {
    HBITMAP cover=nullptr; BookDetails details;
    ~Preview() { if(cover) DeleteObject(cover); }
};
struct Worker {
    std::mutex mutex; std::condition_variable wake;
    bool stop=false,pending=false; size_t version=0;
    fs::path path; int width=0,height=0;
    std::shared_ptr<Preview> result;
};
struct Browser {
    std::unique_ptr<Reader> reader;
    bool fullscreen=false;
    HWND tooltip=nullptr;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowStyle=0,windowExStyle=0;
    UINT savedDpi=96;
    struct ChildPlacement { HWND window; RECT bounds; bool visible; };
    std::vector<ChildPlacement> childPlacements;
    std::vector<fs::path> files,allFiles,selected;
    std::array<bool,std::size(GroupNames)> enabled{};
    bool allTypes=true,tiles=false,rebuilding=false;
    bool previewOnly=false;
    HIMAGELIST imageList=nullptr;
    std::shared_ptr<Thumbnails> thumbnails=std::make_shared<Thumbnails>();
    std::map<fs::path,unsigned long long> sizes;
    std::map<fs::path,unsigned long long> modified;
    std::map<std::wstring,HICON> icons;
    Browser() = default;
    fs::path current,initialFolder,folder;
    std::shared_ptr<Worker> worker=std::make_shared<Worker>();
    ~Browser() { { std::lock_guard lock(thumbnails->mutex); thumbnails->stop=true; } thumbnails->wake.notify_one(); if(imageList) ImageList_Destroy(imageList); { std::lock_guard lock(worker->mutex); worker->stop=true; } worker->wake.notify_one(); for(auto [extension,icon]:icons) if(icon) DestroyIcon(icon); }
};
void toggleFullscreen(HWND window,Browser& state) {
    if(!state.reader || (!state.fullscreen&&!state.reader->canFullscreen()))return;
    auto cover=GetDlgItem(window,IDC_COVER);
    if(!state.fullscreen){
        state.savedDpi=GetDpiForWindow(window);GetWindowPlacement(window,&state.placement);
        state.windowStyle=GetWindowLongPtrW(window,GWL_STYLE);state.windowExStyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
        state.childPlacements.clear();
        for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            if(child==state.reader->window())continue;
            RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
            state.childPlacements.push_back({child,r,(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0});ShowWindow(child,SW_HIDE);
        }
        state.fullscreen=true;
        SetWindowLongPtrW(window,GWL_STYLE,state.windowStyle&~(WS_CAPTION|WS_THICKFRAME|DS_MODALFRAME));
        SetWindowLongPtrW(window,GWL_EXSTYLE,state.windowExStyle&~(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE));
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        SetWindowPos(window,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_FRAMECHANGED);
        RECT r{};GetClientRect(window,&r);SetWindowPos(cover,nullptr,0,0,r.right,r.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
    }else{
        state.fullscreen=false;
        SetWindowLongPtrW(window,GWL_STYLE,state.windowStyle);SetWindowLongPtrW(window,GWL_EXSTYLE,state.windowExStyle);
        SetWindowPlacement(window,&state.placement);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
        UINT dpi=GetDpiForWindow(window);
        for(auto child:state.childPlacements){auto r=child.bounds;SetWindowPos(child.window,nullptr,MulDiv(r.left,dpi,state.savedDpi),MulDiv(r.top,dpi,state.savedDpi),MulDiv(r.right-r.left,dpi,state.savedDpi),MulDiv(r.bottom-r.top,dpi,state.savedDpi),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(child.window,child.visible?SW_SHOW:SW_HIDE);}
        state.childPlacements.clear();
    }
    EnableWindow(GetDlgItem(window,IDC_REFRESH),!state.fullscreen);
    state.reader->fullscreen(state.fullscreen);SetFocus(state.reader->reading()?state.reader->window():cover);InvalidateRect(window,nullptr,TRUE);
    // Decode image previews for the new viewport instead of stretching a thumbnail.
    auto format=fileFormat(state.current.extension().wstring());
    if(format&&std::string(format->mime).starts_with("image/")) {
        RECT bounds{};GetClientRect(cover,&bounds);
        {std::lock_guard lock(state.worker->mutex);++state.worker->version;state.worker->path=state.current;state.worker->width=bounds.right;state.worker->height=bounds.bottom;state.worker->pending=true;state.reader->imageLoading(state.worker->version);}
        state.worker->wake.notify_one();
    }
}
void rebuildGrid(HWND window,Browser& state) {
    auto grid=GetDlgItem(window,IDC_FILE_GRID);
    if(!state.tiles) return;
    state.rebuilding=true;
    // Keep identities from the previous grid, including across sorting/filtering.
    auto itemName=[&](int index) { wchar_t name[32768]{}; if(index>=0) ListView_GetItemText(grid,index,0,name,32768); return std::wstring(name); };
    const auto focused=itemName(ListView_GetNextItem(grid,-1,LVNI_FOCUSED));
    const auto anchor=itemName(ListView_GetSelectionMark(grid));
    POINT origin{}; ListView_GetOrigin(grid,&origin);
    SendMessageW(grid,WM_SETREDRAW,FALSE,0);
    ListView_DeleteAllItems(grid);
    if(state.imageList) { ListView_SetImageList(grid,nullptr,LVSIL_NORMAL); ImageList_Destroy(state.imageList); }
    const int width=MulDiv(80,GetDpiForWindow(window),96),height=MulDiv(104,GetDpiForWindow(window),96);
    state.imageList=ImageList_Create(width,height,ILC_COLOR32|ILC_MASK,16,16);
    std::map<std::wstring,int> icons;
    for(auto [extension,icon]:state.icons) icons[extension]=ImageList_AddIcon(state.imageList,icon);
    std::unique_lock lock(state.thumbnails->mutex);
    for(size_t i=0;i<state.files.size();++i) {
        const auto& path=state.files[i]; auto format=fileFormat(path.extension().wstring());
        int image=format && icons.contains(format->extension) ? icons[format->extension] : 0;
        if(auto found=state.thumbnails->images.find(path); found!=state.thumbnails->images.end()) {
            auto dc=GetDC(grid); auto memory=CreateCompatibleDC(dc); auto bitmap=CreateCompatibleBitmap(dc,width,height);
            auto previous=SelectObject(memory,bitmap); RECT bounds{0,0,width,height};
            auto brush=CreateSolidBrush(usesDarkTheme(window) ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW)); FillRect(memory,&bounds,brush); DeleteObject(brush);
            drawBookCover(memory,bounds,found->second,path.extension().wstring());
            SelectObject(memory,previous); image=ImageList_Add(state.imageList,bitmap,nullptr);
            DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(grid,dc);
        }
        auto name=path.filename().wstring(); LVITEMW item{}; item.mask=LVIF_TEXT|LVIF_IMAGE; item.iItem=static_cast<int>(i); item.pszText=name.data(); item.iImage=image;
        ListView_InsertItem(grid,&item);
        if(name==focused) ListView_SetItemState(grid,static_cast<int>(i),LVIS_FOCUSED,LVIS_FOCUSED);
        if(name==anchor) ListView_SetSelectionMark(grid,static_cast<int>(i));
        if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,i,0)>0) ListView_SetItemState(grid,static_cast<int>(i),LVIS_SELECTED,LVIS_SELECTED);
    }
    ListView_SetImageList(grid,state.imageList,LVSIL_NORMAL);
    ListView_SetIconSpacing(grid,MulDiv(142,GetDpiForWindow(window),96),height+MulDiv(44,GetDpiForWindow(window),96));
    ListView_Scroll(grid,0,origin.y);
    lock.unlock();
    SendMessageW(grid,WM_SETREDRAW,TRUE,0); InvalidateRect(grid,nullptr,TRUE);
    state.rebuilding=false;
}
void setView(HWND window,Browser& state,bool tiles) {
    state.tiles=tiles;
    ShowWindow(GetDlgItem(window,IDC_FILE_GRID),tiles ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(window,IDC_FILE_LIST),tiles ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(window,IDC_BOOK_SCROLL),SW_SHOW);
    useListScrollbar(GetDlgItem(window,tiles?IDC_FILE_GRID:IDC_FILE_LIST),GetDlgItem(window,IDC_BOOK_SCROLL));
    SendDlgItemMessageW(window,IDC_VIEW_LIST,BM_SETCHECK,tiles ? BST_UNCHECKED : BST_CHECKED,0);
    SendDlgItemMessageW(window,IDC_VIEW_THUMBS,BM_SETCHECK,tiles ? BST_CHECKED : BST_UNCHECKED,0);
    rebuildGrid(window,state);
}
void styleBrowser(HWND window) {
    RECT search{};GetWindowRect(GetDlgItem(window,IDC_SEARCH),&search);
    MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&search),2);
    const int inset=MulDiv(4,GetDpiForWindow(window),96),clearWidth=MulDiv(26,GetDpiForWindow(window),96);
    SetWindowPos(GetDlgItem(window,IDC_CLEAR_SEARCH),HWND_TOP,search.right-inset-clearWidth,search.top+inset,clearWidth,search.bottom-search.top-2*inset,SWP_NOACTIVATE);
    ShowWindow(GetDlgItem(window,IDC_CLEAR_SEARCH),GetWindowTextLengthW(GetDlgItem(window,IDC_SEARCH))?SW_SHOWNA:SW_HIDE);
    auto grid=GetDlgItem(window,IDC_FILE_GRID); bool dark=usesDarkTheme(window);
    SetWindowTheme(grid,dark ? L"DarkMode_Explorer" : L"Explorer",nullptr);
    ListView_SetBkColor(grid,dark ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(grid,CLR_NONE);
    ListView_SetTextColor(grid,dark ? RGB(240,240,240) : GetSysColor(COLOR_WINDOWTEXT));
    auto list=GetDlgItem(window,IDC_FILE_LIST);
    SetWindowLongPtrW(list,GWL_STYLE,GetWindowLongPtrW(list,GWL_STYLE)&~WS_BORDER);
    SetWindowPos(list,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    RECT pill{},sort{}; auto combo=GetDlgItem(window,IDC_SORT);GetWindowRect(GetDlgItem(window,IDC_VIEW_LIST),&pill);
    const int height=pill.bottom-pill.top;
    // The selection field excludes the combo's native border; measure that
    // overhead so the outer control, not just its text area, matches the buttons.
    SendMessageW(combo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),height);
    GetWindowRect(combo,&sort);
    SendMessageW(combo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),(std::max)(1,height-(static_cast<int>(sort.bottom-sort.top)-height)));
    SendMessageW(combo,CB_SETITEMHEIGHT,0,height);
    SendMessageW(combo,CB_SETMINVISIBLE,4,0);
    GetWindowRect(combo,&sort);
    MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&sort),2); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&pill),2);
    SetWindowPos(GetDlgItem(window,IDC_SORT),nullptr,sort.left,pill.top+(pill.bottom-pill.top-(sort.bottom-sort.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    InvalidateRect(window,nullptr,TRUE);
}
void paintPanels(HWND window) {
    PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint); FillRect(dc,&paint.rcPaint,dialogBackground(window));
    auto pen=CreatePen(PS_SOLID,MulDiv(1,GetDpiForWindow(window),96),usesDarkTheme(window) ? RGB(105,105,105) : GetSysColor(COLOR_3DSHADOW));
    auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,panelBackground(window));
    const int pad=MulDiv(5,GetDpiForWindow(window),96);
    for(auto id:{IDC_FILE_GRID,IDC_COVER,IDC_FILENAME}) {
        RECT r{}; GetWindowRect(GetDlgItem(window,id),&r); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
        if(id==IDC_FILE_GRID){RECT bar{};GetWindowRect(GetDlgItem(window,IDC_BOOK_SCROLL),&bar);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bar),2);r.right=bar.right;}
        if(id==IDC_FILENAME) { RECT bottom{}; GetWindowRect(GetDlgItem(window,IDC_COPY_PATH),&bottom); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bottom),2); r.bottom=bottom.bottom+MulDiv(10,GetDpiForWindow(window),96); }
        InflateRect(&r,pad,pad); RoundRect(dc,r.left,r.top,r.right,r.bottom,pad*2,pad*2);
    }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); EndPaint(window,&paint);
}
void selectFile(HWND window,Browser& state,bool clearPreview=false) {
    int count=static_cast<int>(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELCOUNT,0,0));
    std::wstring button=state.previewOnly ? (count<=1 ? L"Test sending" : L"Test sending "+std::to_wstring(count)+L" books") : count<=0 ? L"Send books" : count==1 ? L"Send book" : L"Send "+std::to_wstring(count)+L" books";
    SetDlgItemTextW(window,IDOK,button.c_str()); EnableWindow(GetDlgItem(window,IDOK),count>0);
    int index=static_cast<int>(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETCARETINDEX,0,0));
    auto previous=state.current;
    state.current=!clearPreview && index>=0 && static_cast<size_t>(index)<state.files.size() ? state.files[index] : fs::path{};
    if(state.reader&&previous!=state.current)state.reader->select(state.current);
    for(auto id:{IDC_OPEN_FILE,IDC_OPEN_FOLDER,IDC_COPY_PATH}) EnableWindow(GetDlgItem(window,id),!state.current.empty());
    RECT box{}; GetClientRect(GetDlgItem(window,IDC_COVER),&box);
    { std::lock_guard lock(state.worker->mutex);
      ++state.worker->version; state.worker->result.reset(); state.worker->path=state.current;
      state.worker->width=box.right; state.worker->height=box.bottom; state.worker->pending=!state.current.empty(); }
    state.worker->wake.notify_one();
    auto accessible=state.current.empty() ? L"Select a file to preview." : bookInformationText(state.current,{});
    SetDlgItemTextW(window,IDC_FILENAME,accessible.c_str());
    InvalidateRect(GetDlgItem(window,IDC_COVER),nullptr,TRUE); InvalidateRect(GetDlgItem(window,IDC_FILENAME),nullptr,TRUE);
}
void applyFilters(HWND window,Browser& state,bool selectFirst=false,bool refreshing=false) {
    std::set<fs::path> selected;
    for(size_t i=0;i<state.files.size();++i) if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,i,0)>0) selected.insert(state.files[i]);
    auto previous=state.current; state.files.clear();
    wchar_t query[512]{}; GetDlgItemTextW(window,IDC_SEARCH,query,512);
    auto lowercase=[](std::wstring s) { std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));}); return s; };
    auto search=lowercase(query);
    for(const auto& file:state.allFiles) {
        auto format=fileFormat(file.extension().wstring());
        if(format && (state.allTypes || state.enabled[groupOf(file)]) && lowercase(file.filename().wstring()).find(search)!=std::wstring::npos) state.files.push_back(file);
    }
    auto sort=SendDlgItemMessageW(window,IDC_SORT,CB_GETCURSEL,0,0);
    std::sort(state.files.begin(),state.files.end(),[&](const auto& a,const auto& b) {
        if((sort==2 || sort==3) && state.modified[a]!=state.modified[b]) return sort==2 ? state.modified[a]>state.modified[b] : state.modified[a]<state.modified[b];
        return (sort==1 ? -1 : 1)*_wcsicmp(a.filename().c_str(),b.filename().c_str())<0;
    });
    SendDlgItemMessageW(window,IDC_FILE_LIST,WM_SETREDRAW,FALSE,0);
    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_RESETCONTENT,0,0);
    for(size_t i=0;i<state.files.size();++i) {
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(state.files[i].filename().c_str()));
        if(selected.contains(state.files[i]) || (selectFirst && i==0)) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,i);
    }
    // LB_SETSEL also moves the caret: restore the preview only after all selections.
    auto caret=std::find(state.files.begin(),state.files.end(),previous);
    if(caret!=state.files.end()) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,std::distance(state.files.begin(),caret),FALSE);
    SendDlgItemMessageW(window,IDC_FILE_LIST,WM_SETREDRAW,TRUE,0); InvalidateRect(GetDlgItem(window,IDC_FILE_LIST),nullptr,TRUE);
    RECT position{68,64,104,82}; MapDialogRect(window,&position);
    int x=position.left;
    for(size_t i=0;i<state.enabled.size();++i) {
        auto control=GetDlgItem(window,IDC_TYPE_FIRST+static_cast<int>(i));
        bool present=std::any_of(state.allFiles.begin(),state.allFiles.end(),[&](const auto& file){return groupOf(file)==static_cast<int>(i);});
        ShowWindow(control,present ? SW_SHOW : SW_HIDE);
        if(present) { SetWindowPos(control,nullptr,x,position.top,position.right-position.left,position.bottom-position.top,SWP_NOZORDER|SWP_NOACTIVATE); x+=(position.right-position.left)+MulDiv(6,GetDpiForWindow(window),96); }
        SendMessageW(control,BM_SETCHECK,!state.allTypes && state.enabled[i] ? BST_CHECKED : BST_UNCHECKED,0);
    }
    SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_SETCHECK,state.allTypes ? BST_CHECKED : BST_UNCHECKED,0);
    const bool none=!state.allTypes && std::none_of(state.enabled.begin(),state.enabled.end(),[](bool on){return on;});
    SetDlgItemTextW(window,IDC_BROWSER_HELP,none ? L"Choose a file type above, or All types." : state.files.empty() ? L"No matching files. Change the search or file types." : L"Hold Ctrl or Shift to select several files.");
    auto count=std::to_wstring(state.files.size())+(state.files.size()==1 ? L" file" : L" files");
    SetDlgItemTextW(window,IDC_FILE_COUNT,count.c_str());
    rebuildGrid(window,state);
    updateListScrollbar(GetDlgItem(window,IDC_FILE_LIST),GetDlgItem(window,IDC_BOOK_SCROLL));
    selectFile(window,state,refreshing && std::find(state.files.begin(),state.files.end(),previous)==state.files.end());
}
void populateFolder(HWND window,Browser& state,const fs::path& folder,bool refreshing=false) {
    std::vector<fs::path> files; std::error_code error;
    for(fs::directory_iterator it(folder,error),end; !error && it!=end; it.increment(error)) {
        if(it->is_regular_file(error) && fileFormat(it->path().extension().wstring())) files.push_back(it->path());
    }
    if(error) throw std::runtime_error("Could not read this folder.");
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return _wcsicmp(a.filename().c_str(),b.filename().c_str())<0;});
    state.folder=folder;
    state.allFiles=std::move(files);
    if(!refreshing) { state.allTypes=true; state.enabled.fill(false); }
    { std::lock_guard lock(state.thumbnails->mutex); state.thumbnails->stop=true; } state.thumbnails->wake.notify_one();
    state.thumbnails=std::make_shared<Thumbnails>();
    const auto oldSize=state.sizes.find(state.current);
    const auto oldModified=state.modified.find(state.current);
    const auto previousSize=oldSize==state.sizes.end() ? 0 : oldSize->second;
    const auto previousModified=oldModified==state.modified.end() ? 0 : oldModified->second;
    state.sizes.clear();
    state.modified.clear();
    for(const auto& file:state.allFiles) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&attributes)) state.modified[file]=(static_cast<unsigned long long>(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime;
    }
    for(const auto& file:state.allFiles) { std::error_code sizeError; auto size=fs::file_size(file,sizeError); if(!sizeError) state.sizes[file]=size; }
    for(const auto& format:FileFormats) if(!state.icons.contains(format.extension)) {
        state.icons[format.extension]=loadFileTypeIcon(std::wstring(L"file")+format.extension);
    }
    SetDlgItemTextW(window,IDC_FOLDER_PATH,folder.c_str());
    if(refreshing && state.reader && state.sizes.contains(state.current) && state.modified.contains(state.current) && (state.sizes.at(state.current)!=previousSize || state.modified.at(state.current)!=previousModified)) {
        state.reader->select(state.current);
    }
    applyFilters(window,state,!refreshing,refreshing);
    auto thumbnails=state.thumbnails;
    std::thread([thumbnails,window] {
        auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        for(;;) {
            std::unique_lock lock(thumbnails->mutex);
            thumbnails->wake.wait(lock,[&]{return thumbnails->stop || !thumbnails->requests.empty();});
            if(thumbnails->stop) break;
            auto path=thumbnails->requests.front(); thumbnails->requests.pop_front(); lock.unlock();
            auto image=loadCover(path,96,120);
            lock.lock();
            if(thumbnails->stop) { if(image) DeleteObject(image); break; }
            while(thumbnails->images.size()>=256) { auto old=thumbnails->order.front(); thumbnails->order.pop_front(); if(thumbnails->images[old]) DeleteObject(thumbnails->images[old]); thumbnails->images.erase(old); }
            thumbnails->images[path]=image; thumbnails->order.push_back(path); thumbnails->pending.erase(path);
            PostMessageW(window,ThumbnailsReady,0,0);
        }
        if(SUCCEEDED(com)) CoUninitialize();
    }).detach();
}
void refreshFolder(HWND window,Browser& state) {
    if(!state.fullscreen && !state.folder.empty()) populateFolder(window,state,state.folder,true);
}
void styleRefreshTooltip(HWND window,Browser& state) {
    const bool dark=usesDarkTheme(window);
    SendMessageW(state.tooltip,TTM_SETTIPBKCOLOR,dark?RGB(48,48,48):RGB(244,244,244),0);
    SendMessageW(state.tooltip,TTM_SETTIPTEXTCOLOR,dark?RGB(245,245,245):RGB(35,35,35),0);
}
fs::path firstExistingFolder(const fs::path& remembered,const fs::path& downloads,const fs::path& documents) {
    for(const auto& folder:{remembered,downloads,documents}) {
        std::error_code error;
        if(folder.empty() || !fs::is_directory(folder,error) || error) continue;
        // Do not treat access denied as an empty, usable folder.
        fs::directory_iterator probe(folder,error);
        if(!error) return folder;
    }
    return {};
}
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw=nullptr;
    if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&raw))) return {};
    fs::path folder(raw); CoTaskMemFree(raw); return folder;
}
fs::path defaultBrowseFolder() {
    return firstExistingFolder(loadBrowseFolder(),knownFolder(FOLDERID_Downloads),knownFolder(FOLDERID_Documents));
}
fs::path pickFolder(HWND window) {
    IFileDialog* dialog=nullptr;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) throw std::runtime_error("Could not open the folder picker.");
    DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose a folder of books"); auto hr=dialog->Show(window); IShellItem* item=nullptr;
    if(SUCCEEDED(hr)) hr=dialog->GetResult(&item); dialog->Release();
    if(!item) return {};
    PWSTR raw=nullptr; hr=item->GetDisplayName(SIGDN_FILESYSPATH,&raw); item->Release();
    if(FAILED(hr)) return {};
    fs::path folder(raw); CoTaskMemFree(raw);
    return folder;
}
void chooseFolder(HWND window,Browser& state) {
    auto folder=pickFolder(window);
    if(!folder.empty()) { populateFolder(window,state,folder); saveBrowseFolder(folder); }
}
void sizeRows(HWND window) {
    auto list=GetDlgItem(window,IDC_FILE_LIST); auto dc=GetDC(list);
    auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(list,WM_GETFONT,0,0)));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SelectObject(dc,old); ReleaseDC(list,dc);
    SendMessageW(list,LB_SETITEMHEIGHT,0,metrics.tmHeight*2+MulDiv(14,GetDpiForWindow(window),96));
    updateListScrollbar(list,GetDlgItem(window,IDC_BOOK_SCROLL));
}
void drawFile(HWND window,const DRAWITEMSTRUCT& draw,Browser& state) {
    const bool dark=usesDarkTheme(window),selected=(draw.itemState&ODS_SELECTED)!=0;
    auto background=CreateSolidBrush(selected ? GetSysColor(COLOR_HIGHLIGHT) : dark ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW));
    FillRect(draw.hDC,&draw.rcItem,background); DeleteObject(background);
    if(draw.itemID>=state.files.size()) return;
    int saved=SaveDC(draw.hDC); const auto& path=state.files[draw.itemID];
    SelectObject(draw.hDC,reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem,WM_GETFONT,0,0)));
    const int pad=MulDiv(8,GetDpiForWindow(window),96); RECT text=draw.rcItem;
    text.left+=pad; text.right-=pad; text.top+=pad/2;
    if(auto format=fileFormat(path.extension().wstring()); format && state.icons.contains(format->extension)) {
        const int size=MulDiv(24,GetDpiForWindow(window),96);
        DrawIconEx(draw.hDC,text.left,draw.rcItem.top+(draw.rcItem.bottom-draw.rcItem.top-size)/2,state.icons.at(format->extension),size,size,0,nullptr,DI_NORMAL);
        text.left+=size+pad;
    }
    TEXTMETRICW metrics{}; GetTextMetricsW(draw.hDC,&metrics);
    SetBkMode(draw.hDC,TRANSPARENT);
    SetTextColor(draw.hDC,selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : dark ? RGB(240,240,240) : GetSysColor(COLOR_WINDOWTEXT));
    auto name=path.filename().wstring(); text.bottom=text.top+metrics.tmHeight;
    DrawTextW(draw.hDC,name.c_str(),-1,&text,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    auto extension=path.extension().wstring().substr(1); std::transform(extension.begin(),extension.end(),extension.begin(),towupper);
    auto detail=extension; if(state.sizes.contains(path)) detail+=L"  \u00b7  "+readableFileSize(state.sizes.at(path));
    text.top=text.bottom+pad/3; text.bottom=draw.rcItem.bottom;
    SetTextColor(draw.hDC,selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : dark ? RGB(175,175,175) : GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(draw.hDC,detail.c_str(),-1,&text,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(draw.itemState&ODS_FOCUS)drawKeyboardFocus(window,draw.hDC,draw.rcItem,(draw.itemState&ODS_SELECTED)!=0);
    RestoreDC(draw.hDC,saved);
}
void copyPath(HWND window,const fs::path& path) {
    auto text=path.wstring(); auto bytes=(text.size()+1)*sizeof(wchar_t);
    auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes); if(!memory) return;
    auto buffer=GlobalLock(memory); if(!buffer) { GlobalFree(memory); return; }
    memcpy(buffer,text.c_str(),bytes); GlobalUnlock(memory);
    if(OpenClipboard(window)) { EmptyClipboard(); if(SetClipboardData(CF_UNICODETEXT,memory)) memory=nullptr; CloseClipboard(); }
    if(memory) GlobalFree(memory);
}

INT_PTR CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_MEASUREITEM && wp==IDC_FILE_LIST) { reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=48; return TRUE; }
        if(message==WM_INITDIALOG) {
            state=reinterpret_cast<Browser*>(lp); SetWindowLongPtrW(window,DWLP_USER,lp); applyTheme(window);
            state->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            SetWindowTheme(state->tooltip,L"",L"");
            TOOLINFOW refreshTool{sizeof(refreshTool)}; refreshTool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
            refreshTool.hwnd=window; refreshTool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,IDC_REFRESH));
            refreshTool.lpszText=const_cast<LPWSTR>(L"Refresh (F5)");
            SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&refreshTool));
            styleRefreshTooltip(window,*state);
            state->reader=std::make_unique<Reader>(window,GetDlgItem(window,IDC_COVER),[window,state]{toggleFullscreen(window,*state);});
            if(state->previewOnly) SetWindowTextW(window,L"Voltura Books - Browse books (no emails sent)");
            SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_BOOK))));
            EnableWindow(GetDlgItem(window,IDOK),FALSE);
            attachListScrollbar(GetDlgItem(window,IDC_FILE_LIST),GetDlgItem(window,IDC_BOOK_SCROLL));
            sizeRows(window);
            setView(window,*state,false);
            PostMessageW(window,WM_APP+22,0,0);
            SendDlgItemMessageW(window,IDC_SEARCH,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search files..."));
            for(auto name:{L"Name (A-Z)",L"Name (Z-A)",L"Modified (newest)",L"Modified (oldest)"}) SendDlgItemMessageW(window,IDC_SORT,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,0,0);
            applyFilters(window,*state);
            auto worker=state->worker;
            std::thread([worker,window] {
                auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                for(;;) {
                    std::unique_lock lock(worker->mutex); worker->wake.wait(lock,[&]{return worker->stop||worker->pending;});
                    if(worker->stop) break;
                    auto path=worker->path; auto version=worker->version; int width=worker->width,height=worker->height; worker->pending=false; lock.unlock();
                    auto result=std::make_shared<Preview>();
                    if(SUCCEEDED(com)) { result->cover=loadCover(path,width,height); result->details=loadBookDetails(path); }
                    lock.lock(); if(!worker->stop && worker->version==version) {
                        // Selection changes clear result; a same-selection resize
                        // must not replace a valid preview with a failed decode.
                        if(result->cover||!worker->result||!worker->result->cover)worker->result=result;
                        PostMessageW(window,PreviewReady,static_cast<WPARAM>(version),0);
                    }
                }
                if(SUCCEEDED(com)) CoUninitialize();
            }).detach();
            if(!state->initialFolder.empty()) populateFolder(window,*state,state->initialFolder);
            return TRUE;
        }
        if(!state) return FALSE;
        if((message==DialogShortcut&&wp==VK_F5)||(message==WM_COMMAND&&LOWORD(wp)==IDC_REFRESH)) {
            refreshFolder(window,*state); SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE); return TRUE;
        }
        if(message==DialogShortcut&&wp==VK_F11&&state->reader&&state->reader->canFullscreen()){toggleFullscreen(window,*state);SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE);return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)==IDC_FULLSCREEN_READER&&state->reader){state->reader->openFullscreen();return TRUE;}
        if(message==WM_DESTROY){DestroyWindow(state->tooltip);state->tooltip=nullptr;state->reader.reset();return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)>=IDC_READER_PREVIOUS&&LOWORD(wp)<=IDC_READER_FULLSCREEN&&state->reader){SendMessageW(state->reader->window(),WM_COMMAND,wp,lp);return TRUE;}
        if(message==WM_DRAWITEM&&wp>=IDC_READER_PREVIOUS&&wp<=IDC_READER_FULLSCREEN&&state->reader){SendMessageW(state->reader->window(),message,wp,lp);return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL&&state->fullscreen){toggleFullscreen(window,*state);return TRUE;}
        if(message==WM_SIZE&&state->fullscreen&&state->reader){RECT r{};GetClientRect(window,&r);SetWindowPos(GetDlgItem(window,IDC_COVER),nullptr,0,0,r.right,r.bottom,SWP_NOZORDER|SWP_NOACTIVATE);state->reader->resize();return TRUE;}
        if(message==WM_PAINT&&state->fullscreen){PAINTSTRUCT p{};auto dc=BeginPaint(window,&p);FillRect(dc,&p.rcPaint,panelBackground(window));EndPaint(window,&p);return TRUE;}
        if(message==WM_PAINT) { paintPanels(window); return TRUE; }
        if(message==WM_APP+22 || message==WM_SETTINGCHANGE || message==WM_THEMECHANGED) { styleBrowser(window); styleRefreshTooltip(window,*state); return TRUE; }
        if(message==WM_COMMAND && (LOWORD(wp)==IDC_VIEW_LIST || LOWORD(wp)==IDC_VIEW_THUMBS)) { setView(window,*state,LOWORD(wp)==IDC_VIEW_THUMBS); return TRUE; }
        if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->idFrom==IDC_FILE_GRID) {
            auto header=reinterpret_cast<NMHDR*>(lp);
            if(header->code==NM_CUSTOMDRAW && !state->rebuilding) {
                auto draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if(draw->nmcd.dwDrawStage==CDDS_PREPAINT) { SetWindowLongPtrW(window,DWLP_MSGRESULT,CDRF_NOTIFYITEMDRAW); return TRUE; }
                if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec<state->files.size()) {
                    const auto& path=state->files[draw->nmcd.dwItemSpec];
                    { std::lock_guard lock(state->thumbnails->mutex);
                      if(!state->thumbnails->images.contains(path) && state->thumbnails->pending.insert(path).second) state->thumbnails->requests.push_back(path); }
                    state->thumbnails->wake.notify_one();
                }
            }
            if(header->code==LVN_ITEMCHANGED && !state->rebuilding) {
                auto grid=GetDlgItem(window,IDC_FILE_GRID);
                SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);
                for(int i=-1;(i=ListView_GetNextItem(grid,i,LVNI_SELECTED))!=-1;) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,i);
                int focus=ListView_GetNextItem(grid,-1,LVNI_FOCUSED);
                if(focus>=0) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,focus,FALSE);
                selectFile(window,*state); return TRUE;
            }
        }
        if(message==ThumbnailsReady) { if(state->tiles) SetTimer(window,3,150,nullptr); return TRUE; }
        if(message==WM_TIMER && wp==3) { KillTimer(window,3); rebuildGrid(window,*state); return TRUE; }
        if(message==WM_DPICHANGED) { if(state->fullscreen){MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);SetWindowPos(window,nullptr,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOZORDER|SWP_NOACTIVATE);}else{sizeRows(window);styleBrowser(window);rebuildGrid(window,*state);}if(state->reader)state->reader->resize();return TRUE; }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_CLEAR_SEARCH) { SetFocus(GetDlgItem(window,IDC_SEARCH));SetDlgItemTextW(window,IDC_SEARCH,L"");return TRUE; }
        if(message==WM_DRAWITEM && wp==IDC_CLEAR_SEARCH) {
            auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp);const bool dark=usesDarkTheme(window);
            auto brush=CreateSolidBrush(dark?RGB(45,45,45):GetSysColor(COLOR_WINDOW));FillRect(draw->hDC,&draw->rcItem,brush);DeleteObject(brush);
            const int saved=SaveDC(draw->hDC);
            auto font=CreateFontW(-MulDiv(14,GetDpiForWindow(window),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons");
            SelectObject(draw->hDC,font);SetBkMode(draw->hDC,TRANSPARENT);SetTextColor(draw->hDC,dark?RGB(240,240,240):GetSysColor(COLOR_WINDOWTEXT));
            DrawTextW(draw->hDC,L"\xE711",1,&draw->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(draw->itemState&ODS_FOCUS)drawKeyboardFocus(draw->hwndItem,draw->hDC,draw->rcItem);
            RestoreDC(draw->hDC,saved);DeleteObject(font);return TRUE;
        }
        if(message==WM_COMMAND && ((LOWORD(wp)==IDC_SEARCH && HIWORD(wp)==EN_CHANGE) || (LOWORD(wp)==IDC_SORT && HIWORD(wp)==CBN_SELCHANGE))) {
            ShowWindow(GetDlgItem(window,IDC_CLEAR_SEARCH),GetWindowTextLengthW(GetDlgItem(window,IDC_SEARCH))?SW_SHOWNA:SW_HIDE);
            applyFilters(window,*state); return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_COPY_PATH && !state->current.empty()) { copyPath(window,state->current); return TRUE; }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_OPEN_FILE && !state->current.empty()) {
            auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",state->current.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
            if(result<=32) throw std::runtime_error("Windows could not open this file. Check its default app.");
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_OPEN_FOLDER && !state->current.empty()) {
            auto item=ILCreateFromPathW(state->current.c_str());
            if(item) { SHOpenFolderAndSelectItems(item,0,nullptr,0); ILFree(item); } return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)>=IDC_TYPE_FIRST && LOWORD(wp)<IDC_TYPE_FIRST+state->enabled.size()) {
            auto index=LOWORD(wp)-IDC_TYPE_FIRST; state->enabled[index]=SendDlgItemMessageW(window,LOWORD(wp),BM_GETCHECK,0,0)==BST_CHECKED;
            if(!std::any_of(state->enabled.begin(),state->enabled.end(),[](bool on){return on;})) { state->allTypes=true; state->enabled.fill(false); }
            else state->allTypes=false;
            applyFilters(window,*state); return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_ALL_TYPES) { state->allTypes=true; state->enabled.fill(false); applyFilters(window,*state); return TRUE; }
        if(message==WM_VSCROLL && reinterpret_cast<HWND>(lp)==GetDlgItem(window,IDC_BOOK_SCROLL)) {
            SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(reinterpret_cast<HWND>(lp),SB_CTL,&info);
            int position=info.nPos;
            switch(LOWORD(wp)) { case SB_LINEUP: --position; break; case SB_LINEDOWN: ++position; break; case SB_PAGEUP: position-=info.nPage; break; case SB_PAGEDOWN: position+=info.nPage; break; case SB_THUMBTRACK: case SB_THUMBPOSITION: position=info.nTrackPos; break; }
            SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETTOPINDEX,(std::max)(0,position),0); return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_FOLDER) { chooseFolder(window,*state); return TRUE; }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_FILE_LIST && HIWORD(wp)==LBN_SELCHANGE) { selectFile(window,*state); return TRUE; }
        if(message==PreviewReady) {
            {std::lock_guard lock(state->worker->mutex);if(wp!=state->worker->version)return TRUE;}
            std::shared_ptr<Preview> preview; { std::lock_guard lock(state->worker->mutex); preview=state->worker->result; }
            if(preview) SetDlgItemTextW(window,IDC_FILENAME,bookInformationText(state->current,preview->details).c_str());
            if(preview&&state->reader)state->reader->previewReady(preview->details.pages,preview->cover!=nullptr);
            if(state->reader)state->reader->imageLoaded(static_cast<uint64_t>(wp));
            InvalidateRect(GetDlgItem(window,IDC_COVER),nullptr,TRUE); InvalidateRect(GetDlgItem(window,IDC_FILENAME),nullptr,TRUE); return TRUE;
        }
        if(message==WM_DRAWITEM && wp==IDC_FILE_LIST) { drawFile(window,*reinterpret_cast<DRAWITEMSTRUCT*>(lp),*state); return TRUE; }
        if(message==WM_DRAWITEM && (wp==IDC_COVER || wp==IDC_FILENAME)) {
            auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp); FillRect(draw->hDC,&draw->rcItem,panelBackground(window));
            if(state->current.empty()) return TRUE;
            std::shared_ptr<Preview> preview; { std::lock_guard lock(state->worker->mutex); preview=state->worker->result; }
            if(wp==IDC_COVER) drawBookCover(draw->hDC,draw->rcItem,preview ? preview->cover : nullptr,state->current.extension().wstring());
            else {
                SendMessageW(window,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(draw->hDC),reinterpret_cast<LPARAM>(draw->hwndItem));
                auto format=fileFormat(state->current.extension().wstring());
                auto icon=format && state->icons.contains(format->extension) ? state->icons.at(format->extension) : nullptr;
                drawBookInformation(draw->hDC,draw->rcItem,reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem,WM_GETFONT,0,0)),state->current,preview ? preview->details : BookDetails{},true,true,icon,true,true);
            }
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDOK) {
            int count=static_cast<int>(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELCOUNT,0,0));
            if(count<=0) return TRUE;
            std::vector<int> indices(count); SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELITEMS,count,reinterpret_cast<LPARAM>(indices.data()));
            for(auto index:indices) if(index>=0 && static_cast<size_t>(index)<state->files.size()) state->selected.push_back(state->files[index]);
            EndDialog(window,IDOK); return TRUE;
        }
        if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) { EndDialog(window,IDCANCEL); return TRUE; }
    } catch(const std::exception& e) { themedMessageBox(window,wide(e.what()).c_str(),L"Voltura Books",MB_OK|MB_ICONERROR); }
    return FALSE;
}
}
std::vector<fs::path> browseBooks(HWND owner,const fs::path& initialFolder,bool previewOnly) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    Browser state; state.initialFolder=initialFolder; state.previewOnly=previewOnly;
    if(state.initialFolder.empty()) state.initialFolder=defaultBrowseFolder();
    if(state.initialFolder.empty()) state.initialFolder=pickFolder(owner);
    if(state.initialFolder.empty()) return {};
    // Browse Books is the visible app surface while its modal loop runs. Keep
    // ownership for lifetime/modality, but give the browser its own shell entry.
    struct RestoreOwner {
        HWND window,focus;
        bool visible;
        ~RestoreOwner(){if(visible&&IsWindow(window)){ShowWindow(window,SW_SHOW);SetActiveWindow(window);if(IsWindow(focus))SetFocus(focus);}}
    } restore{owner,GetFocus(),owner&&IsWindowVisible(owner)};
    if(restore.visible)ShowWindow(owner,SW_HIDE);
    auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_BROWSER),owner,proc,reinterpret_cast<LPARAM>(&state));
    if(result==-1) throw std::runtime_error("Could not open the folder browser.");
    // The caller completes the hidden sending launcher on both Send and Close.
    // Restore it only on failure, when the caller needs to report an error.
    restore.visible=false;
    return result==IDOK ? state.selected : std::vector<fs::path>{};
}
}
