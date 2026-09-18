// Run the production queue and dialogs with deterministic mail/history adapters.
#include "core.h"
#include "history.h"
#include "theme.h"
#include "resource.h"
#include "cover.h"
#include <atomic>
#include <vector>
#include <fstream>
#include <iostream>
namespace books {
std::atomic_bool blockCover=false, coverEntered=false, coverExited=false;
HBITMAP fixtureCover(const fs::path&,int,int) noexcept {
    if(blockCover.load()) { coverEntered=true; while(blockCover.load()) Sleep(1); coverExited=true; }
    return nullptr;
}
int configurationReads=0;
Settings fixtureSettings() { ++configurationReads; Settings s; s.direct=true; s.sender=L"sender@example.com"; s.kindle=L"reader@kindle.com"; return s; }
std::wstring fixturePassword() { ++configurationReads; return {}; }
int scenario=0, calls=0, recorded=0, duplicates=0, summaries=0;
HWND current=nullptr; std::vector<std::wstring> statuses; std::vector<fs::path> submitted;
std::wstring summaryText;
HWND broker=nullptr; fs::path extraFile;
bool fixtureSentBefore(const fs::path&,const std::string&,const std::wstring&) { EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM)->BOOL { if(GetDlgItem(w,IDC_STATUS)) current=w; return TRUE; },0); return (scenario==2 || scenario==3) && duplicates++==0; }
void fixtureRecord(const fs::path&,const std::string&,const std::wstring&,const std::wstring&) { ++recorded; }
int fixtureMessage(HWND,const wchar_t* text,const wchar_t*,UINT flags) {
    if(flags&MB_ICONQUESTION) return scenario==9 ? IDYES : scenario==3 ? IDCANCEL : IDNO;
    ++summaries; summaryText=text; return IDOK;
}
Result fixtureSend(const Settings&,const std::wstring&,const fs::path& path,std::atomic_bool& cancel) {
    wchar_t text[256]{}; GetDlgItemTextW(current,IDC_STATUS,text,256); statuses.emplace_back(text); submitted.push_back(path); ++calls;
    if((scenario==9 || scenario==10) && calls==1) {
        auto payload=extraFile.wstring(); payload+=L'\0'; payload+=L'\0';
        COPYDATASTRUCT data{1,static_cast<DWORD>(payload.size()*sizeof(wchar_t)),payload.data()};
        SendMessageW(broker,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data));
    }
    if(blockCover.load()) while(!coverEntered.load()) Sleep(1);
    if(scenario==6 && calls==1) { PostMessageW(current,WM_COMMAND,IDCANCEL,0); while(!cancel) Sleep(1); return {false,L"Cancelled"}; }
    if((scenario==4 || scenario==5) && calls==1) return {false,L"Fixture failure"};
    return {true,L"Email sent."};
}
}
#define loadCover fixtureCover
#define loadSettings fixtureSettings
#define loadPassword fixturePassword
#define sentBefore fixtureSentBefore
#define recordSent fixtureRecord
#define themedMessageBox fixtureMessage
#define sendBook fixtureSend
#define wWinMain unusedApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#undef loadCover
#undef sendBook
#undef themedMessageBox
#undef recordSent
#undef sentBefore
#undef loadPassword
#undef loadSettings
static LRESULT CALLBACK observe(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto result=DefSubclassProc(w,m,wp,lp);
    if(m==books::SendComplete && books::scenario==8) PostMessageW(w,WM_COMMAND,IDCANCEL,0);
    if(m==books::SendComplete && books::calls==1) {
        if(books::scenario==4) PostMessageW(w,WM_COMMAND,IDC_RETRY,0);
        if(books::scenario==5) PostMessageW(w,WM_COMMAND,IDC_SKIP,0);
    }
    return result;
}
static LRESULT CALLBACK hook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_ACTIVATE) {
        books::current=reinterpret_cast<HWND>(wp);
        SetWindowSubclass(books::current,observe,99,0);
        if(books::scenario==8) PostMessageW(books::current,WM_COMMAND,IDCANCEL,0);
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
static bool settingsLayoutPassed=true;
static INT_PTR CALLBACK settingsCheck(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_APP+91) {
        settingsLayoutPassed &= books::text(window,IDC_PASSWORD)==L"test-password";
        settingsLayoutPassed &= SendDlgItemMessageW(window,IDC_PASSWORD,EM_GETPASSWORDCHAR,0,0)!=0;
        SendDlgItemMessageW(window,IDC_PASSWORD_SHOW,BM_CLICK,0,0);
        settingsLayoutPassed &= SendDlgItemMessageW(window,IDC_PASSWORD,EM_GETPASSWORDCHAR,0,0)==0;
        SendDlgItemMessageW(window,IDC_PASSWORD_SHOW,BM_CLICK,0,0);
        settingsLayoutPassed &= SendDlgItemMessageW(window,IDC_PASSWORD,EM_GETPASSWORDCHAR,0,0)!=0;
        SetDlgItemTextW(window,IDC_PASSWORD,L"");
        settingsLayoutPassed &= books::text(window,IDC_PASSWORD).empty();
        SendDlgItemMessageW(window,IDC_METHOD_DIRECT,BM_CLICK,0,0);
        settingsLayoutPassed &= IsDlgButtonChecked(window,IDC_METHOD_DIRECT)==BST_CHECKED && !IsWindowVisible(GetDlgItem(window,IDC_PASSWORD));
        SendDlgItemMessageW(window,IDC_METHOD_PROVIDER,BM_CLICK,0,0);
        settingsLayoutPassed &= IsDlgButtonChecked(window,IDC_METHOD_PROVIDER)==BST_CHECKED && IsWindowVisible(GetDlgItem(window,IDC_PASSWORD));
        SendDlgItemMessageW(window,IDC_ADVANCED,BM_CLICK,0,0);
        settingsLayoutPassed &= IsWindowVisible(GetDlgItem(window,IDC_HOST));
        SendDlgItemMessageW(window,IDC_ADVANCED,BM_CLICK,0,0);
        settingsLayoutPassed &= !IsWindowVisible(GetDlgItem(window,IDC_HOST));
        SetDlgItemTextW(window,IDC_PORT,L"465");
        SendDlgItemMessageW(window,IDC_PORT,EM_SETSEL,3,3);
        SendDlgItemMessageW(window,IDC_PORT,WM_CHAR,L'a',0);
        settingsLayoutPassed &= books::text(window,IDC_PORT)==L"465";
        settingsLayoutPassed &= !(GetWindowLongPtrW(GetDlgItem(window,IDC_PORT),GWL_STYLE)&ES_NUMBER);
        SetDlgItemTextW(window,IDC_HOST,L"send.one.com");
        SendDlgItemMessageW(window,IDC_HOST,EM_SETSEL,12,12);
        SendDlgItemMessageW(window,IDC_HOST,WM_CHAR,L'#',0);
        SendDlgItemMessageW(window,IDC_HOST,WM_CHAR,L'§',0);
        settingsLayoutPassed &= books::text(window,IDC_HOST)==L"send.one.com";
        SendDlgItemMessageW(window,IDC_HOST,WM_CHAR,L'a',0);
        settingsLayoutPassed &= books::text(window,IDC_HOST)==L"send.one.coma";
        SendMessageW(window,WM_HOST_INPUT_REJECTED,0,0);
        settingsLayoutPassed &= !books::text(window,IDC_HOST_ERROR).empty();
        settingsLayoutPassed &= books::text(window,IDC_DISCOVERY_STATUS).find(L"Paste a server")==std::wstring::npos;
        auto state=reinterpret_cast<books::SettingsDialog*>(GetWindowLongPtrW(window,DWLP_USER));
        SetDlgItemTextW(window,IDC_SENDER,L"sender@example.com");
        SetDlgItemTextW(window,IDC_HOST,L"");
        CheckDlgButton(window,IDC_ADVANCED,BST_UNCHECKED);
        state->job=std::make_shared<books::SetupJob>(); state->job->id=999;
        state->job->sender=state->lastLookup=L"sender@example.com"; state->job->done=true;
        SendMessageW(window,books::MailSetupReady,999,0);
        settingsLayoutPassed &= !state->job && IsDlgButtonChecked(window,IDC_ADVANCED)==BST_CHECKED;
        CheckDlgButton(window,IDC_ADVANCED,BST_UNCHECKED);
        settingsLayoutPassed &= !books::startLookup(window,*state);
        EndDialog(window,IDCANCEL); return TRUE;
    }
    auto result=books::settingsProc(window,message,wp,lp);
    if(message==WM_INITDIALOG) PostMessageW(window,WM_APP+91,0,0);
    return result;
}
static bool previewPublicationPassed=true;
static INT_PTR CALLBACK previewCheck(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_INITDIALOG) {
        SetWindowLongPtrW(window,DWLP_USER,lp);
        PostMessageW(window,WM_APP+92,0,0); return TRUE;
    }
    if(message==WM_APP+92) {
        auto state=reinterpret_cast<books::SendDialog*>(GetWindowLongPtrW(window,DWLP_USER));
        state->preview=std::make_shared<books::SendPreview>();
        state->preview->details.title=L"Published title";
        books::sendProc(window,WM_TIMER,books::PreviewTimer,0);
        previewPublicationPassed &= state->details.title.empty() && !state->coverReady;
        state->preview->done.store(true,std::memory_order_release);
        books::sendProc(window,WM_TIMER,books::PreviewTimer,0);
        previewPublicationPassed &= state->details.title==L"Published title" && state->coverReady && !state->preview;
        EndDialog(window,IDCANCEL); return TRUE;
    }
    return FALSE;
}
int main() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
    const auto root=books::fs::temp_directory_path()/(L"VolturaBooks-queue-"+std::to_wstring(GetCurrentProcessId())); books::fs::create_directories(root);
    std::vector<books::fs::path> paths{root/L"First.epub",root/L"Second.epub",root/L"Third.epub"};
    for(auto& p:paths) { std::ofstream f(p); f<<"PK fixture"; }
    auto h=SetWindowsHookExW(WH_CBT,hook,nullptr,GetCurrentThreadId()); int failures=0;
    for(int scenario=0;scenario<7;++scenario) {
        books::scenario=scenario; books::calls=books::recorded=books::duplicates=books::summaries=0; books::statuses.clear(); books::submitted.clear();
        books::summaryText.clear();
        auto result=books::sendSelectedBooks(scenario==0 ? std::vector<books::fs::path>{paths[0]} : paths);
        const int expected[]={1,3,2,0,4,3,1};
        bool ok=books::calls==expected[scenario];
        if(scenario==0) ok&=books::statuses[0]==L"Sending your book..." && books::summaries==0;
        if(scenario==1) ok&=books::submitted==paths && books::recorded==3 && books::statuses[0]==L"Sending book 1 of 3..." && books::statuses[2]==L"Sending book 3 of 3...";
        if(scenario==4) ok&=books::submitted[0]==books::submitted[1] && books::recorded==3;
        if(scenario==5) ok&=books::recorded==2 && result==0 && books::summaryText==
            L"2 email(s) sent.\n1 book(s) skipped.\n0 book(s) not confirmed sent.\n0 book(s) not started.\n\nKindle delivery may take a few minutes.";
        if(scenario==6) ok&=books::recorded==0;
        if(!ok) { std::cerr<<"FAIL queue scenario "<<scenario<<"\n"; ++failures; }
    }
    for(int scenario=7;scenario<9;++scenario) {
        books::scenario=scenario; books::calls=books::recorded=books::configurationReads=0;
        books::previewOnly=true;
        auto result=books::sendSelectedBooks({paths[0]});
        if(books::calls || books::recorded || books::configurationReads || result!=(scenario==8 ? 1 : 0)) { std::cerr<<"FAIL preview scenario "<<scenario<<"\n"; ++failures; }
    }
    books::previewOnly=false;
    // A stalled preview must not hold up cancellation or retain dialog state.
    books::scenario=6; books::calls=books::recorded=0; books::blockCover=true;
    auto watchdog=std::thread([] { for(int i=0;i<300 && books::blockCover;++i) Sleep(10); books::blockCover=false; });
    books::sendSelectedBooks({paths[0],paths[1]});
    if(!books::blockCover || !books::coverEntered) { std::cerr<<"FAIL cancellation waited for preview\n"; ++failures; }
    books::blockCover=false; watchdog.join();
    while(!books::coverExited) Sleep(1);
    WNDCLASSW cls{}; cls.lpfnWndProc=books::instanceProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"VolturaBooks.TestInstance";
    RegisterClassW(&cls); books::broker=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    books::extraFile=paths[1];
    for(int scenario=9;scenario<=10;++scenario) {
        books::scenario=scenario; books::calls=books::recorded=0; books::submitted.clear();
        books::sendSelectedBooks({paths[0]});
        const auto expected=scenario==9 ? std::vector<books::fs::path>{paths[0],paths[1]} : std::vector<books::fs::path>{paths[0]};
        if(books::submitted!=expected || !books::pendingFiles.empty()) { std::cerr<<"FAIL incoming files scenario "<<scenario<<"\n"; ++failures; }
    }
    DestroyWindow(books::broker);
    UnhookWindowsHookEx(h); h=nullptr;
    books::Settings setup; books::SettingsDialog setupState{setup}; setupState.initialPassword=L"test-password";
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),nullptr,settingsCheck,reinterpret_cast<LPARAM>(&setupState));
    if(!settingsLayoutPassed) { std::cerr<<"FAIL settings delivery cards or advanced settings\n"; ++failures; }
    books::SendDialog previewState; previewState.path=paths[0];
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SEND),nullptr,previewCheck,reinterpret_cast<LPARAM>(&previewState));
    if(!previewPublicationPassed) { std::cerr<<"FAIL preview publication\n"; ++failures; }
    UnhookWindowsHookEx(h); for(auto& p:paths) books::fs::remove(p); books::fs::remove(root); CoUninitialize();
    std::cout<<"Queue scenarios: "<<11-failures<<"/11 passed\n"; return failures ? 1 : 0;
}
