#include "about.h"
#include <commctrl.h>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    books::showAbout(nullptr);
}
