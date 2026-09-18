#include "browser.h"
#include <commctrl.h>
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(count==2) books::browseBooks(nullptr,args[1]); // Preview only; selected files are never sent.
    LocalFree(args); CoUninitialize(); return 0;
}
