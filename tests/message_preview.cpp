#include "theme.h"
#include <commctrl.h>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    books::themedMessageBox(nullptr,L"Voltura Books is already open. Finish the current send or close Settings, then try again.",L"Voltura Books",MB_OK|MB_ICONINFORMATION);
    return 0;
}
