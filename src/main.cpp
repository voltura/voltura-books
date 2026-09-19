#include "core.h"
#include "resource.h"
#include "cover.h"
#include "placeholder.h"
#include "book_information.h"
#include "file_icons.h"
#include "browser.h"
#include "history.h"
#include "mail_setup.h"
#include "theme.h"
#include "about.h"
#include "list_scrollbar.h"
#include "drop.h"
#include "formats.h"
#include "shell_selection.h"
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <curl/curl.h>
#include <thread>
#include <memory>
#include <vector>

namespace books {
static bool previewOnly=false;
static constexpr wchar_t InstanceClass[]=L"VolturaBooks.Instance";
static constexpr UINT IncomingFiles=WM_APP+70;
static constexpr UINT BrowseRequest=WM_APP+72;
static HWND dropWindow=nullptr;
static bool sendingQueue=false,confirmingIncoming=false;
static std::vector<fs::path> pendingFiles;
static fs::path pendingBrowseFolder;
static std::vector<std::unique_ptr<std::vector<fs::path>>> deferredRequests;
static std::wstring browseFolderError(const fs::path& folder) {
    std::error_code error;
    if(folder.empty() || !fs::is_directory(folder,error) || error)return L"Choose an existing folder to browse.";
    fs::directory_iterator probe(folder,error);
    if(error)return L"Voltura Books cannot read this folder.";
    return {};
}
static HWND foregroundDialog() {
    HWND target=nullptr;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM value)->BOOL {
        if(IsWindowVisible(window) && IsWindowEnabled(window)) *reinterpret_cast<HWND*>(value)=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&target));
    if(target) { if(IsIconic(target)) ShowWindow(target,SW_RESTORE); SetForegroundWindow(target); }
    return target;
}
static LRESULT CALLBACK instanceProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_COPYDATA) {
        auto data=reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if(data && data->dwData==3) {
            if(!data->lpData || data->cbData<sizeof(wchar_t) || data->cbData>65536 || data->cbData%sizeof(wchar_t))return FALSE;
            auto text=static_cast<const wchar_t*>(data->lpData);size_t count=data->cbData/sizeof(wchar_t);
            if(text[count-1] || wcsnlen(text,count)!=count-1)return FALSE;
            fs::path folder(std::wstring(text,count-1));
            if(!browseFolderError(folder).empty())return FALSE;
            if(navigateBrowseBooks(folder))return TRUE;
            const bool launcherReady=dropWindow && IsWindowVisible(dropWindow) && IsWindowEnabled(dropWindow);
            foregroundDialog();
            if(!launcherReady)return 3;
            pendingBrowseFolder=std::move(folder);
            return PostMessageW(dropWindow,BrowseRequest,FALSE,0);
        }
        if(data && data->dwData==2) {
            if(data->cbData!=sizeof(DWORD) || !data->lpData)return FALSE;
            DWORD testSending=0;memcpy(&testSending,data->lpData,sizeof(testSending));
            if(testSending>1)return FALSE;
            const bool launcherReady=dropWindow && IsWindowVisible(dropWindow) && IsWindowEnabled(dropWindow);
            // Never change the mode of an open browser or an in-flight queue.
            if(!launcherReady && previewOnly!=(testSending!=0))return 2;
            foregroundDialog();
            if(launcherReady)return PostMessageW(dropWindow,BrowseRequest,testSending,0);
            return TRUE;
        }
        if(!data || data->dwData!=1 || !data->lpData || data->cbData<sizeof(wchar_t) || data->cbData>1024*1024 || data->cbData%sizeof(wchar_t)) return FALSE;
        auto text=static_cast<const wchar_t*>(data->lpData); size_t count=data->cbData/sizeof(wchar_t);
        if(text[count-1]) return FALSE;
        auto files=std::make_unique<std::vector<fs::path>>();
        for(size_t i=0;i<count && text[i];) {
            size_t end=i; while(end<count && text[end]) ++end;
            if(end==count) return FALSE;
            files->emplace_back(std::wstring(text+i,end-i)); i=end+1;
        }
        if(!PostMessageW(window,IncomingFiles,0,reinterpret_cast<LPARAM>(files.get()))) return FALSE;
        files.release(); return TRUE;
    }
    if(message==IncomingFiles) {
        std::unique_ptr<std::vector<fs::path>> files(reinterpret_cast<std::vector<fs::path>*>(lp));
        auto owner=foregroundDialog();
        if(files->empty()) return 0;
        // Serialize prompts received during a nested modal message loop.
        if(confirmingIncoming) { deferredRequests.push_back(std::move(files)); return 0; }
        for(const auto& file:*files) if(!validateFile(file).empty()) {
            themedMessageBox(owner,L"One of the selected files cannot be sent. Check its type, size, and location.",L"Voltura Books",MB_OK|MB_ICONERROR); return 0;
        }
        if(sendingQueue) {
            confirmingIncoming=true;
            auto answer=themedMessageBox(owner,previewOnly ? L"Add these files after the current sending tests? No emails will be sent." : L"Add these files after the current books?",L"Voltura Books",MB_YESNO|MB_ICONQUESTION);
            confirmingIncoming=false;
            for(auto& request:deferredRequests) if(PostMessageW(window,IncomingFiles,0,reinterpret_cast<LPARAM>(request.get()))) request.release();
            deferredRequests.clear();
            if(answer!=IDYES) return 0;
        }
        pendingFiles.insert(pendingFiles.end(),files->begin(),files->end());
        SetTimer(window,1,100,nullptr); return 0;
    }
    if(message==WM_TIMER) {
        if(pendingFiles.empty() || sendingQueue) KillTimer(window,1);
        else if(dropWindow && IsWindowEnabled(dropWindow)) PostMessageW(dropWindow,IncomingFiles,0,0);
        return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
static HICON fileTypeIcon(const fs::path& path) {
    return loadFileTypeIcon(path);
}
static void error(HWND owner, const std::wstring& message) { themedMessageBox(owner, message.c_str(), L"Voltura Books", MB_OK | MB_ICONERROR); }
static void setBookIcon(HWND window) {
    applyTheme(window);
    auto instance = GetModuleHandleW(nullptr);
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_BOOK), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON,GetDpiForWindow(window)), GetSystemMetricsForDpi(SM_CYSMICON,GetDpiForWindow(window)), LR_SHARED)));
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(instance, MAKEINTRESOURCEW(IDI_BOOK))));
}
static void fitFilename(HWND window, const fs::path& path, const BookDetails& details) {
    auto filename=bookInformationText(path,details);
    HWND label = GetDlgItem(window, IDC_FILENAME);
    SetWindowTextW(label, filename.c_str());
    RECT bounds{}; GetWindowRect(label, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    HDC dc = GetDC(label);
    auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0)));
    RECT measured{0, 0, bounds.right - bounds.left, 0};
    measured.bottom=drawBookInformation(dc,measured,reinterpret_cast<HFONT>(SendMessageW(label,WM_GETFONT,0,0)),path,details,false,true,nullptr,true);
    SelectObject(dc, previous); ReleaseDC(label, dc);
    const int extra = (std::max)(0L, measured.bottom + 4 - (bounds.bottom - bounds.top));
    if (!extra) return;
    SetWindowPos(label, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top + extra, SWP_NOMOVE | SWP_NOZORDER);
    for (auto id : {IDC_STATUS, IDC_SEND_HELP, IDC_PROGRESS, IDC_SETTINGS, IDC_SKIP, IDC_RETRY, IDCANCEL}) {
        RECT r{}; GetWindowRect(GetDlgItem(window, id), &r);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
        SetWindowPos(GetDlgItem(window, id), nullptr, r.left, r.top + extra, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    RECT outer{}; GetWindowRect(window, &outer);
    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top + extra, SWP_NOMOVE | SWP_NOZORDER);
}
static std::wstring text(HWND window, int id) {
    auto control = GetDlgItem(window, id);
    std::wstring value(GetWindowTextLengthW(control) + 1, 0);
    value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
    return value;
}
constexpr UINT MailSetupReady = WM_APP + 3;
struct SetupJob {
    unsigned id;
    std::wstring sender;
    MailSetup result;
    std::atomic_bool cancelled=false, done=false;
};
struct SettingsDialog {
    Settings& settings;
    std::shared_ptr<SetupJob> job;
    std::wstring lastLookup;
    std::wstring appliedSender;
    std::wstring lastHelpHost;
    bool saveAfterLookup=false;
    std::wstring initialPassword;
    bool passwordVisible=false;
    HWND copyToast=nullptr;
    std::wstring layoutKey;
    ~SettingsDialog() { wipe(initialPassword); if(job) job->cancelled=true; }
};
static void advancedLayout(HWND window, bool expanded);
static void updatePasswordHelp(HWND window, SettingsDialog& state) {
    if(SendDlgItemMessageW(window,IDC_METHOD,CB_GETCURSEL,0,0)==1) return;
    auto host=text(window,IDC_HOST);
    state.lastHelpHost=host;
    auto help=passwordHelp(host);
    SetDlgItemTextW(window,IDC_PASSWORD_LABEL,help.label.c_str());
    if(!host.empty()) SetDlgItemTextW(window,IDC_DISCOVERY_STATUS,help.instructions.c_str());
    auto link=L"<a>"+help.linkLabel+L"</a>";
    SetDlgItemTextW(window,IDC_PASSWORD_HELP,link.c_str());
    ShowWindow(GetDlgItem(window,IDC_PASSWORD_HELP),help.url.empty() ? SW_HIDE : SW_SHOW);
    advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED);
}
static bool startLookup(HWND window, SettingsDialog& state) {
    if(SendDlgItemMessageW(window,IDC_METHOD,CB_GETCURSEL,0,0)==1) return false;
    auto sender=text(window,IDC_SENDER);
    if(!validEmailAddress(sender)) return false;
    if(IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED || sender.empty()) return false;
    if(!text(window,IDC_HOST).empty() && sender==state.appliedSender) return false;
    auto at=sender.find(L'@');
    if(at==std::wstring::npos || at==0 || at+1==sender.size()) return false;
    if(state.job && state.job->sender==sender) return true;
    if(sender==state.lastLookup) return false;
    if(state.job) state.job->cancelled=true;
    static std::atomic_uint nextId=0;
    auto job=std::make_shared<SetupJob>(); job->id=++nextId; job->sender=sender;
    state.job=job; state.lastLookup=sender;
    SetDlgItemTextW(window,IDC_HOST,L"");
    updatePasswordHelp(window,state);
    SetDlgItemTextW(window,IDC_DISCOVERY_STATUS,L"Finding your email server...");
    std::thread([job,window,domain=sender.substr(at+1)] {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(SUCCEEDED(com)) {
            job->result=discoverMailSetup(domain);
            CoUninitialize();
        }
        job->done=true;
        if(!job->cancelled) PostMessageW(window,MailSetupReady,job->id,0);
    }).detach();
    return true;
}
static void advancedLayout(HWND window, bool expanded) {
    const bool direct=SendDlgItemMessageW(window,IDC_METHOD,CB_GETCURSEL,0,0)==1;
    ShowWindow(GetDlgItem(window,IDC_METHOD),SW_HIDE);
    CheckDlgButton(window,IDC_METHOD_DIRECT,direct ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(window,IDC_METHOD_PROVIDER,direct ? BST_UNCHECKED : BST_CHECKED);
    const auto methodHelp=direct ? L"No password needed. Some networks block direct sending, and the connection may be unencrypted. If it fails, use your email provider." : L"Send through your own email account. We find the server settings for you.";
    if(text(window,IDC_METHOD_HELP)!=methodHelp) SetDlgItemTextW(window,IDC_METHOD_HELP,methodHelp);
    if(direct) expanded=false;
    for(auto id : {IDC_PASSWORD_LABEL, IDC_PASSWORD, IDC_PASSWORD_SHOW, IDC_PASSWORD_COPY, IDC_ADVANCED})
        ShowWindow(GetDlgItem(window,id),direct ? SW_HIDE : SW_SHOW);
    if(direct) ShowWindow(GetDlgItem(window,IDC_PASSWORD_HELP),SW_HIDE);
    auto place=[&](int id,LONG x,LONG y,LONG width,LONG height) {
        RECT r{x,y,width,height}; MapDialogRect(window,&r);
        SetWindowPos(GetDlgItem(window,id),nullptr,r.left,r.top,r.right,r.bottom,SWP_NOZORDER);
    };
    auto textHeight=[&](int id,LONG width) {
        RECT bounds{0,0,width,1000}; MapDialogRect(window,&bounds); const LONG scale=bounds.bottom; bounds.bottom=0;
        auto control=GetDlgItem(window,id); auto dc=GetDC(control); auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
        auto value=text(window,id); DrawTextW(dc,value.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old); ReleaseDC(control,dc);
        return (std::max)(8L,(bounds.bottom*1000+scale-1)/scale);
    };
    const bool link=!direct && !passwordHelp(text(window,IDC_HOST)).url.empty();
    const LONG hostErrorHeight=text(window,IDC_HOST_ERROR).empty() ? 0 : textHeight(IDC_HOST_ERROR,314)+4;
    const auto layoutKey=std::to_wstring(GetDpiForWindow(window))+L":"+std::to_wstring(direct)+L":"+std::to_wstring(expanded)+L":"+std::to_wstring(link)+L":"+std::to_wstring(textHeight(IDC_METHOD_HELP,388))+L":"+std::to_wstring(textHeight(IDC_DISCOVERY_STATUS,302))+L":"+std::to_wstring(textHeight(IDC_APPROVAL_HELP,388))+L":"+std::to_wstring(hostErrorHeight);
    auto state=reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(state && state->layoutKey==layoutKey) return;
    if(state) state->layoutKey=layoutKey;
    auto previousScroll=GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL);
    if(previousScroll) ScrollWindowEx(window,0,previousScroll,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE);
    SetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL,0,FALSE);
    const LONG methodHeight=textHeight(IDC_METHOD_HELP,388);
    place(IDC_METHOD_HELP,26,111,388,methodHeight);
    const LONG destination=111+methodHeight+22;
    place(IDC_DESTINATION_HEADING,26,destination,388,12);
    place(IDC_KINDLE,26,destination+16,388,20);
    const LONG account=destination+58;
    place(IDC_ACCOUNT_HEADING,26,account,388,12);
    place(IDC_SENDER_LABEL,26,account+23,80,12); place(IDC_SENDER,112,account+18,302,20);
    place(IDC_PASSWORD_LABEL,26,account+49,82,24); place(IDC_PASSWORD,112,account+44,250,20);
    place(IDC_PASSWORD_SHOW,366,account+44,22,20); place(IDC_PASSWORD_COPY,392,account+44,22,20);
    const LONG guidanceHeight=textHeight(IDC_DISCOVERY_STATUS,302);
    place(IDC_DISCOVERY_STATUS,112,account+70,302,guidanceHeight);
    place(IDC_PASSWORD_HELP,112,account+70+guidanceHeight+4,302,14);
    const LONG footer=direct ? account+44 : account+70+guidanceHeight+(link ? 24 : 8);
    const LONG approvalHeight=textHeight(IDC_APPROVAL_HELP,388);
    const LONG advanced=footer+approvalHeight+22;
    ShowWindow(GetDlgItem(window,IDC_DISCOVERY_STATUS),direct ? SW_HIDE : SW_SHOW);
    place(IDC_APPROVAL_HELP,26,footer,388,approvalHeight);
    place(IDC_ADVANCED,26,advanced,388,14);
    place(IDC_HOST_LABEL,26,advanced+26,72,12); place(IDC_PORT_LABEL,324,advanced+26,30,12);
    place(IDC_HOST,100,advanced+21,214,20); place(IDC_PORT,354,advanced+21,60,20);
    place(IDC_HOST_ERROR,100,advanced+43,314,hostErrorHeight);
    ShowWindow(GetDlgItem(window,IDC_HOST_ERROR),expanded && hostErrorHeight ? SW_SHOW : SW_HIDE);
    place(IDC_SECURITY_LABEL,26,advanced+52+hostErrorHeight,72,12); place(IDC_SECURITY,100,advanced+47+hostErrorHeight,314,60);
    for(auto id : {IDC_HOST,IDC_PORT,IDC_SECURITY,IDC_HOST_LABEL,IDC_PORT_LABEL,IDC_SECURITY_LABEL})
        ShowWindow(GetDlgItem(window,id),expanded ? SW_SHOW : SW_HIDE);
    const LONG buttons=direct ? footer+approvalHeight+22 : advanced+(expanded ? 86+hostErrorHeight : 28);
    place(IDOK,276,buttons,70,22); place(IDCANCEL,354,buttons,72,22);
    RECT units{0,0,440,buttons+36}; MapDialogRect(window,&units);
    RECT outer{},client{}; GetWindowRect(window,&outer); GetClientRect(window,&client);
    const int frameHeight=(outer.bottom-outer.top)-client.bottom;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
    const int height=(std::min)(units.bottom+frameHeight,monitor.rcWork.bottom-monitor.rcWork.top-MulDiv(16,GetDpiForWindow(window),96));
    const int top=(std::max)(monitor.rcWork.top,(std::min)(outer.top,monitor.rcWork.bottom-height));
    SetWindowPos(window,nullptr,outer.left,top,outer.right-outer.left,height,SWP_NOZORDER);
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS}; scroll.nMax=units.bottom-1; scroll.nPage=height-frameHeight; scroll.nPos=0;
    SetScrollInfo(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL,&scroll,FALSE);
    InvalidateRect(GetDlgItem(window,IDC_SETTINGS_SCROLL),nullptr,FALSE);
    RECT bar{430,0,438,0}; MapDialogRect(window,&bar);
    SetWindowPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),nullptr,bar.left,0,bar.right-bar.left,height-frameHeight,SWP_NOZORDER);
    ShowWindow(GetDlgItem(window,IDC_SETTINGS_SCROLL),static_cast<int>(scroll.nPage)<units.bottom ? SW_SHOW : SW_HIDE);
    InvalidateRect(window,nullptr,FALSE);
}
static void paintSettingsPanels(HWND window) {
    PAINTSTRUCT paint{}; auto target=BeginPaint(window,&paint); RECT client{}; GetClientRect(window,&client);
    auto dc=CreateCompatibleDC(target); auto bitmap=CreateCompatibleBitmap(target,client.right,client.bottom);
    auto previousBitmap=SelectObject(dc,bitmap); FillRect(dc,&client,dialogBackground(window));
    auto pen=CreatePen(PS_SOLID,1,usesDarkTheme(window) ? RGB(65,65,65) : GetSysColor(COLOR_3DSHADOW));
    auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,panelBackground(window));
    auto panel=[&](RECT r) { RoundRect(dc,r.left,r.top,r.right,r.bottom,MulDiv(12,GetDpiForWindow(window),96),MulDiv(12,GetDpiForWindow(window),96)); };
    auto section=[&](int first,int last) {
        RECT r{14,0,426,0}; MapDialogRect(window,&r); RECT start{},end{};
        GetWindowRect(GetDlgItem(window,first),&start); GetWindowRect(GetDlgItem(window,last),&end);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&start),2); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&end),2);
        r.top=start.top-MulDiv(12,GetDpiForWindow(window),96); r.bottom=end.bottom+MulDiv(12,GetDpiForWindow(window),96); panel(r); return r;
    };
    section(IDC_DELIVERY_HEADING,IDC_METHOD_HELP); section(IDC_DESTINATION_HEADING,IDC_KINDLE);
    auto account=section(IDC_ACCOUNT_HEADING,IDC_APPROVAL_HELP);
    if(IsWindowVisible(GetDlgItem(window,IDC_ADVANCED))) {
        RECT advanced{}; GetWindowRect(GetDlgItem(window,IDC_ADVANCED),&advanced); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&advanced),2);
        advanced.left=account.left; advanced.right=account.right; advanced.top-=MulDiv(12,GetDpiForWindow(window),96);
        RECT button{}; GetWindowRect(GetDlgItem(window,IDOK),&button); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);
        advanced.bottom=button.top-MulDiv(16,GetDpiForWindow(window),96); panel(advanced);
    }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen);
    BitBlt(target,paint.rcPaint.left,paint.rcPaint.top,paint.rcPaint.right-paint.rcPaint.left,paint.rcPaint.bottom-paint.rcPaint.top,dc,paint.rcPaint.left,paint.rcPaint.top,SRCCOPY);
    SelectObject(dc,previousBitmap); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window,&paint);
}
static void scrollSettings(HWND window,int position) {
    const int old=GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL);
    SetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL,position,FALSE);
    const int next=GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL);
    if(old!=next) {
        auto bar=GetDlgItem(window,IDC_SETTINGS_SCROLL); RECT r{}; GetWindowRect(bar,&r); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
        ScrollWindowEx(window,0,old-next,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
        SetWindowPos(bar,nullptr,r.left,r.top,0,0,SWP_NOSIZE|SWP_NOZORDER); InvalidateRect(bar,nullptr,FALSE);
    }
}
static LRESULT CALLBACK copyToastProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint); RECT r{}; GetClientRect(window,&r);
        auto owner=GetWindow(window,GW_OWNER); const bool dark=usesDarkTheme(owner);
        auto brush=CreateSolidBrush(dark ? RGB(48,48,48) : GetSysColor(COLOR_WINDOW)); FillRect(dc,&r,brush); DeleteObject(brush);
        auto pen=CreatePen(PS_SOLID,1,dark ? RGB(90,90,90) : GetSysColor(COLOR_3DSHADOW)); auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        const int radius=MulDiv(10,GetDpiForWindow(owner),96); RoundRect(dc,0,0,r.right,r.bottom,radius,radius);
        auto oldFont=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(owner,WM_GETFONT,0,0))); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,dark ? RGB(240,240,240) : GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc,L"Password copied",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,oldFont); SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
static INT_PTR CALLBACK settingsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if(message!=WM_INITDIALOG && !state) return FALSE;
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<SettingsDialog*>(lparam); SetWindowLongPtrW(window, DWLP_USER, lparam);
            setBookIcon(window);
            const auto& s = state->settings;
            SendDlgItemMessageW(window,IDC_METHOD,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Send through my email provider"));
            SendDlgItemMessageW(window,IDC_METHOD,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Send directly (no password)"));
            SendDlgItemMessageW(window,IDC_METHOD,CB_SETCURSEL,s.direct ? 1 : 0,0);
            state->appliedSender=s.sender;
            SetDlgItemTextW(window, IDC_KINDLE, s.kindle.c_str()); SetDlgItemTextW(window, IDC_SENDER, s.sender.c_str());
            SetDlgItemTextW(window, IDC_HOST, s.host.c_str()); SetDlgItemInt(window, IDC_PORT, s.port, FALSE);
            SetDlgItemTextW(window, IDC_DISCOVERY_STATUS, s.host.empty() ? L"Your email server will be found automatically." : L"Using your saved email server settings.");
            updatePasswordHelp(window,*state);
            for (auto id : {IDC_KINDLE, IDC_SENDER, IDC_HOST}) SendDlgItemMessageW(window, id, EM_SETLIMITTEXT, 254, 0);
            SendDlgItemMessageW(window, IDC_PASSWORD, EM_SETLIMITTEXT, 1280, 0);
            SetDlgItemTextW(window,IDC_PASSWORD,state->initialPassword.c_str()); wipe(state->initialPassword);
            SendDlgItemMessageW(window, IDC_PORT, EM_SETLIMITTEXT, 5, 0);
            SendDlgItemMessageW(window, IDC_SECURITY, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TLS (usually port 465)"));
            SendDlgItemMessageW(window, IDC_SECURITY, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"STARTTLS (usually port 587)"));
            SendDlgItemMessageW(window, IDC_SECURITY, CB_SETCURSEL, s.startTls ? 1 : 0, 0);
            attachContentScrollbar(window,GetDlgItem(window,IDC_SETTINGS_SCROLL));
            advancedLayout(window, false);
            SendDlgItemMessageW(window,IDC_KINDLE,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Your Kindle email address"));
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_SENDER && HIWORD(wparam)==EN_CHANGE) {
            SetDlgItemTextW(window,IDC_PASSWORD,L""); return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_PASSWORD_SHOW) {
            state->passwordVisible=!state->passwordVisible;
            SendDlgItemMessageW(window,IDC_PASSWORD,EM_SETPASSWORDCHAR,state->passwordVisible ? 0 : L'\x25CF',0);
            SetDlgItemTextW(window,IDC_PASSWORD_SHOW,state->passwordVisible ? L"Hide password" : L"Show password");
            InvalidateRect(GetDlgItem(window,IDC_PASSWORD),nullptr,TRUE); return TRUE;
        }
        if(message==WM_TIMER && wparam==95) {
            KillTimer(window,95);
            if(state->copyToast) { DestroyWindow(state->copyToast); state->copyToast=nullptr; }
            return TRUE;
        }
        if(message==WM_DESTROY && state->copyToast) { DestroyWindow(state->copyToast); state->copyToast=nullptr; }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_PASSWORD_COPY) {
            auto value=text(window,IDC_PASSWORD);
            auto memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
            if(!memory) { wipe(value); throw std::runtime_error("Could not copy the password."); }
            auto data=GlobalLock(memory);
            if(!data) { GlobalFree(memory); wipe(value); throw std::runtime_error("Could not copy the password."); }
            memcpy(data,value.c_str(),(value.size()+1)*sizeof(wchar_t)); GlobalUnlock(memory); wipe(value);
            bool copied=false;
            if(OpenClipboard(window)) { if(EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory)) copied=true; CloseClipboard(); }
            if(!copied) { if(auto bytes=GlobalLock(memory)) { SecureZeroMemory(bytes,GlobalSize(memory)); GlobalUnlock(memory); } GlobalFree(memory); error(window,L"Could not copy the password. Try again."); }
            if(copied) {
                if(state->copyToast) DestroyWindow(state->copyToast);
                static const auto toastClass=[] { WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpfnWndProc=copyToastProc; cls.lpszClassName=L"VolturaBooks.CopyToast"; return RegisterClassW(&cls); }();
                RECT button{}; GetWindowRect(GetDlgItem(window,IDC_PASSWORD_COPY),&button);
                const auto dpi=GetDpiForWindow(window); const int width=MulDiv(160,dpi,96),height=MulDiv(36,dpi,96);
                state->copyToast=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,MAKEINTATOM(toastClass),L"Password copied",WS_POPUP,button.right-width,button.bottom+MulDiv(6,dpi,96),width,height,window,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(state->copyToast) {
                    const int radius=MulDiv(10,dpi,96); auto region=CreateRoundRectRgn(0,0,width+1,height+1,radius,radius);
                    if(!SetWindowRgn(state->copyToast,region,FALSE)) DeleteObject(region);
                    ShowWindow(state->copyToast,SW_SHOWNOACTIVATE); SetTimer(window,95,2500,nullptr);
                }
            }
            return TRUE;
        }
        if(message==WM_ERASEBKGND) return TRUE;
        if(message==WM_PAINT) { paintSettingsPanels(window); return TRUE; }
        if(message==ContentScroll) { scrollSettings(window,static_cast<int>(wparam)); return TRUE; }
        if(message==WM_VSCROLL) {
            SCROLLINFO scroll{sizeof(scroll),SIF_ALL}; GetScrollInfo(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL,&scroll); int next=scroll.nPos;
            const int line=MulDiv(24,GetDpiForWindow(window),96);
            switch(LOWORD(wparam)) {
                case SB_LINEUP: next-=line; break; case SB_LINEDOWN: next+=line; break;
                case SB_PAGEUP: next-=scroll.nPage; break; case SB_PAGEDOWN: next+=scroll.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: next=scroll.nTrackPos; break;
                case SB_TOP: next=scroll.nMin; break; case SB_BOTTOM: next=scroll.nMax; break;
            }
            scrollSettings(window,next); return TRUE;
        }
        if(message==WM_MOUSEWHEEL) { scrollSettings(window,GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL)-GET_WHEEL_DELTA_WPARAM(wparam)*MulDiv(48,GetDpiForWindow(window),96)/WHEEL_DELTA); return TRUE; }
        if(message==WM_COMMAND && (HIWORD(wparam)==EN_SETFOCUS || HIWORD(wparam)==BN_SETFOCUS || HIWORD(wparam)==CBN_SETFOCUS)) {
            RECT control{},client{}; GetWindowRect(reinterpret_cast<HWND>(lparam),&control); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&control),2); GetClientRect(window,&client);
            if(control.bottom>client.bottom) scrollSettings(window,GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL)+control.bottom-client.bottom+8);
            else if(control.top<0) scrollSettings(window,GetScrollPos(GetDlgItem(window,IDC_SETTINGS_SCROLL),SB_CTL)+control.top-8);
        }
        if(message==WM_DPICHANGED) PostMessageW(window,WM_APP+92,0,0);
        if(message==WM_APP+92) { advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED); return TRUE; }
        if(message==WM_COMMAND && (LOWORD(wparam)==IDC_METHOD_DIRECT || LOWORD(wparam)==IDC_METHOD_PROVIDER)) {
            SendDlgItemMessageW(window,IDC_METHOD,CB_SETCURSEL,LOWORD(wparam)==IDC_METHOD_DIRECT ? 1 : 0,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_METHOD,CBN_SELCHANGE),0); return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) { EndDialog(window, IDCANCEL); return TRUE; }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_METHOD && HIWORD(wparam)==CBN_SELCHANGE) {
            if(state->job) state->job->cancelled=true;
            state->job.reset(); state->lastLookup.clear(); state->saveAfterLookup=false;
            advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED);
            if(SendDlgItemMessageW(window,IDC_METHOD,CB_GETCURSEL,0,0)==0) {
                SetDlgItemTextW(window,IDC_DISCOVERY_STATUS,L"Your email server will be found automatically.");
                updatePasswordHelp(window,*state); startLookup(window,*state);
            }
            return TRUE;
        }
        if (message == MailSetupReady && state->job && wparam==state->job->id && state->job->done) {
            auto job=std::move(state->job); // Only unhandled lookups remain pending.
            if(text(window,IDC_SENDER)!=job->sender || IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED) { state->saveAfterLookup=false; return TRUE; }
            const auto& setup=job->result;
            bool save=state->saveAfterLookup; state->saveAfterLookup=false;
            if(setup.host.empty()) {
                SetDlgItemTextW(window,IDC_DISCOVERY_STATUS,setup.guidance.empty() ? L"Server not found. Enter the settings supplied by your email provider below." : setup.guidance.c_str());
                CheckDlgButton(window,IDC_ADVANCED,BST_CHECKED); advancedLayout(window,true);
            } else {
                SetDlgItemTextW(window,IDC_HOST,setup.host.c_str()); SetDlgItemInt(window,IDC_PORT,setup.port,FALSE);
                state->appliedSender=job->sender;
                SendDlgItemMessageW(window,IDC_SECURITY,CB_SETCURSEL,setup.startTls ? 1 : 0,0);
                updatePasswordHelp(window,*state);
                if(save) PostMessageW(window,WM_COMMAND,IDOK,0);
            }
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_SENDER && HIWORD(wparam)==EN_KILLFOCUS) { startLookup(window,*state); return TRUE; }
        if(message==WM_HOST_INPUT_REJECTED) {
            SetDlgItemTextW(window,IDC_HOST_ERROR,wparam==IDC_PORT ? L"Port accepts numbers only, from 1 to 65535." : L"Enter a hostname or IP address. Only letters, numbers, dots, hyphens and IPv6 colons are accepted.");
            advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED); return TRUE;
        }
        if(message==WM_COMMAND && (LOWORD(wparam)==IDC_HOST || LOWORD(wparam)==IDC_PORT) && HIWORD(wparam)==EN_CHANGE) {
            if(!text(window,IDC_HOST_ERROR).empty()) { SetDlgItemTextW(window,IDC_HOST_ERROR,L""); advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED); }
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_HOST && HIWORD(wparam)==EN_KILLFOCUS) {
            auto host=text(window,IDC_HOST);
            if(host!=state->lastHelpHost) {
                updatePasswordHelp(window,*state);
                Settings check; check.kindle=L"reader@kindle.com"; check.sender=L"sender@example.com"; check.host=host;
                if(auto issue=validate(check); !issue.empty()) { SetDlgItemTextW(window,IDC_HOST_ERROR,issue.c_str()); advancedLayout(window,IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED); }
            }
            return TRUE;
        }
        if(message==WM_NOTIFY) {
            auto notification=reinterpret_cast<NMHDR*>(lparam);
            if(notification->idFrom==IDC_PASSWORD_HELP && (notification->code==NM_CLICK || notification->code==NM_RETURN)) {
                auto help=passwordHelp(text(window,IDC_HOST));
                if(!help.url.empty()) ShellExecuteW(window,L"open",help.url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                return TRUE;
            }
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_ADVANCED) {
            bool show = IsDlgButtonChecked(window, IDC_ADVANCED) == BST_CHECKED;
            advancedLayout(window, show);
            if(show) {
                if(state->job) state->job->cancelled=true;
                state->job.reset(); state->lastLookup.clear(); state->saveAfterLookup=false;
                SetDlgItemTextW(window,IDC_DISCOVERY_STATUS,L"Use the server settings supplied by your email provider.");
            } else startLookup(window,*state);
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
            if(!validEmailAddress(text(window,IDC_KINDLE))) { error(window,L"Check your Kindle email address. Use an address such as name@kindle.com."); SetFocus(GetDlgItem(window,IDC_KINDLE)); return TRUE; }
            if(!validEmailAddress(text(window,IDC_SENDER))) { error(window,L"Check your sender email address. Use an address such as name@example.com."); SetFocus(GetDlgItem(window,IDC_SENDER)); return TRUE; }
            if(startLookup(window,*state)) { state->saveAfterLookup=true; return TRUE; }
            Settings s;
            s.direct = SendDlgItemMessageW(window,IDC_METHOD,CB_GETCURSEL,0,0)==1;
            s.kindle = text(window, IDC_KINDLE); s.sender = text(window, IDC_SENDER); s.host = text(window, IDC_HOST);
            s.port = static_cast<int>(GetDlgItemInt(window, IDC_PORT, nullptr, FALSE));
            s.startTls = SendDlgItemMessageW(window, IDC_SECURITY, CB_GETCURSEL, 0, 0) == 1;
            if (auto validation = validate(s); !validation.empty()) {
                Settings hostCheck=s; hostCheck.port=465;
                if(!validate(hostCheck).empty()) {
                    CheckDlgButton(window,IDC_ADVANCED,BST_CHECKED); SetDlgItemTextW(window,IDC_HOST_ERROR,validation.c_str()); advancedLayout(window,true); SetFocus(GetDlgItem(window,IDC_HOST));
                } else error(window,validation);
                return TRUE;
            }
            auto password = s.direct ? std::wstring{} : text(window, IDC_PASSWORD);
            try { saveSettings(s, password); } catch (...) { wipe(password); throw; }
            wipe(password); SetDlgItemTextW(window, IDC_PASSWORD, L"");
            state->settings = s; EndDialog(window, IDOK); return TRUE;
        }
    } catch (const std::exception& e) { error(window, wide(e.what())); }
    return FALSE;
}
bool showSettings(HWND owner, Settings& settings) {
    SettingsDialog state{settings};
    state.initialPassword=loadPassword();
    auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner, settingsProc, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Could not open settings.");
    return result == IDOK;
}
constexpr UINT SendComplete = WM_APP + 1;
constexpr UINT_PTR PreviewTimer = 2;
struct SendPreview {
    std::atomic_bool done=false;
    HBITMAP cover=nullptr;
    BookDetails details;
    ~SendPreview() { if(cover) DeleteObject(cover); }
};
struct SendDialog {
    bool simulated=false;
    Settings settings;
    fs::path path;
    std::atomic_bool cancel = false;
    std::thread worker;
    Result result;
    size_t bookNumber = 1, bookCount = 1;
    bool busy = false;
    bool historySaved = false;
    std::shared_ptr<SendPreview> preview;
    HBITMAP cover = nullptr;
    HICON typeIcon = nullptr;
    bool coverReady = false;
    BookDetails details;
    ~SendDialog() {
        cancel = true;
        if (worker.joinable()) worker.join();
        if (cover) DeleteObject(cover);
        if (typeIcon) DestroyIcon(typeIcon);
    }
};
static bool beginSend(HWND window, SendDialog& state) {
    if(state.simulated) {
        state.cancel=false; state.busy=true;
        SetWindowTextW(window,L"Voltura Books - Test sending (no emails sent)");
        SetDlgItemTextW(window,IDC_HEADING,L"Testing sending");
        auto progress=state.bookCount>1 ? L"Testing sending book "+std::to_wstring(state.bookNumber)+L" of "+std::to_wstring(state.bookCount)+L"..." : L"Testing sending...";
        SetDlgItemTextW(window,IDC_STATUS,progress.c_str());
        SetDlgItemTextW(window,IDC_SEND_HELP,L"Simulation only. No email will be sent.");
        for(auto id:{IDC_SETTINGS,IDC_RETRY,IDC_SKIP}) EnableWindow(GetDlgItem(window,id),FALSE);
        SendDlgItemMessageW(window,IDC_PROGRESS,PBM_SETMARQUEE,TRUE,25);
        state.worker=std::thread([window,&state] {
            for(int i=0;i<80 && !state.cancel;++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            state.result={ !state.cancel, state.cancel ? L"Sending test cancelled. No email was sent." : L"Sending test complete. No email was sent." };
            PostMessageW(window,SendComplete,0,0);
        });
        return true;
    }
    auto identity = std::make_unique<BookIdentity>(state.path);
    const auto history = localData() / L"sent-books.tsv";
    if (sentBefore(history, identity->hash, state.settings.kindle)) {
        auto question = L"Book sent before. Send again?\n\n" + state.path.filename().wstring();
        if(state.bookCount>1) question+=L"\n\nYes: send again. No: skip this book. Cancel: stop the remaining books.";
        auto answer=themedMessageBox(window, question.c_str(), L"Voltura Books", (state.bookCount>1 ? MB_YESNOCANCEL : MB_YESNO) | MB_ICONQUESTION | MB_DEFBUTTON2);
        if(answer!=IDYES) { EndDialog(window,answer==IDNO ? IDIGNORE : IDCANCEL); return false; }
    }
    auto password = state.settings.direct ? std::wstring{} : loadPassword();
    state.cancel = false; state.busy = true; state.historySaved = false;
    auto progress=state.bookCount>1 ? L"Sending book "+std::to_wstring(state.bookNumber)+L" of "+std::to_wstring(state.bookCount)+L"..." : L"Sending your book...";
    SetDlgItemTextW(window, IDC_STATUS, progress.c_str());
    SetDlgItemTextW(window,IDC_HEADING,L"Sending to Kindle");
    SetDlgItemTextW(window,IDC_SEND_HELP,L"Sending by email. Amazon handles delivery to your Kindle.");
    EnableWindow(GetDlgItem(window,IDC_SKIP),FALSE);
    SetDlgItemTextW(window, IDCANCEL, L"Cancel"); EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
    EnableWindow(GetDlgItem(window, IDC_SETTINGS), FALSE); EnableWindow(GetDlgItem(window, IDC_RETRY), FALSE);
    ShowWindow(GetDlgItem(window, IDC_PROGRESS), SW_SHOW);
    SendDlgItemMessageW(window, IDC_PROGRESS, PBM_SETMARQUEE, TRUE, 25);
    state.worker = std::thread([window, &state, history, identity = std::move(identity), secret = std::move(password)]() mutable {
        state.result = sendBook(state.settings, secret, state.path, state.cancel);
        if (state.result.success) {
            try {
                recordSent(history, identity->hash, state.settings.kindle, state.path.filename().wstring());
                state.historySaved = true;
            } catch (...) {
                state.result.message = L"Email sent, but sent history could not be saved. Duplicate warnings may miss this book.";
            }
        }
        wipe(secret); PostMessageW(window, SendComplete, 0, 0);
    });
    return true;
}
static INT_PTR CALLBACK sendProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto state = reinterpret_cast<SendDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<SendDialog*>(lparam); SetWindowLongPtrW(window, DWLP_USER, lparam);
            state->typeIcon=fileTypeIcon(state->path);
            setBookIcon(window);
            fitFilename(window,state->path,state->details);
            RECT box{}; GetClientRect(GetDlgItem(window, IDC_COVER), &box);
            auto preview=state->preview=std::make_shared<SendPreview>();
            // The worker owns only its result: closing the dialog never waits for
            // preview I/O and cannot leave a worker holding dialog/window pointers.
            std::thread([preview,path=state->path,box] {
                const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                if(SUCCEEDED(com)) {
                    try {
                        preview->cover=loadCover(path,box.right,box.bottom);
                        preview->details=loadBookDetails(path);
                    } catch(...) { /* A preview failure must not interrupt sending. */ }
                    CoUninitialize();
                }
                preview->done.store(true,std::memory_order_release);
            }).detach();
            SetTimer(window,PreviewTimer,100,nullptr);
            ShowWindow(GetDlgItem(window,IDC_SKIP),state->bookCount>1 ? SW_SHOW : SW_HIDE);
            beginSend(window, *state);
            return TRUE;
        }
        if (message == WM_TIMER && wparam==PreviewTimer) {
            if(!state->preview || !state->preview->done.load(std::memory_order_acquire)) return TRUE;
            KillTimer(window,PreviewTimer);
            state->cover=state->preview->cover; state->preview->cover=nullptr;
            state->details=std::move(state->preview->details);
            state->preview.reset();
            state->coverReady = true;
            fitFilename(window,state->path,state->details);
            InvalidateRect(GetDlgItem(window, IDC_COVER), nullptr, TRUE);
            return TRUE;
        }
        if(message==WM_DRAWITEM && wparam==IDC_FILENAME && state) {
            auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            FillRect(draw->hDC,&draw->rcItem,panelBackground(window));
            auto dc=draw->hDC; SendMessageW(window,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(draw->hwndItem));
            drawBookInformation(dc,draw->rcItem,reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem,WM_GETFONT,0,0)),state->path,state->details,true,true,state->typeIcon,true);
            return TRUE;
        }
        if (message == WM_DRAWITEM && wparam == IDC_COVER) {
            auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            FillRect(draw->hDC, &draw->rcItem, panelBackground(window));
            drawBookCover(draw->hDC,draw->rcItem,state && state->coverReady ? state->cover : nullptr,state ? state->path.extension().wstring() : L"");
            return TRUE;
        }
        if (message == SendComplete) {
            if (state->worker.joinable()) state->worker.join(); state->busy = false;
            ShowWindow(GetDlgItem(window, IDC_PROGRESS), SW_HIDE);
            SetDlgItemTextW(window, IDC_STATUS, state->result.message.c_str());
            SetDlgItemTextW(window,IDC_HEADING,state->simulated ? L"Sending test complete" : state->result.success ? L"Email sent" : L"Sending needs attention");
            SetDlgItemTextW(window,IDC_SEND_HELP,state->simulated ? L"No emails sent. Your sent-book history is unchanged." : state->result.success ? L"Kindle delivery may take a few minutes." : L"Check the message above before trying again.");
            SetDlgItemTextW(window, IDCANCEL, L"Close"); EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
            EnableWindow(GetDlgItem(window, IDC_SETTINGS), !state->result.success && !state->simulated);
            EnableWindow(GetDlgItem(window, IDC_RETRY), !state->result.success && !state->simulated);
            EnableWindow(GetDlgItem(window,IDC_SKIP),!state->result.success);
            if(state->bookCount>1 && state->cancel) { EndDialog(window,IDCANCEL); return TRUE; }
            if (state->result.success && (state->simulated || state->historySaved || state->bookCount>1)) SetTimer(window, 1, state->bookCount>1 ? 200 : 3000, nullptr);
            return TRUE;
        }
        if (message == WM_TIMER && wparam==1) { if(!confirmingIncoming) EndDialog(window, IDOK); return TRUE; }
        if(message==WM_COMMAND && LOWORD(wparam)==IDC_SKIP && !state->busy && !state->result.success) { EndDialog(window,IDABORT); return TRUE; }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_SETTINGS && !state->busy) { showSettings(window, state->settings); return TRUE; }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_RETRY && !state->busy) { beginSend(window, *state); return TRUE; }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (state->busy) {
                state->cancel = true; SetDlgItemTextW(window, IDC_STATUS, L"Cancelling... Waiting for the connection to close.");
                EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
            } else EndDialog(window, IDCANCEL);
            return TRUE;
        }
    } catch (const std::exception& e) { error(window, wide(e.what())); if (state && !state->worker.joinable()) EndDialog(window, IDCANCEL); }
    return FALSE;
}
static INT_PTR CALLBACK dropProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto path=reinterpret_cast<std::vector<fs::path>*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG) {
            dropWindow=window;
            SetWindowLongPtrW(window,DWLP_USER,lp); setBookIcon(window); DragAcceptFiles(window,TRUE);
            CheckDlgButton(window,IDC_PREVIEW_MODE,previewOnly ? BST_CHECKED : BST_UNCHECKED);
            if(previewOnly) { SetWindowTextW(window,L"Voltura Books - Test sending (no emails sent)"); SetDlgItemTextW(window,IDC_APPROVAL_HELP,L"Simulates sending. No emails are sent and no history is saved."); }
            SetFocus(GetDlgItem(window,IDC_CHOOSE_BOOK)); return FALSE;
        }
        if(message==WM_DESTROY) dropWindow=nullptr;
        if(message==BrowseRequest) {
            if(!IsWindowEnabled(window))return TRUE;
            CheckDlgButton(window,IDC_PREVIEW_MODE,wp ? BST_CHECKED : BST_UNCHECKED);
            SendMessageW(window,WM_COMMAND,IDC_PREVIEW_MODE,0);
            auto folder=pendingBrowseFolder;pendingBrowseFolder.clear();
            if(folder.empty())SendMessageW(window,WM_COMMAND,IDC_BROWSE_FOLDER,0);
            else {
                auto selection=browseBooks(window,folder,previewOnly,true);
                if(!selection.empty()){*path=std::move(selection);EndDialog(window,IDOK);}
                else EndDialog(window,IDCANCEL);
            }
            return TRUE;
        }
        if(message==IncomingFiles && path && !pendingFiles.empty()) {
            *path=std::move(pendingFiles); pendingFiles.clear(); EndDialog(window,IDOK); return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_PREVIEW_MODE) {
            previewOnly=IsDlgButtonChecked(window,IDC_PREVIEW_MODE)==BST_CHECKED;
            SetWindowTextW(window,previewOnly ? L"Voltura Books - Test sending (no emails sent)" : L"Voltura Books - Send a book");
            SetDlgItemTextW(window,IDC_APPROVAL_HELP,previewOnly ? L"Simulates sending. No emails are sent and no history is saved." : L"One email per book. Up to 50 MB per file."); return TRUE;
        }
        auto accept=[&](const fs::path& candidate) {
            if(auto validation=validateFile(candidate); !validation.empty()) { error(window,validation); return; }
            *path={candidate}; EndDialog(window,IDOK);
        };
        if(message==WM_DROPFILES) {
            *path=droppedBooks(reinterpret_cast<HDROP>(wp)); EndDialog(window,IDOK);
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_BROWSE_FOLDER) {
            auto selection=browseBooks(window,{},previewOnly);
            if(!selection.empty()) { *path=std::move(selection); EndDialog(window,IDOK); }
            else EndDialog(window,IDCANCEL);
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_CHOOSE_BOOK) {
            std::wstring selected(1024*1024,0); OPENFILENAMEW picker{sizeof(picker)};
            picker.hwndOwner=window; picker.lpstrFile=selected.data(); picker.nMaxFile=static_cast<DWORD>(selected.size());
            picker.lpstrFilter=FileFilter; picker.lpstrTitle=L"Choose books to send";
            picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER|OFN_ALLOWMULTISELECT;
            if(GetOpenFileNameW(&picker)) {
                const wchar_t* next=selected.c_str()+wcslen(selected.c_str())+1;
                if(!*next) accept(selected.c_str());
                else {
                    std::vector<fs::path> choices;
                    while(*next) {
                        auto candidate=fs::path(selected.c_str())/next;
                        if(auto validation=validateFile(candidate); !validation.empty()) { error(window,validation); return TRUE; }
                        choices.push_back(candidate); next+=wcslen(next)+1;
                    }
                    *path=std::move(choices); EndDialog(window,IDOK);
                }
            }
            else if(CommDlgExtendedError()) error(window,L"Could not open the file picker. You can drop a book into this window instead.");
            return TRUE;
        }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_ABOUT) { if(showAbout(window)) { pendingFiles.clear(); EndDialog(window,IDCANCEL); } return TRUE; }
        if(message==WM_COMMAND && LOWORD(wp)==IDC_SETTINGS) { auto settings=loadSettings(); showSettings(window,settings); return TRUE; }
        if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) { EndDialog(window,IDCANCEL); return TRUE; }
    } catch(const std::exception& e) { error(window,wide(e.what())); }
    return FALSE;
}
static int sendSelectedBooks(std::vector<fs::path> paths) {
    if(paths.empty()) return 0;
    struct QueueGuard { QueueGuard(){sendingQueue=true;} ~QueueGuard(){sendingQueue=false;} } guard;
    for(const auto& path:paths) if(auto validation=validateFile(path); !validation.empty()) { error(nullptr,path.filename().wstring()+L": "+validation); return 1; }
    auto settings=previewOnly ? Settings{} : loadSettings(); auto password=previewOnly || settings.direct ? std::wstring{} : loadPassword();
    bool ready=(settings.direct || !password.empty()) && validate(settings).empty(); wipe(password);
    if(!previewOnly && !ready && !showSettings(nullptr,settings)) return 1;
    size_t sent=0,skipped=0,failed=0,processed=0; bool historyWarning=false;
    for(size_t index=0;index<paths.size();++index) {
        auto path=paths[index];
        SendDialog state; state.simulated=previewOnly; state.path=path; state.settings=settings; state.bookNumber=processed+1; state.bookCount=paths.size();
        auto dialog=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SEND),nullptr,sendProc,reinterpret_cast<LPARAM>(&state));
        if(dialog==-1) throw std::runtime_error("Could not open sending progress.");
        settings=state.settings; ++processed;
        if(state.result.success) { ++sent; historyWarning|=!state.simulated && !state.historySaved; }
        else if(dialog==IDIGNORE || dialog==IDABORT) ++skipped;
        else ++failed;
        paths.insert(paths.end(),pendingFiles.begin(),pendingFiles.end()); pendingFiles.clear();
        if(dialog==IDCANCEL) break;
    }
    if(paths.size()>1) {
        if(previewOnly) { auto summary=std::to_wstring(sent)+L" sending tests completed. No emails were sent and no history was saved."; themedMessageBox(nullptr,summary.c_str(),L"Voltura Books - Sending tests complete",MB_OK|MB_ICONINFORMATION); return 0; }
        auto summary=std::to_wstring(sent)+L" email(s) sent.\n"+std::to_wstring(skipped)+L" book(s) skipped.\n"+
            std::to_wstring(failed)+L" book(s) not confirmed sent.\n"+std::to_wstring(paths.size()-processed)+L" book(s) not started.\n\nKindle delivery may take a few minutes.";
        if(failed) summary+=L"\nCheck your Kindle before retrying an uncertain submission.";
        if(historyWarning) summary+=L"\nSome sent history could not be saved; duplicate reminders may miss these books.";
        themedMessageBox(nullptr,summary.c_str(),L"Voltura Books - Sending complete",MB_OK|MB_ICONINFORMATION);
    }
    return failed ? 1 : 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace books;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS}; InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { error(nullptr, L"Could not initialize the network library."); return 1; }
    int argc=0; auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!args) { curl_global_cleanup(); if(SUCCEEDED(com)) CoUninitialize(); return 1; }
    std::vector<std::wstring> argv(args,args+argc); LocalFree(args);
    std::vector<fs::path> launchFiles;
    fs::path launchFolder;
    const bool browse=argc>=2 && argv[1]==L"--browse";
    try {
        if(browse && !(argc==2 || (argc==3 && (argv[2]==L"--test-sending" || !argv[2].empty()))))
            throw std::runtime_error("Use --browse, --browse \"folder\", or --browse --test-sending.");
        if(argc>=3 && argv[1]==L"--send") for(int i=2;i<argc;++i) launchFiles.emplace_back(argv[i]);
        else if(argc>=2 && argv[1]==L"--shell") launchFiles=receiveShellSelection();
        else if(browse && argc==3 && argv[2]!=L"--test-sending") {
            launchFolder=argv[2];
            if(auto validation=browseFolderError(launchFolder);!validation.empty())throw std::runtime_error(utf8(validation));
        }
    } catch(const std::exception& e) { error(nullptr,wide(e.what())); curl_global_cleanup(); if(SUCCEEDED(com)) CoUninitialize(); return 1; }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VolturaBooks.Application");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        bool delivered=false,modeConflict=false,browseBusy=false;
        const bool maintenance=argc==2 && (argv[1]==L"--install" || argv[1]==L"--uninstall");
        if(mutex && !maintenance) {
            HWND existing=nullptr;
            for(int i=0;i<40 && !existing;++i) { existing=FindWindowExW(HWND_MESSAGE,nullptr,InstanceClass,nullptr); if(!existing) Sleep(50); }
            if(existing) {
                DWORD process=0; GetWindowThreadProcessId(existing,&process); AllowSetForegroundWindow(process);
                std::wstring payload;
                for(const auto& file:launchFiles) { payload+=file.wstring(); payload+=L'\0'; } payload+=L'\0';
                if(payload.size()*sizeof(wchar_t)<=1024*1024) {
                    COPYDATASTRUCT data{1,static_cast<DWORD>(payload.size()*sizeof(wchar_t)),payload.data()}; DWORD_PTR reply=0;
                    DWORD testSending=argc==3 && argv[2]==L"--test-sending";
                    auto folderText=launchFolder.wstring();
                    if(!launchFolder.empty())data={3,static_cast<DWORD>((folderText.size()+1)*sizeof(wchar_t)),folderText.data()};
                    else if(browse)data={2,sizeof(testSending),&testSending};
                    const bool responded=SendMessageTimeoutW(existing,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&reply)!=0;
                    modeConflict=responded && browse && reply==2;
                    browseBusy=responded && !launchFolder.empty() && reply==3;
                    delivered=responded && reply==TRUE;
                }
            }
        }
        if(!delivered) error(nullptr,browseBusy ? L"Voltura Books is busy with another operation. Finish or close it, then browse the folder again." : modeConflict ? L"Voltura Books is already open in a different sending mode. Close it before opening Browse books in the requested mode." : maintenance ? L"Close Voltura Books before installing or removing it." : L"The open Voltura Books window is not responding. Close it and try again.");
        if (mutex) CloseHandle(mutex); curl_global_cleanup(); if (SUCCEEDED(com)) CoUninitialize(); return delivered ? 0 : 1;
    }
    WNDCLASSW instanceType{}; instanceType.lpfnWndProc=instanceProc; instanceType.hInstance=GetModuleHandleW(nullptr); instanceType.lpszClassName=InstanceClass;
    RegisterClassW(&instanceType);
    auto instanceWindow=CreateWindowExW(0,InstanceClass,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instanceType.hInstance,nullptr);
    int result = 0;
    try {
        if(!instanceWindow) throw std::runtime_error("Could not initialize the application window.");
        if (argc == 2 && argv[1] == L"--install") installApp();
        else if (argc == 2 && argv[1] == L"--uninstall") uninstallApp();
        else if (argc == 2 && argv[1] == L"--settings") {
            auto s=loadSettings(); showSettings(nullptr,s);
        } else if((argc==2 && argv[1]==L"--browse") || (argc==3 && argv[1]==L"--browse")) {
            previewOnly=argc==3 && argv[2]==L"--test-sending";
            result=sendSelectedBooks(browseBooks(nullptr,launchFolder,previewOnly,!launchFolder.empty()));
        } else if(argc==1 || (argc==2 && (argv[1]==L"--drop" || argv[1]==L"--test-sending"))) {
            previewOnly=argc==2 && argv[1]==L"--test-sending";
            std::vector<fs::path> path;
            auto dialog=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_DROP),nullptr,dropProc,reinterpret_cast<LPARAM>(&path));
            if(dialog==-1) throw std::runtime_error("Could not open the book window.");
            if(dialog==IDOK) result=sendSelectedBooks(path);
        } else if(argc>=3 && argv[1]==L"--send") {
            result=sendSelectedBooks(launchFiles);
        } else if(argc>=2 && argv[1]==L"--shell") result=sendSelectedBooks(launchFiles);
        else throw std::runtime_error("Use --send \"book.epub\", --drop, --browse [\"folder\"|--test-sending], --test-sending, --settings, --install, or --uninstall. You can select several books.");
        while(!pendingFiles.empty()) { auto next=std::move(pendingFiles); pendingFiles.clear(); result=sendSelectedBooks(std::move(next)); }

    } catch (const std::exception& e) { error(nullptr, wide(e.what())); result = 1; }
    if(instanceWindow) DestroyWindow(instanceWindow);
    ReleaseMutex(mutex); CloseHandle(mutex); curl_global_cleanup(); if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

