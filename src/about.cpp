#include "about.h"
#include "update.h"
#include "theme.h"
#include "resource.h"
#include "books_version.h"
#include <shellapi.h>
#include <atomic>
#include <memory>
#include <thread>

namespace books {
namespace {
struct Check { UpdateResult result; std::atomic<bool> done{false}; };
struct About { std::shared_ptr<Check> check; bool checkOnOpen=false; };
void open(HWND window,const wchar_t* url) {
    if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",url,nullptr,nullptr,SW_SHOWNORMAL))<=32)
        themedMessageBox(window,L"Could not open the link. Please try again.",L"Voltura Books",MB_OK|MB_ICONERROR);
}
void showCheckProgress(HWND window) {
    SetDlgItemTextW(window,IDC_UPDATE_STATUS,L"Checking and downloading updates...");
    EnableWindow(GetDlgItem(window,IDC_UPDATE_CHECK),FALSE);
    EnableWindow(GetDlgItem(window,IDC_UPDATE_OPEN),FALSE);
    SetTimer(window,1,200,nullptr);
}
void showCheckResult(HWND window,const Check& check) {
    SetDlgItemTextW(window,IDC_UPDATE_STATUS,check.result.message.c_str());
    EnableWindow(GetDlgItem(window,IDC_UPDATE_CHECK),TRUE);
    EnableWindow(GetDlgItem(window,IDC_UPDATE_OPEN),!check.result.installer.empty());
}
void startCheck(HWND window,About& state) {
    if(state.check && !state.check->done.load()) { showCheckProgress(window); return; }
    try {
        auto check=std::make_shared<Check>();
        std::thread([check]{check->result=downloadUpdate(); check->done.store(true);}).detach();
        state.check=check;
        showCheckProgress(window);
    } catch(...) {
        SetDlgItemTextW(window,IDC_UPDATE_STATUS,L"Could not start the update check. Please try again.");
        EnableWindow(GetDlgItem(window,IDC_UPDATE_CHECK),TRUE);
    }
}
INT_PTR CALLBACK aboutProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto state=reinterpret_cast<About*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        SetWindowLongPtrW(window,DWLP_USER,lp); applyTheme(window);
        SetDlgItemTextW(window,IDC_HEADING,L"Voltura Books " BOOKS_VERSION);
        SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_BOOK))));
        state=reinterpret_cast<About*>(lp);
        EnableWindow(GetDlgItem(window,IDC_UPDATE_OPEN),FALSE);
        if(state->checkOnOpen) startCheck(window,*state);
        else if(state->check) {
            if(state->check->done.load()) showCheckResult(window,*state->check);
            else showCheckProgress(window);
        }
        return TRUE;
    }
    if(!state) return FALSE;
    if(message==WM_COMMAND) {
        switch(LOWORD(wp)) {
        case IDC_UPDATE_CHECK:
            startCheck(window,*state);
            return TRUE;
        case IDC_UPDATE_OPEN:
            if(state->check && state->check->done.load() && !state->check->result.installer.empty()) {
                try {
                    auto& result=state->check->result;
                    if(!updateFileMatches(result.installer,result.sha256)) throw std::runtime_error("Invalid installer");
                    if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",result.installer.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32) throw std::runtime_error("Installer launch failed");
                    EndDialog(window,IDOK);
                } catch(...) { SetDlgItemTextW(window,IDC_UPDATE_STATUS,L"Could not start the verified installer. Check for updates again."); EnableWindow(GetDlgItem(window,IDC_UPDATE_OPEN),FALSE); }
            }
            return TRUE;
        case IDC_ABOUT_WEBSITE: open(window,L"https://voltura.github.io/voltura-books/"); return TRUE;
        case IDC_ABOUT_ISSUES: open(window,L"https://github.com/voltura/voltura-books/issues/new?template=bug_report.yml"); return TRUE;
        case IDC_ABOUT_LICENSE: open(window,L"https://github.com/voltura/voltura-books/blob/main/LICENSE"); return TRUE;
        case IDC_ABOUT_SUPPORT: open(window,L"https://www.paypal.com/donate?hosted_button_id=7PN65YXN64DBG"); return TRUE;
        case IDC_ABOUT_COFFEE: open(window,L"https://ko-fi.com/G2G74W5F8"); return TRUE;
        case IDCANCEL: EndDialog(window,IDCANCEL); return TRUE;
        }
    }
    if(message==WM_TIMER && wp==1 && state->check && state->check->done.load()) {
        KillTimer(window,1); showCheckResult(window,*state->check); return TRUE;
    }
    if(message==WM_CLOSE) { EndDialog(window,IDCANCEL); return TRUE; }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint);
        FillRect(dc,&paint.rcPaint,dialogBackground(window));
        auto brush=SelectObject(dc,panelBackground(window));
        auto pen=CreatePen(PS_SOLID,1,usesDarkTheme(window)?RGB(65,65,65):GetSysColor(COLOR_3DSHADOW));
        auto oldPen=SelectObject(dc,pen);
        for(auto box:{RECT{10,10,380,94},RECT{10,101,380,180}}) {
            MapDialogRect(window,&box); auto radius=MulDiv(12,GetDpiForWindow(window),96);
            RoundRect(dc,box.left,box.top,box.right,box.bottom,radius,radius);
        }
        SelectObject(dc,oldPen); SelectObject(dc,brush); DeleteObject(pen); EndPaint(window,&paint); return TRUE;
    }
    if(message==WM_DESTROY) KillTimer(window,1);
    return FALSE;
}
}
bool showAbout(HWND owner,bool checkForUpdates) {
    static About state;
    state.checkOnOpen=checkForUpdates;
    return DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_ABOUT),owner,aboutProc,reinterpret_cast<LPARAM>(&state))==IDOK;
}
}
