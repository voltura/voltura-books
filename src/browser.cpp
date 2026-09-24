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
#include <chrono>
#include <uxtheme.h>
namespace books {
namespace {
constexpr UINT PreviewReady=WM_APP+20;
constexpr UINT ThumbnailsReady=WM_APP+21;
constexpr UINT BrowseFolderRequest=WM_APP+23;
constexpr UINT SubfolderScanReady=WM_APP+24;
constexpr UINT GridSelectionChanged=WM_APP+25;
constexpr int MaximumSelection=100;
constexpr unsigned AnchorRight=1,AnchorBottom=2,GrowWidth=4,GrowHeight=8;
struct LayoutControl { int id; unsigned flags; };
constexpr std::array<LayoutControl,18> LayoutControls{{
    {IDC_REFRESH,AnchorRight},{IDC_FOLDER,AnchorRight},{IDC_FILENAME,AnchorRight},
    {IDC_FULLSCREEN_READER,AnchorRight|AnchorBottom},{IDC_DOWNLOAD_VIEW,AnchorRight|AnchorBottom},
    {IDC_OPEN_FILE,AnchorRight|AnchorBottom},{IDC_OPEN_FOLDER,AnchorRight|AnchorBottom},
    {IDC_COPY_PATH,AnchorRight|AnchorBottom},{IDC_SCAN_STATUS,AnchorRight|AnchorBottom},
    {IDOK,AnchorRight|AnchorBottom},{IDCANCEL,AnchorRight|AnchorBottom},
    {IDC_FILE_COUNT,AnchorBottom},{IDC_BROWSER_HELP,AnchorBottom},
    {IDC_FILE_LIST,GrowHeight},{IDC_FILE_GRID,GrowHeight},{IDC_BOOK_SCROLL,GrowHeight},
    {IDC_PANEL_DIVIDER,GrowHeight},
    {IDC_COVER,GrowWidth|GrowHeight}
}};
HWND activeBrowser=nullptr;
constexpr const wchar_t* GroupNames[]={L"EPUB",L"PDF",L"RTF",L"TXT",L"HTML",L"Word",L"Images"};
int groupOf(const fs::path& file) {
    auto format=fileFormat(file.extension().wstring());
    if(!format) return -1;
    auto index=format-FileFormats;
    return index<4 ? static_cast<int>(index) : index<6 ? 4 : index<8 ? 5 : 6;
}
bool cloudPlaceholder(const fs::path& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&
        (attributes&(FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_RECALL_ON_OPEN|FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
}
struct Thumbnails {
    std::mutex mutex; std::condition_variable wake; bool stop=false;
    std::deque<fs::path> requests,order; std::set<fs::path> pending;
    std::map<fs::path,HBITMAP> images;
    ~Thumbnails(){for(auto [path,image]:images) if(image) DeleteObject(image);}
};
struct Preview {
    HBITMAP cover=nullptr; BookDetails details;
    bool downloadRequested=false,downloaded=false;
    ~Preview() { if(cover) DeleteObject(cover); }
};
struct Worker {
    std::mutex mutex; std::condition_variable wake;
    bool stop=false,pending=false,hydrate=false; size_t version=0;
    fs::path path; int width=0,height=0;
    std::shared_ptr<Preview> result;
};
struct ScannedFile {
    fs::path path;
    unsigned long long size=0,modified=0;
    bool hasSize=false,hasModified=false;
};
struct ScanUpdate {
    size_t generation=0;
    std::vector<ScannedFile> files;
    bool complete=false;
};
struct ScanWorker {
    std::mutex mutex;
    bool stop=false;
    size_t generation=0;
    HWND window=nullptr;
    std::deque<ScanUpdate> updates;
};
struct Browser {
    std::unique_ptr<Reader> reader;
    bool fullscreen=false;
    HWND tooltip=nullptr;
    HWND menu=nullptr;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowStyle=0,windowExStyle=0;
    UINT savedDpi=96;
    SIZE initialClientSize{};
    UINT layoutDpi=96;
    std::array<RECT,LayoutControls.size()> initialBounds{};
    bool layoutReady=false;
    bool resizing=false;
    bool draggingDivider=false;
    int dividerExtraWidth=0,dragStartX=0,dragStartWidth=0;
    struct ChildPlacement { HWND window; RECT bounds; bool visible; };
    std::vector<ChildPlacement> childPlacements;
    std::vector<fs::path> files,allFiles,directFiles,gridFiles,selected;
    std::array<bool,std::size(GroupNames)> enabled{};
    bool allTypes=true,tiles=false,rebuilding=false;
    bool gridSelectionPending=false,suppressMenuClick=false;
    bool includeSubfolders=false,scanActive=false;
    bool hydrating=false;
    size_t scanGeneration=0,subfolderFilesFound=0;
    bool previewOnly=false;
    bool rememberInitialFolder=false;
    BrowserMenuActions menuActions;
    HIMAGELIST imageList=nullptr;
    std::shared_ptr<Thumbnails> thumbnails=std::make_shared<Thumbnails>();
    std::map<fs::path,unsigned long long> sizes;
    std::map<fs::path,unsigned long long> modified;
    std::map<std::wstring,HICON> icons;
    std::set<fs::path> hydratedFiles;
    Browser() = default;
    fs::path current,initialFolder,folder;
    std::shared_ptr<Worker> worker=std::make_shared<Worker>();
    std::shared_ptr<ScanWorker> scanWorker=std::make_shared<ScanWorker>();
    ~Browser() {
        { std::lock_guard lock(thumbnails->mutex); thumbnails->stop=true; } thumbnails->wake.notify_one();
        if(imageList) ImageList_Destroy(imageList);
        { std::lock_guard lock(worker->mutex); worker->stop=true; } worker->wake.notify_one();
        { std::lock_guard lock(scanWorker->mutex); scanWorker->stop=true; ++scanWorker->generation; scanWorker->window=nullptr; scanWorker->updates.clear(); }
        for(auto [extension,icon]:icons) if(icon) DestroyIcon(icon);
    }
};
bool currentWorkerRequest(const std::shared_ptr<Worker>& worker,size_t version) {
    std::lock_guard lock(worker->mutex);
    return !worker->stop&&worker->version==version;
}
bool hydrateFile(const fs::path& path,const std::shared_ptr<Worker>& worker,size_t version) {
    auto file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    std::array<unsigned char,64*1024> buffer{};
    bool completed=false;
    for(;;) {
        if(!currentWorkerRequest(worker,version))break;
        DWORD read=0;
        if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr))break;
        if(!read){completed=true;break;}
    }
    CloseHandle(file);
    return completed;
}
bool needsDownload(const Browser& state,const fs::path& path) {
    return !path.empty()&&!state.hydratedFiles.contains(path)&&cloudPlaceholder(path);
}
void updateDownloadAction(HWND window,const Browser& state) {
    const bool download=state.hydrating||needsDownload(state,state.current);
    ShowWindow(GetDlgItem(window,IDC_DOWNLOAD_VIEW),download?SW_SHOW:SW_HIDE);
    ShowWindow(GetDlgItem(window,IDC_FULLSCREEN_READER),download?SW_HIDE:SW_SHOW);
    SetDlgItemTextW(window,IDC_DOWNLOAD_VIEW,state.hydrating?L"Downloading\u2026":L"Download and view");
    EnableWindow(GetDlgItem(window,IDC_DOWNLOAD_VIEW),download&&!state.hydrating);
    if(state.hydrating)SetTimer(window,4,80,nullptr);else KillTimer(window,4);
}
constexpr wchar_t HotItemProperty[]=L"VolturaBooks.HotItem";
int hotItem(HWND list) {return static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(list,HotItemProperty)))-1;}
void updateHotItem(HWND list,bool clear=false) {
    const bool grid=GetDlgCtrlID(list)==IDC_FILE_GRID;
    int next=-1;POINT point{};GetCursorPos(&point);
    if(!clear&&IsWindowVisible(list)&&IsWindowEnabled(list)&&WindowFromPoint(point)==list) {
        ScreenToClient(list,&point);
        if(grid){LVHITTESTINFO hit{};hit.pt=point;next=ListView_HitTest(list,&hit);}
        else {auto hit=SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));RECT bounds{};
            if(!HIWORD(hit)&&SendMessageW(list,LB_GETITEMRECT,LOWORD(hit),reinterpret_cast<LPARAM>(&bounds))!=LB_ERR&&PtInRect(&bounds,point))next=LOWORD(hit);}
    }
    const int previous=hotItem(list);if(previous==next)return;
    if(next<0)RemovePropW(list,HotItemProperty);else SetPropW(list,HotItemProperty,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(next+1)));
    for(int item:{previous,next})if(item>=0){RECT r{};
        if(grid){if(ListView_GetItemRect(list,item,&r,LVIR_BOUNDS))InvalidateRect(list,&r,FALSE);}
        else if(SendMessageW(list,LB_GETITEMRECT,item,reinterpret_cast<LPARAM>(&r))!=LB_ERR)InvalidateRect(list,&r,FALSE);
    }
}
LRESULT CALLBACK hoverItemsProc(HWND list,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if(message==WM_KEYDOWN&&wp==L'A'&&(GetKeyState(VK_CONTROL)&0x8000)) {
        auto parent=GetParent(list);auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(parent,DWLP_USER));
        if(state)state->rebuilding=true;
        const int count=GetDlgCtrlID(list)==IDC_FILE_GRID ? ListView_GetItemCount(list) : static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
        if(GetDlgCtrlID(list)==IDC_FILE_GRID) {
            ListView_SetItemState(list,-1,0,LVIS_SELECTED);
            for(int i=0;i<(std::min)(count,MaximumSelection);++i)ListView_SetItemState(list,i,LVIS_SELECTED,LVIS_SELECTED);
        }
        auto selection=GetDlgItem(parent,IDC_FILE_LIST);SendMessageW(selection,LB_SETSEL,FALSE,-1);
        for(int i=0;i<(std::min)(count,MaximumSelection);++i)SendMessageW(selection,LB_SETSEL,TRUE,i);
        if(count>0)SendMessageW(selection,LB_SETCARETINDEX,0,FALSE);
        if(state)state->rebuilding=false;
        SendMessageW(parent,WM_COMMAND,MAKEWPARAM(IDC_FILE_LIST,LBN_SELCHANGE),reinterpret_cast<LPARAM>(selection));
        return 0;
    }
    if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,list,0};TrackMouseEvent(&track);updateHotItem(list);}
    if(message==WM_MOUSELEAVE||message==WM_CANCELMODE||message==WM_NCDESTROY||
       (message==WM_SHOWWINDOW&&!wp)||(message==WM_ENABLE&&!wp)||message==LB_RESETCONTENT||message==LVM_DELETEALLITEMS)updateHotItem(list,true);
    auto result=DefSubclassProc(list,message,wp,lp);
    if(message==WM_MOUSEWHEEL||message==WM_VSCROLL||message==WM_HSCROLL||message==WM_SIZE||message==WM_KEYDOWN||
       message==LB_SETTOPINDEX||message==LVM_SCROLL||message==LVM_ENSUREVISIBLE||message==WM_SETREDRAW||
       (message==WM_SHOWWINDOW&&wp)||(message==WM_ENABLE&&wp))updateHotItem(list);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(list,hoverItemsProc,1);
    return result;
}
void limitSelection(HWND window) {
    auto list=GetDlgItem(window,IDC_FILE_LIST);
    int count=static_cast<int>(SendMessageW(list,LB_GETSELCOUNT,0,0));
    if(count<=MaximumSelection)return;
    std::vector<int> selected(count);SendMessageW(list,LB_GETSELITEMS,count,reinterpret_cast<LPARAM>(selected.data()));
    const int caret=static_cast<int>(SendMessageW(list,LB_GETCARETINDEX,0,0));
    if(count==MaximumSelection+1&&caret>=0&&SendMessageW(list,LB_GETSEL,caret,0)>0)SendMessageW(list,LB_SETSEL,FALSE,caret);
    else for(size_t i=MaximumSelection;i<selected.size();++i)SendMessageW(list,LB_SETSEL,FALSE,selected[i]);
}
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
    state.reader->fullscreen(state.fullscreen);
    // Re-measure after restored child placement so the active WebView reflows
    // to the smaller browse preview viewport.
    state.reader->resize();
    SetFocus(state.reader->reading()?state.reader->window():cover);InvalidateRect(window,nullptr,TRUE);
    // Decode image previews for the new viewport instead of stretching a thumbnail.
    auto format=fileFormat(state.current.extension().wstring());
    if(format&&std::string(format->mime).starts_with("image/")) {
        RECT bounds{};GetClientRect(cover,&bounds);
        {std::lock_guard lock(state.worker->mutex);++state.worker->version;state.worker->path=state.current;state.worker->width=bounds.right;state.worker->height=bounds.bottom;state.worker->pending=true;state.reader->imageLoading(state.worker->version);}
        state.worker->wake.notify_one();
    }
}
std::wstring displayedPath(const Browser& state,const fs::path& path) {
    if(state.includeSubfolders&&!state.folder.empty()) {
        auto relative=path.lexically_relative(state.folder);
        if(!relative.empty()&&*relative.begin()!=L"..") return relative.wstring();
    }
    return path.filename().wstring();
}
std::wstring compactTilePath(const Browser& state,const fs::path& path,size_t maximum=36) {
    auto label=displayedPath(state,path);
    if(label.size()<=maximum)return label;
    const auto separator=label.find_first_of(L"\\/");
    if(separator==std::wstring::npos)return label;
    const auto filename=path.filename().wstring();
    auto compact=label.substr(0,separator)+L"\\...\\"+filename;
    if(compact.size()<=maximum)return compact;
    compact=L"...\\"+filename;
    if(compact.size()<=maximum)return compact;
    const auto extension=path.extension().wstring();
    const auto room=maximum>4+extension.size()+4 ? maximum-4-extension.size()-3 : 0;
    return room ? L"...\\"+filename.substr(0,room)+L"..."+extension : label;
}
void rebuildGrid(HWND window,Browser& state) {
    auto grid=GetDlgItem(window,IDC_FILE_GRID);
    if(!state.tiles) return;
    state.rebuilding=true;
    // Keep identities from the previous grid, including across sorting/filtering.
    auto itemPath=[&](int index) { return index>=0&&static_cast<size_t>(index)<state.gridFiles.size() ? state.gridFiles[index] : fs::path{}; };
    const auto focused=itemPath(ListView_GetNextItem(grid,-1,LVNI_FOCUSED));
    const auto anchor=itemPath(ListView_GetSelectionMark(grid));
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
        auto name=compactTilePath(state,path); LVITEMW item{}; item.mask=LVIF_TEXT|LVIF_IMAGE; item.iItem=static_cast<int>(i); item.pszText=name.data(); item.iImage=image;
        ListView_InsertItem(grid,&item);
        if(path==focused) ListView_SetItemState(grid,static_cast<int>(i),LVIS_FOCUSED,LVIS_FOCUSED);
        if(path==anchor) ListView_SetSelectionMark(grid,static_cast<int>(i));
        if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,i,0)>0) ListView_SetItemState(grid,static_cast<int>(i),LVIS_SELECTED,LVIS_SELECTED);
    }
    ListView_SetImageList(grid,state.imageList,LVSIL_NORMAL);
    ListView_SetIconSpacing(grid,MulDiv(142,GetDpiForWindow(window),96),height+MulDiv(44,GetDpiForWindow(window),96));
    ListView_Scroll(grid,0,origin.y);
    state.gridFiles=state.files;
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
void captureBrowserLayout(HWND window,Browser& state) {
    RECT client{}; GetClientRect(window,&client);
    state.initialClientSize={client.right,client.bottom};
    state.layoutDpi=GetDpiForWindow(window);
    for(size_t i=0;i<LayoutControls.size();++i) {
        auto& bounds=state.initialBounds[i];
        GetWindowRect(GetDlgItem(window,LayoutControls[i].id),&bounds);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
    }
    state.layoutReady=true;
}
int availableDividerWidth(HWND window,const Browser& state) {
    RECT client{}; GetClientRect(window,&client);
    return (std::max)(0,static_cast<int>(client.right)-MulDiv(state.initialClientSize.cx,GetDpiForWindow(window),state.layoutDpi));
}
int dividerOffset(HWND window,const Browser& state) {
    if(!IsZoomed(window))return 0;
    return (std::min)(availableDividerWidth(window,state),MulDiv(state.dividerExtraWidth,GetDpiForWindow(window),state.layoutDpi));
}
void layoutBrowser(HWND window,const Browser& state) {
    if(!state.layoutReady || state.fullscreen || IsIconic(window))return;
    RECT client{}; GetClientRect(window,&client);
    const auto dpi=GetDpiForWindow(window);
    const int extraWidth=client.right-MulDiv(state.initialClientSize.cx,dpi,state.layoutDpi);
    const int extraHeight=client.bottom-MulDiv(state.initialClientSize.cy,dpi,state.layoutDpi);
    const int split=dividerOffset(window,state);
    for(size_t i=0;i<LayoutControls.size();++i) {
        const auto& original=state.initialBounds[i];
        const auto [id,flags]=LayoutControls[i];
        const bool fileView=id==IDC_FILE_LIST||id==IDC_FILE_GRID;
        const bool moveWithSplit=id==IDC_BOOK_SCROLL||id==IDC_PANEL_DIVIDER||id==IDC_COVER;
        const int left=MulDiv(original.left,dpi,state.layoutDpi)+((flags&AnchorRight)?extraWidth:0)+(moveWithSplit?split:0);
        const int top=MulDiv(original.top,dpi,state.layoutDpi)+((flags&AnchorBottom)?extraHeight:0);
        const int width=MulDiv(original.right-original.left,dpi,state.layoutDpi)+((flags&GrowWidth)?extraWidth:0)+(fileView?split:0)-(id==IDC_COVER?split:0);
        const int height=MulDiv(original.bottom-original.top,dpi,state.layoutDpi)+((flags&GrowHeight)?extraHeight:0);
        SetWindowPos(GetDlgItem(window,id),nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    updateListScrollbar(GetDlgItem(window,state.tiles?IDC_FILE_GRID:IDC_FILE_LIST),GetDlgItem(window,IDC_BOOK_SCROLL));
    if(state.reader)state.reader->resize();
    InvalidateRect(window,nullptr,TRUE);
}
void resizePreview(HWND window,Browser& state) {
    if(state.current.empty() || state.hydrating)return;
    RECT cover{}; GetClientRect(GetDlgItem(window,IDC_COVER),&cover);
    {
        std::lock_guard lock(state.worker->mutex);
        if(state.worker->path!=state.current || (state.worker->width==cover.right && state.worker->height==cover.bottom))return;
        ++state.worker->version;
        state.worker->path=state.current;
        state.worker->width=cover.right;
        state.worker->height=cover.bottom;
        state.worker->hydrate=false;
        state.worker->pending=true;
    }
    state.worker->wake.notify_one();
}
LRESULT CALLBACK dividerProc(HWND divider,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto window=GetParent(divider);
    auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,DWLP_USER));
    if(!state)return DefSubclassProc(divider,message,wp,lp);
    if(message==WM_SETCURSOR && reinterpret_cast<HWND>(wp)==divider && IsZoomed(window) && !state->fullscreen) {
        SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
    }
    if(message==WM_LBUTTONDOWN && IsZoomed(window) && !state->fullscreen) {
        POINT point{};GetCursorPos(&point);
        state->draggingDivider=true;
        state->dragStartX=point.x;
        state->dragStartWidth=dividerOffset(window,*state);
        SetCapture(divider);
        return 0;
    }
    if(message==WM_MOUSEMOVE && state->draggingDivider && GetCapture()==divider) {
        POINT point{};GetCursorPos(&point);
        const int width=std::clamp(state->dragStartWidth+static_cast<int>(point.x)-state->dragStartX,0,availableDividerWidth(window,*state));
        const int adjusted=MulDiv(width,state->layoutDpi,GetDpiForWindow(window));
        if(adjusted!=state->dividerExtraWidth) {
            state->dividerExtraWidth=adjusted;
            layoutBrowser(window,*state);
        }
        return 0;
    }
    if(message==WM_LBUTTONUP || message==WM_CANCELMODE || message==WM_CAPTURECHANGED) {
        if(state->draggingDivider) {
            state->draggingDivider=false;
            if(GetCapture()==divider)ReleaseCapture();
            if(!state->fullscreen)resizePreview(window,*state);
        }
        if(message!=WM_CAPTURECHANGED)return 0;
    }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};auto dc=BeginPaint(divider,&paint);
        RECT bounds{};GetClientRect(divider,&bounds);
        FillRect(dc,&bounds,dialogBackground(window));
        const auto dpi=GetDpiForWindow(window);
        const int line=(std::max)(1,MulDiv(1,dpi,96));
        const int dash=(std::max)(3,MulDiv(3,dpi,96));
        const int step=(std::max)(4,MulDiv(4,dpi,96));
        auto brush=CreateSolidBrush(usesDarkTheme(window)?RGB(118,118,118):GetSysColor(COLOR_3DSHADOW));
        for(int i=-1;i<=1;++i) {
            const int y=(bounds.bottom-step)/2+i*step;
            RECT mark{(bounds.right-dash)/2,y,(bounds.right+dash)/2,y+line};
            FillRect(dc,&mark,brush);
        }
        DeleteObject(brush);EndPaint(divider,&paint);return 0;
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(divider,dividerProc,1);
    return DefSubclassProc(divider,message,wp,lp);
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
    SetWindowLongPtrW(list,GWL_EXSTYLE,GetWindowLongPtrW(list,GWL_EXSTYLE)&~(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
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
    InvalidateRect(GetDlgItem(window,IDC_PANEL_DIVIDER),nullptr,TRUE);
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
        if(id==IDC_FILENAME) { RECT bottom{}; GetWindowRect(GetDlgItem(window,IDC_FILE_LIST),&bottom); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bottom),2); r.bottom=bottom.bottom; }
        InflateRect(&r,pad,pad); RoundRect(dc,r.left,r.top,r.right,r.bottom,pad*2,pad*2);
    }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); EndPaint(window,&paint);
}
void selectFile(HWND window,Browser& state,bool clearPreview=false,bool reloadUnchanged=true) {
    limitSelection(window);
    int count=static_cast<int>(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELCOUNT,0,0));
    std::wstring button=state.previewOnly ? (count<=1 ? L"Test sending" : L"Test sending "+std::to_wstring(count)+L" books") : count<=0 ? L"Send books" : count==1 ? L"Send book" : L"Send "+std::to_wstring(count)+L" books";
    SetDlgItemTextW(window,IDOK,button.c_str()); EnableWindow(GetDlgItem(window,IDOK),count>0);
    int index=static_cast<int>(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETCARETINDEX,0,0));
    auto previous=state.current;
    state.current=!clearPreview && index>=0 && static_cast<size_t>(index)<state.files.size() ? state.files[index] : fs::path{};
    if(previous!=state.current){state.hydrating=false;if(state.reader)state.reader->select(state.current);}
    for(auto id:{IDC_OPEN_FILE,IDC_OPEN_FOLDER,IDC_COPY_PATH}) EnableWindow(GetDlgItem(window,id),!state.current.empty());
    if(state.hydrating&&previous==state.current){updateDownloadAction(window,state);return;}
    if(!reloadUnchanged&&previous==state.current){updateDownloadAction(window,state);return;}
    RECT box{}; GetClientRect(GetDlgItem(window,IDC_COVER),&box);
    { std::lock_guard lock(state.worker->mutex);
      ++state.worker->version; state.worker->result.reset(); state.worker->path=state.current;
      state.worker->width=box.right; state.worker->height=box.bottom; state.worker->hydrate=false; state.worker->pending=!state.current.empty(); }
    state.worker->wake.notify_one();
    auto accessible=state.current.empty() ? L"Select a file to preview." : bookInformationText(state.current,{});
    SetDlgItemTextW(window,IDC_FILENAME,accessible.c_str());
    InvalidateRect(GetDlgItem(window,IDC_COVER),nullptr,TRUE); InvalidateRect(GetDlgItem(window,IDC_FILENAME),nullptr,TRUE);
    updateDownloadAction(window,state);
}
void applyFilters(HWND window,Browser& state,bool selectFirst=false,bool refreshing=false,bool preservePreview=false) {
    std::set<fs::path> selected;
    for(size_t i=0;i<state.files.size();++i) if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,i,0)>0) selected.insert(state.files[i]);
    auto previous=state.current; state.files.clear();
    wchar_t query[512]{}; GetDlgItemTextW(window,IDC_SEARCH,query,512);
    auto lowercase=[](std::wstring s) { std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));}); return s; };
    auto search=lowercase(query);
    for(const auto& file:state.allFiles) {
        auto format=fileFormat(file.extension().wstring());
        if(format && (state.allTypes || state.enabled[groupOf(file)]) && lowercase(displayedPath(state,file)).find(search)!=std::wstring::npos) state.files.push_back(file);
    }
    auto sort=SendDlgItemMessageW(window,IDC_SORT,CB_GETCURSEL,0,0);
    std::sort(state.files.begin(),state.files.end(),[&](const auto& a,const auto& b) {
        if((sort==2 || sort==3) && state.modified[a]!=state.modified[b]) return sort==2 ? state.modified[a]>state.modified[b] : state.modified[a]<state.modified[b];
        const auto aName=displayedPath(state,a),bName=displayedPath(state,b);
        return (sort==1 ? -1 : 1)*_wcsicmp(aName.c_str(),bName.c_str())<0;
    });
    SendDlgItemMessageW(window,IDC_FILE_LIST,WM_SETREDRAW,FALSE,0);
    SendDlgItemMessageW(window,IDC_FILE_LIST,LB_RESETCONTENT,0,0);
    for(size_t i=0;i<state.files.size();++i) {
        const auto name=displayedPath(state,state.files[i]);
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        if(selected.contains(state.files[i]) || (selectFirst && i==0)) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,i);
    }
    // LB_SETSEL also moves the caret: restore the preview only after all selections.
    auto caret=std::find(state.files.begin(),state.files.end(),previous);
    if(caret!=state.files.end()) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,std::distance(state.files.begin(),caret),FALSE);
    SendDlgItemMessageW(window,IDC_FILE_LIST,WM_SETREDRAW,TRUE,0); InvalidateRect(GetDlgItem(window,IDC_FILE_LIST),nullptr,TRUE);
    RECT position{68,72,104,90}; MapDialogRect(window,&position);
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
    SetDlgItemTextW(window,IDC_BROWSER_HELP,none ? L"Choose a file type above, or All types." : state.files.empty() ? L"No matching files. Change the search or file types." : L"Hold Ctrl or Shift to select up to 100 files.");
    auto count=std::to_wstring(state.files.size())+(state.files.size()==1 ? L" file" : L" files");
    SetDlgItemTextW(window,IDC_FILE_COUNT,count.c_str());
    rebuildGrid(window,state);
    updateListScrollbar(GetDlgItem(window,IDC_FILE_LIST),GetDlgItem(window,IDC_BOOK_SCROLL));
    selectFile(window,state,refreshing && std::find(state.files.begin(),state.files.end(),previous)==state.files.end(),!preservePreview);
}
void updateScanStatus(HWND window,const Browser& state) {
    const auto text=state.scanActive ? L"Scanning subfolders\u2026 "+std::to_wstring(state.subfolderFilesFound)+
        (state.subfolderFilesFound==1 ? L" file found" : L" files found") : std::wstring{};
    SetDlgItemTextW(window,IDC_SCAN_STATUS,text.c_str());
}
void cancelSubfolderScan(HWND window,Browser& state) {
    {
        std::lock_guard lock(state.scanWorker->mutex);
        state.scanGeneration=++state.scanWorker->generation;
        state.scanWorker->updates.clear();
    }
    state.scanActive=false;
    state.subfolderFilesFound=0;
    updateScanStatus(window,state);
}
bool scanCancelled(const std::shared_ptr<ScanWorker>& worker,size_t generation) {
    std::lock_guard lock(worker->mutex);
    return worker->stop||worker->generation!=generation;
}
bool queueScanUpdate(const std::shared_ptr<ScanWorker>& worker,ScanUpdate update) {
    std::lock_guard lock(worker->mutex);
    if(worker->stop||worker->generation!=update.generation)return false;
    worker->updates.push_back(std::move(update));
    if(worker->window)PostMessageW(worker->window,SubfolderScanReady,0,0);
    return true;
}
void scanSubfolders(const std::shared_ptr<ScanWorker>& worker,const fs::path& folder,size_t generation) {
    constexpr size_t BatchSize=32;
    const auto normalizedFolder=folder.lexically_normal();
    std::vector<ScannedFile> batch;
    batch.reserve(BatchSize);
    auto lastBatch=std::chrono::steady_clock::now();
    auto flush=[&] {
        if(batch.empty())return true;
        if(!queueScanUpdate(worker,{generation,std::move(batch),false}))return false;
        batch.clear();batch.reserve(BatchSize);lastBatch=std::chrono::steady_clock::now();return true;
    };
    std::error_code error;
    fs::recursive_directory_iterator it(folder,fs::directory_options::skip_permission_denied,error),end;
    while(it!=end) {
        if(scanCancelled(worker,generation))return;
        const auto path=it->path();
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))it.disable_recursion_pending();
        std::error_code entryError;
        if(path.parent_path().lexically_normal()!=normalizedFolder&&it->is_regular_file(entryError)&&!entryError&&fileFormat(path.extension().wstring())) {
            ScannedFile file;file.path=path;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)) {
                file.modified=(static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime;
                file.hasModified=true;
            }
            auto size=fs::file_size(path,entryError);
            if(!entryError){file.size=size;file.hasSize=true;}
            batch.push_back(std::move(file));
        }
        if((batch.size()>=BatchSize||(!batch.empty()&&std::chrono::steady_clock::now()-lastBatch>=std::chrono::milliseconds(150)))&&!flush())return;
        error.clear();it.increment(error);error.clear();
    }
    if(!flush())return;
    queueScanUpdate(worker,{generation,{},true});
}
void startSubfolderScan(HWND window,Browser& state) {
    if(!state.includeSubfolders||state.folder.empty())return;
    {
        std::lock_guard lock(state.scanWorker->mutex);
        state.scanGeneration=++state.scanWorker->generation;
        state.scanWorker->updates.clear();
        state.scanWorker->window=window;
    }
    state.scanActive=true;
    state.subfolderFilesFound=0;
    updateScanStatus(window,state);
    const auto worker=state.scanWorker;
    const auto folder=state.folder;
    const auto generation=state.scanGeneration;
    std::thread([worker,folder,generation]{scanSubfolders(worker,folder,generation);}).detach();
}
void applySubfolderScanUpdates(HWND window,Browser& state) {
    std::deque<ScanUpdate> updates;
    {
        std::lock_guard lock(state.scanWorker->mutex);
        updates.swap(state.scanWorker->updates);
    }
    bool changed=false,complete=false;
    for(auto& update:updates) {
        if(update.generation!=state.scanGeneration||!state.includeSubfolders)continue;
        for(auto& file:update.files) {
            state.allFiles.push_back(file.path);
            if(file.hasSize)state.sizes[file.path]=file.size;
            if(file.hasModified)state.modified[file.path]=file.modified;
        }
        state.subfolderFilesFound+=update.files.size();
        changed|=!update.files.empty();
        complete|=update.complete;
    }
    if(changed)applyFilters(window,state,false,false,true);
    if(complete)state.scanActive=false;
    updateScanStatus(window,state);
}
void populateFolder(HWND window,Browser& state,const fs::path& folder,bool refreshing=false) {
    std::vector<fs::path> files; std::error_code error;
    for(fs::directory_iterator it(folder,error),end; !error && it!=end; it.increment(error)) {
        if(it->is_regular_file(error) && fileFormat(it->path().extension().wstring())) files.push_back(it->path());
    }
    if(error) throw std::runtime_error("Could not read this folder.");
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return _wcsicmp(a.filename().c_str(),b.filename().c_str())<0;});
    cancelSubfolderScan(window,state);
    state.folder=folder;
    state.directFiles=std::move(files);
    state.allFiles=state.directFiles;
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
            const bool placeholder=cloudPlaceholder(path);
            auto image=placeholder ? nullptr : loadCover(path,96,120);
            lock.lock();
            if(thumbnails->stop) { if(image) DeleteObject(image); break; }
            while(thumbnails->images.size()>=256) { auto old=thumbnails->order.front(); thumbnails->order.pop_front(); if(thumbnails->images[old]) DeleteObject(thumbnails->images[old]); thumbnails->images.erase(old); }
            thumbnails->images[path]=image; thumbnails->order.push_back(path); thumbnails->pending.erase(path);
            if(!placeholder)PostMessageW(window,ThumbnailsReady,0,0);
        }
        if(SUCCEEDED(com)) CoUninitialize();
    }).detach();
    startSubfolderScan(window,state);
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
    auto background=CreateSolidBrush(interactionColor(draw.hwndItem,selected ? GetSysColor(COLOR_HIGHLIGHT) : dark ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW),hotItem(draw.hwndItem)==static_cast<int>(draw.itemID)));
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
    auto name=displayedPath(state,path); text.bottom=text.top+metrics.tmHeight;
    DrawTextW(draw.hDC,name.c_str(),-1,&text,DT_SINGLELINE|DT_PATH_ELLIPSIS|DT_NOPREFIX);
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

void roundBrowserMenu(HWND window) {
    RECT bounds{};GetClientRect(window,&bounds);
    const int radius=MulDiv(12,GetDpiForWindow(window),96);
    auto region=CreateRoundRectRgn(0,0,bounds.right+1,bounds.bottom+1,radius,radius);
    if(!SetWindowRgn(window,region,TRUE))DeleteObject(region);
}
void closeBrowserMenu(HWND window,bool restoreFocus) {
    auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,DWLP_USER));
    auto owner=GetWindow(window,GW_OWNER);
    if(state&&state->menu==window)state->menu=nullptr;
    DestroyWindow(window);
    if(restoreFocus&&IsWindow(owner))SetFocus(GetDlgItem(owner,IDC_BROWSER_MENU));
}
LRESULT CALLBACK browserMenuButtonProc(HWND button,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto menu=reinterpret_cast<HWND>(data);
    if(message==WM_KEYDOWN) {
        if(wp==VK_ESCAPE){closeBrowserMenu(menu,true);return 0;}
        if(wp==VK_UP||wp==VK_DOWN||wp==VK_TAB) {
            constexpr int ids[]{IDC_MENU_SETTINGS,IDC_MENU_ABOUT,IDC_MENU_CHECK_UPDATES};
            int current=0;for(int i=0;i<3;++i)if(GetDlgItem(menu,ids[i])==button)current=i;
            const bool backwards=wp==VK_UP||(wp==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000));
            SetFocus(GetDlgItem(menu,ids[(current+(backwards?2:1))%3]));return 0;
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(button,browserMenuButtonProc,1);
    return DefSubclassProc(button,message,wp,lp);
}
INT_PTR CALLBACK browserMenuProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<Browser*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);applyTheme(window);
        for(auto id:{IDC_MENU_SETTINGS,IDC_MENU_ABOUT,IDC_MENU_CHECK_UPDATES})
            SetWindowSubclass(GetDlgItem(window,id),browserMenuButtonProc,1,reinterpret_cast<DWORD_PTR>(window));
        EnableWindow(GetDlgItem(window,IDC_MENU_SETTINGS),state->menuActions.settings!=nullptr);
        EnableWindow(GetDlgItem(window,IDC_MENU_ABOUT),state->menuActions.about!=nullptr);
        EnableWindow(GetDlgItem(window,IDC_MENU_CHECK_UPDATES),state->menuActions.about!=nullptr);
        roundBrowserMenu(window);return TRUE;
    }
    if(!state)return FALSE;
    if(message==WM_COMMAND) {
        const auto command=LOWORD(wp);
        if(command==IDC_MENU_SETTINGS||command==IDC_MENU_ABOUT||command==IDC_MENU_CHECK_UPDATES) {
            auto owner=GetWindow(window,GW_OWNER);closeBrowserMenu(window,false);
            try {
                if(command==IDC_MENU_SETTINGS&&state->menuActions.settings)state->menuActions.settings(owner);
                else if(command!=IDC_MENU_SETTINGS&&state->menuActions.about&&state->menuActions.about(owner,command==IDC_MENU_CHECK_UPDATES))EndDialog(owner,IDCANCEL);
            } catch(const std::exception& e) { themedMessageBox(owner,wide(e.what()).c_str(),L"Voltura Books",MB_OK|MB_ICONERROR); }
            return TRUE;
        }
    }
    if(message==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE) {
        POINT point{};GetCursorPos(&point);
        if(WindowFromPoint(point)==GetDlgItem(GetWindow(window,GW_OWNER),IDC_BROWSER_MENU))state->suppressMenuClick=true;
        closeBrowserMenu(window,false);return TRUE;
    }
    if(message==WM_CLOSE){closeBrowserMenu(window,true);return TRUE;}
    if(message==WM_DPICHANGED) {
        auto suggested=reinterpret_cast<RECT*>(lp);
        SetWindowPos(window,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
        roundBrowserMenu(window);return TRUE;
    }
    if(message==WM_ERASEBKGND)return TRUE;
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT bounds{};GetClientRect(window,&bounds);
        FillRect(dc,&bounds,panelBackground(window));
        auto pen=CreatePen(PS_SOLID,(std::max)(1,MulDiv(1,GetDpiForWindow(window),96)),usesDarkTheme(window)?RGB(78,78,78):GetSysColor(COLOR_3DSHADOW));
        auto oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        const int radius=MulDiv(12,GetDpiForWindow(window),96);RoundRect(dc,0,0,bounds.right,bounds.bottom,radius,radius);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);EndPaint(window,&paint);return TRUE;
    }
    if(message==WM_NCDESTROY&&state->menu==window)state->menu=nullptr;
    return FALSE;
}
void showBrowserMenu(HWND owner,Browser& state) {
    if(state.menu){closeBrowserMenu(state.menu,true);return;}
    auto menu=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_BROWSER_MENU),owner,browserMenuProc,reinterpret_cast<LPARAM>(&state));
    if(!menu)throw std::runtime_error("Could not open the menu.");
    state.menu=menu;
    RECT anchor{},bounds{};GetWindowRect(GetDlgItem(owner,IDC_BROWSER_MENU),&anchor);GetWindowRect(menu,&bounds);
    const LONG width=bounds.right-bounds.left,height=bounds.bottom-bounds.top,gap=MulDiv(4,GetDpiForWindow(owner),96);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor);
    LONG x=(std::min)((std::max)(anchor.left,monitor.rcWork.left),monitor.rcWork.right-width);
    LONG y=anchor.bottom+gap;if(y+height>monitor.rcWork.bottom)y=anchor.top-gap-height;
    y=(std::min)((std::max)(y,monitor.rcWork.top),monitor.rcWork.bottom-height);
    SetWindowPos(menu,HWND_TOP,x,y,width,height,SWP_SHOWWINDOW);
    SetForegroundWindow(menu);SetFocus(GetDlgItem(menu,IDC_MENU_SETTINGS));
}

INT_PTR CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto state=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_MEASUREITEM && wp==IDC_FILE_LIST) { reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=48; return TRUE; }
        if(message==WM_INITDIALOG) {
            state=reinterpret_cast<Browser*>(lp); SetWindowLongPtrW(window,DWLP_USER,lp); applyTheme(window);
            captureBrowserLayout(window,*state);
            activeBrowser=window;
            ShowWindow(GetDlgItem(window,IDC_DOWNLOAD_VIEW),SW_HIDE);
            { std::lock_guard lock(state->scanWorker->mutex); state->scanWorker->window=window; }
            SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_SETCHECK,BST_UNCHECKED,0);
            for(auto id:{IDC_FILE_LIST,IDC_FILE_GRID})SetWindowSubclass(GetDlgItem(window,id),hoverItemsProc,1,0);
            SetWindowSubclass(GetDlgItem(window,IDC_PANEL_DIVIDER),dividerProc,1,0);
            state->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            SetWindowTheme(state->tooltip,L"",L"");
            TOOLINFOW refreshTool{sizeof(refreshTool)}; refreshTool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
            refreshTool.hwnd=window; refreshTool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,IDC_REFRESH));
            refreshTool.lpszText=const_cast<LPWSTR>(L"Refresh (F5)");
            SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&refreshTool));
            TOOLINFOW menuTool{sizeof(menuTool)};menuTool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
            menuTool.hwnd=window;menuTool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window,IDC_BROWSER_MENU));
            menuTool.lpszText=const_cast<LPWSTR>(L"Menu");
            SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&menuTool));
            styleRefreshTooltip(window,*state);
            state->reader=std::make_unique<Reader>(window,GetDlgItem(window,IDC_COVER),[window,state]{toggleFullscreen(window,*state);});
            if(state->previewOnly) SetWindowTextW(window,L"Voltura Books - Browse books (no emails sent)");
            auto appIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_BOOK));
            SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(appIcon));
            SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(appIcon));
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
                    auto path=worker->path; auto version=worker->version; int width=worker->width,height=worker->height; bool hydrate=worker->hydrate; worker->pending=false; worker->hydrate=false; lock.unlock();
                    auto result=std::make_shared<Preview>();
                    result->downloadRequested=hydrate;
                    if(hydrate)result->downloaded=hydrateFile(path,worker,version);
                    if(SUCCEEDED(com)&&(result->downloaded||!cloudPlaceholder(path))) {
                        auto format=fileFormat(path.extension().wstring());
                        if(!format || !std::string(format->mime).starts_with("image/")) {
                            width=(std::min)(width,2048); height=(std::min)(height,2048);
                        }
                        result->cover=loadCover(path,width,height); result->details=loadBookDetails(path);
                    }
                    lock.lock(); if(!worker->stop && worker->version==version) {
                        // Selection changes clear result; a same-selection resize
                        // must not replace a valid preview with a failed decode.
                        if(result->downloadRequested||result->cover||!worker->result||!worker->result->cover)worker->result=result;
                        PostMessageW(window,PreviewReady,static_cast<WPARAM>(version),0);
                    }
                }
                if(SUCCEEDED(com)) CoUninitialize();
            }).detach();
            if(!state->initialFolder.empty()) {
                try {
                    populateFolder(window,*state,state->initialFolder);
                    if(state->rememberInitialFolder) saveBrowseFolder(state->initialFolder);
                } catch(...) { EndDialog(window,IDCANCEL); }
            }
            return TRUE;
        }
        if(!state) return FALSE;
        if(message==WM_ENTERSIZEMOVE){state->resizing=true;return TRUE;}
        if(message==WM_EXITSIZEMOVE){state->resizing=false;resizePreview(window,*state);return TRUE;}
        if(message==WM_GETMINMAXINFO && state->layoutReady && !state->fullscreen) {
            RECT frame{},client{}; GetWindowRect(window,&frame); GetClientRect(window,&client);
            const auto dpi=GetDpiForWindow(window);
            auto limits=reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize.x=(std::max)(limits->ptMinTrackSize.x,MulDiv(state->initialClientSize.cx,dpi,state->layoutDpi)+(frame.right-frame.left)-(client.right-client.left));
            limits->ptMinTrackSize.y=(std::max)(limits->ptMinTrackSize.y,MulDiv(state->initialClientSize.cy,dpi,state->layoutDpi)+(frame.bottom-frame.top)-(client.bottom-client.top));
            return TRUE;
        }
        if(message==BrowseFolderRequest && lp) {
            const auto& folder=*reinterpret_cast<const fs::path*>(lp);
            try { populateFolder(window,*state,folder); }
            catch(...) { SetWindowLongPtrW(window,DWLP_MSGRESULT,FALSE); return TRUE; }
            saveBrowseFolder(folder);
            if(IsIconic(window))ShowWindow(window,SW_RESTORE);
            SetForegroundWindow(window); SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE); return TRUE;
        }
        if((message==DialogShortcut&&wp==VK_F5)||(message==WM_COMMAND&&LOWORD(wp)==IDC_REFRESH)) {
            refreshFolder(window,*state); SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE); return TRUE;
        }
        if(message==WM_COMMAND&&LOWORD(wp)==IDC_BROWSER_MENU) {
            if(state->suppressMenuClick){state->suppressMenuClick=false;SetFocus(GetDlgItem(window,IDC_BROWSER_MENU));return TRUE;}
            showBrowserMenu(window,*state);return TRUE;
        }
        if(message==DialogShortcut&&wp==VK_F11&&state->reader&&state->reader->canFullscreen()){toggleFullscreen(window,*state);SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE);return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)==IDC_FULLSCREEN_READER&&state->reader){state->reader->openFullscreen();return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)==IDC_DOWNLOAD_VIEW&&!state->current.empty()&&needsDownload(*state,state->current)){
            RECT box{};GetClientRect(GetDlgItem(window,IDC_COVER),&box);
            state->hydrating=true;
            {std::lock_guard lock(state->worker->mutex);++state->worker->version;state->worker->result.reset();state->worker->path=state->current;state->worker->width=box.right;state->worker->height=box.bottom;state->worker->hydrate=true;state->worker->pending=true;}
            state->worker->wake.notify_one();updateDownloadAction(window,*state);return TRUE;
        }
        if(message==WM_DESTROY){
            if(activeBrowser==window)activeBrowser=nullptr;
            if(state->menu){auto menu=state->menu;state->menu=nullptr;DestroyWindow(menu);}
            { std::lock_guard lock(state->scanWorker->mutex); state->scanWorker->stop=true; ++state->scanWorker->generation; state->scanWorker->window=nullptr; state->scanWorker->updates.clear(); }
            DestroyWindow(state->tooltip);state->tooltip=nullptr;state->reader.reset();return TRUE;
        }
        if(message==WM_COMMAND&&LOWORD(wp)>=IDC_READER_PREVIOUS&&LOWORD(wp)<=IDC_READER_FULLSCREEN&&state->reader){SendMessageW(state->reader->window(),WM_COMMAND,wp,lp);return TRUE;}
        if(message==WM_DRAWITEM&&wp>=IDC_READER_PREVIOUS&&wp<=IDC_READER_FULLSCREEN&&state->reader){SendMessageW(state->reader->window(),message,wp,lp);return TRUE;}
        if(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL&&state->fullscreen){toggleFullscreen(window,*state);return TRUE;}
        if(message==WM_SIZE&&state->fullscreen&&state->reader){RECT r{};GetClientRect(window,&r);SetWindowPos(GetDlgItem(window,IDC_COVER),nullptr,0,0,r.right,r.bottom,SWP_NOZORDER|SWP_NOACTIVATE);state->reader->resize();return TRUE;}
        if(message==WM_SIZE){
            if(state->draggingDivider && !IsZoomed(window) && GetCapture()==GetDlgItem(window,IDC_PANEL_DIVIDER))ReleaseCapture();
            layoutBrowser(window,*state);
            InvalidateRect(GetDlgItem(window,IDC_PANEL_DIVIDER),nullptr,TRUE);
            if(!state->resizing)resizePreview(window,*state);
            return TRUE;
        }
        if(message==WM_PAINT&&state->fullscreen){PAINTSTRUCT p{};auto dc=BeginPaint(window,&p);FillRect(dc,&p.rcPaint,panelBackground(window));EndPaint(window,&p);return TRUE;}
        if(message==WM_PAINT) { paintPanels(window); return TRUE; }
        if(message==WM_APP+22 || message==WM_SETTINGCHANGE || message==WM_THEMECHANGED) { styleBrowser(window); styleRefreshTooltip(window,*state); return TRUE; }
        if(message==WM_COMMAND && (LOWORD(wp)==IDC_VIEW_LIST || LOWORD(wp)==IDC_VIEW_THUMBS)) { setView(window,*state,LOWORD(wp)==IDC_VIEW_THUMBS); return TRUE; }
        if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->idFrom==IDC_FILE_GRID) {
            auto header=reinterpret_cast<NMHDR*>(lp);
            if(header->code==LVN_ITEMCHANGING&&!state->rebuilding) {
                auto changed=reinterpret_cast<NMLISTVIEW*>(lp);
                if((changed->uChanged&LVIF_STATE)&&!(changed->uOldState&LVIS_SELECTED)&&(changed->uNewState&LVIS_SELECTED)&&ListView_GetSelectedCount(header->hwndFrom)>=MaximumSelection) {
                    SetWindowLongPtrW(window,DWLP_MSGRESULT,TRUE);return TRUE;
                }
            }
            if(header->code==NM_CUSTOMDRAW && !state->rebuilding) {
                auto draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if(draw->nmcd.dwDrawStage==CDDS_PREPAINT) { SetWindowLongPtrW(window,DWLP_MSGRESULT,CDRF_NOTIFYITEMDRAW); return TRUE; }
                if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec<state->files.size()) {
                    const auto& path=state->files[draw->nmcd.dwItemSpec];
                    { std::lock_guard lock(state->thumbnails->mutex);
                      if(!state->thumbnails->images.contains(path) && state->thumbnails->pending.insert(path).second) state->thumbnails->requests.push_back(path); }
                    state->thumbnails->wake.notify_one();
                    if(hotItem(header->hwndFrom)==static_cast<int>(draw->nmcd.dwItemSpec)) {
                        draw->nmcd.uItemState|=CDIS_HOT;
                        SetWindowLongPtrW(window,DWLP_MSGRESULT,CDRF_NOTIFYPOSTPAINT);return TRUE;
                    }
                }
            }
            if(header->code==NM_CUSTOMDRAW) {
                auto draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if(draw->nmcd.dwDrawStage==CDDS_ITEMPOSTPAINT&&hotItem(header->hwndFrom)==static_cast<int>(draw->nmcd.dwItemSpec)) {
                    HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
                    if(!(contrast.dwFlags&HCF_HIGHCONTRASTON)) {
                        RECT bounds{};ListView_GetItemRect(header->hwndFrom,static_cast<int>(draw->nmcd.dwItemSpec),&bounds,LVIR_BOUNDS);
                        auto brush=CreateSolidBrush(usesDarkTheme(window)?RGB(105,105,105):RGB(150,150,150));FrameRect(draw->nmcd.hdc,&bounds,brush);DeleteObject(brush);
                    }
                }
            }
            if(header->code==LVN_ITEMCHANGED && !state->rebuilding) {
                if(!state->gridSelectionPending){state->gridSelectionPending=true;PostMessageW(window,GridSelectionChanged,0,0);}
                return TRUE;
            }
        }
        if(message==GridSelectionChanged) {
            state->gridSelectionPending=false;
            auto grid=GetDlgItem(window,IDC_FILE_GRID);
            SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);
            int copied=0;
            for(int i=-1;copied<MaximumSelection&&(i=ListView_GetNextItem(grid,i,LVNI_SELECTED))!=-1;++copied)SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,i);
            int focus=ListView_GetNextItem(grid,-1,LVNI_FOCUSED);
            if(focus>=0)SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,focus,FALSE);
            selectFile(window,*state);return TRUE;
        }
        if(message==ThumbnailsReady) { if(state->tiles) SetTimer(window,3,150,nullptr); return TRUE; }
        if(message==SubfolderScanReady) { applySubfolderScanUpdates(window,*state); return TRUE; }
        if(message==WM_TIMER && wp==3) { KillTimer(window,3); rebuildGrid(window,*state); return TRUE; }
        if(message==WM_TIMER && wp==4) { InvalidateRect(GetDlgItem(window,IDC_DOWNLOAD_VIEW),nullptr,FALSE); return TRUE; }
        if(message==WM_DPICHANGED) { if(state->fullscreen){MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);SetWindowPos(window,nullptr,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOZORDER|SWP_NOACTIVATE);}else{layoutBrowser(window,*state);sizeRows(window);styleBrowser(window);rebuildGrid(window,*state);if(!state->resizing)resizePreview(window,*state);}if(state->reader)state->reader->resize();return TRUE; }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_CLEAR_SEARCH) { SetFocus(GetDlgItem(window,IDC_SEARCH));SetDlgItemTextW(window,IDC_SEARCH,L"");return TRUE; }
        if(message==WM_DRAWITEM && wp==IDC_CLEAR_SEARCH) {
            auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp);const bool dark=usesDarkTheme(window);
            auto brush=CreateSolidBrush(dark?RGB(45,45,45):GetSysColor(COLOR_WINDOW));FillRect(draw->hDC,&draw->rcItem,brush);DeleteObject(brush);
            const bool hot=controlHovered(draw->hwndItem),pressed=IsWindowEnabled(draw->hwndItem)&&(draw->itemState&ODS_SELECTED);
            if(hot||pressed) {
                const auto color=interactionColor(draw->hwndItem,dark?RGB(45,45,45):GetSysColor(COLOR_WINDOW),hot,pressed);
                auto background=CreateSolidBrush(color);auto oldBrush=SelectObject(draw->hDC,background);auto oldPen=SelectObject(draw->hDC,GetStockObject(NULL_PEN));
                const auto& r=draw->rcItem;const int radius=MulDiv(6,GetDpiForWindow(window),96);RoundRect(draw->hDC,r.left,r.top,r.right,r.bottom,radius,radius);
                SelectObject(draw->hDC,oldBrush);SelectObject(draw->hDC,oldPen);DeleteObject(background);
            }
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
        if(message==WM_COMMAND && LOWORD(wp)==IDC_INCLUDE_SUBFOLDERS) {
            state->includeSubfolders=SendDlgItemMessageW(window,IDC_INCLUDE_SUBFOLDERS,BM_GETCHECK,0,0)==BST_CHECKED;
            cancelSubfolderScan(window,*state);
            state->allFiles=state->directFiles;
            applyFilters(window,*state);
            if(state->includeSubfolders)startSubfolderScan(window,*state);
            return TRUE;
        }
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
            const bool downloadFailed=preview&&preview->downloadRequested&&!preview->downloaded;
            if(preview&&preview->downloadRequested){state->hydrating=false;if(preview->downloaded)state->hydratedFiles.insert(state->current);}
            if(preview) SetDlgItemTextW(window,IDC_FILENAME,bookInformationText(state->current,preview->details).c_str());
            if(preview&&state->reader)state->reader->previewReady(preview->details.pages,preview->cover!=nullptr);
            if(state->reader)state->reader->imageLoaded(static_cast<uint64_t>(wp));
            if(preview&&preview->downloaded){std::lock_guard lock(state->thumbnails->mutex);if(auto found=state->thumbnails->images.find(state->current);found!=state->thumbnails->images.end()){if(found->second)DeleteObject(found->second);state->thumbnails->images.erase(found);}state->thumbnails->pending.erase(state->current);InvalidateRect(GetDlgItem(window,IDC_FILE_GRID),nullptr,FALSE);}
            updateDownloadAction(window,*state);
            InvalidateRect(GetDlgItem(window,IDC_COVER),nullptr,TRUE); InvalidateRect(GetDlgItem(window,IDC_FILENAME),nullptr,TRUE);
            if(!state->resizing && !state->fullscreen)resizePreview(window,*state);
            if(downloadFailed)themedMessageBox(window,L"OneDrive could not download this file. Check your connection and try again.",L"Voltura Books",MB_OK|MB_ICONERROR);
            return TRUE;
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
            count=(std::min)(count,MaximumSelection);
            std::vector<int> indices(count); SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSELITEMS,count,reinterpret_cast<LPARAM>(indices.data()));
            for(auto index:indices) if(index>=0 && static_cast<size_t>(index)<state->files.size()) state->selected.push_back(state->files[index]);
            EndDialog(window,IDOK); return TRUE;
        }
        if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) { EndDialog(window,IDCANCEL); return TRUE; }
    } catch(const std::exception& e) { themedMessageBox(window,wide(e.what()).c_str(),L"Voltura Books",MB_OK|MB_ICONERROR); }
    return FALSE;
}
}
bool navigateBrowseBooks(const fs::path& folder) {
    return activeBrowser && SendMessageW(activeBrowser,BrowseFolderRequest,0,reinterpret_cast<LPARAM>(&folder))==TRUE;
}
std::vector<fs::path> browseBooks(HWND owner,const fs::path& initialFolder,bool previewOnly,bool rememberInitialFolder,BrowserMenuActions menuActions) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    Browser state; state.initialFolder=initialFolder; state.previewOnly=previewOnly; state.rememberInitialFolder=rememberInitialFolder;state.menuActions=menuActions;
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
