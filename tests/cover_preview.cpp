// Preview the production resources and rendering without entering the send flow.
#define wWinMain unusedApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
static bool presentation = false;
static INT_PTR CALLBACK preview(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if(message==WM_INITDIALOG) {
        auto state=reinterpret_cast<books::SendDialog*>(lp);
        SetWindowLongPtrW(window,DWLP_USER,lp);
        books::setBookIcon(window);
        state->typeIcon=books::fileTypeIcon(state->path);
        state->details=books::loadBookDetails(state->path);
        books::fitFilename(window,state->path,state->details);
        RECT box{}; GetClientRect(GetDlgItem(window,IDC_COVER),&box);
        state->cover=books::loadCover(state->path,box.right,box.bottom);
        state->coverReady=true;
        SetWindowTextW(window,L"Voltura Books - Cover preview (no email)");
        SetDlgItemTextW(window,IDC_STATUS,L"Cover preview. No email is sent.");
        ShowWindow(GetDlgItem(window,IDC_PROGRESS),SW_HIDE);
        EnableWindow(GetDlgItem(window,IDC_SETTINGS),FALSE);
        EnableWindow(GetDlgItem(window,IDC_RETRY),FALSE);
        ShowWindow(GetDlgItem(window,IDC_SKIP),SW_HIDE);
        SetDlgItemTextW(window,IDCANCEL,L"Close");
        if(presentation) {
            SetWindowTextW(window,L"Voltura Books - Send to Kindle");
            SetDlgItemTextW(window,IDC_STATUS,L"Sending your book...");
            ShowWindow(GetDlgItem(window,IDC_PROGRESS),SW_SHOW);
            SendDlgItemMessageW(window,IDC_PROGRESS,PBM_SETMARQUEE,TRUE,25);
            SetDlgItemTextW(window,IDCANCEL,L"Cancel");
        }
        SetTimer(window,1,120000,nullptr);
        return TRUE;
    }
    return books::sendProc(window,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    int argc=0; auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc!=2 && argc!=3) { LocalFree(args); CoUninitialize(); return 2; }
    presentation=argc==3 && std::wstring(args[2])==L"--presentation";
    books::SendDialog state; state.path=args[1]; LocalFree(args);
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SEND),nullptr,preview,reinterpret_cast<LPARAM>(&state));
    CoUninitialize(); return 0;
}
