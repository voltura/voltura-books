// Exercise the real setup dialog without persisting settings or credentials.
#define wWinMain unusedApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
static INT_PTR CALLBACK preview(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_COMMAND && LOWORD(wp)==IDOK) { EndDialog(window,IDCANCEL); return TRUE; }
    if(message==WM_TIMER) { EndDialog(window,IDCANCEL); return TRUE; }
    auto result=books::settingsProc(window,message,wp,lp);
    if(message==WM_INITDIALOG) {
        SetWindowTextW(window,L"Voltura Books - Setup preview (not saved)");
        EnableWindow(GetDlgItem(window,IDOK),FALSE);
        books::startLookup(window,*reinterpret_cast<books::SettingsDialog*>(lp));
        SetTimer(window,1,120000,nullptr);
        if(!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) EndDialog(window,2);
    }
    return result;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES | ICC_LINK_CLASS}; InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    books::Settings settings; settings.sender=argc>1 ? argv[1] : L"example@one.com"; LocalFree(argv);
    books::SettingsDialog state{settings};
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),nullptr,preview,reinterpret_cast<LPARAM>(&state));
    CoUninitialize(); return 0;
}
