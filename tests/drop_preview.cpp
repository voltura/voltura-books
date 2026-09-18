#define wWinMain unusedApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    std::vector<books::fs::path> paths;
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_DROP),nullptr,books::dropProc,reinterpret_cast<LPARAM>(&paths));
    CoUninitialize(); return 0; // Preview only: never sends selected files.
}
