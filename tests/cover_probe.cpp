#include "cover.h"
#include <objbase.h>
#include <iostream>
int wmain(int argc, wchar_t** argv) {
    if(argc!=2) return 2;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    HBITMAP cover=books::loadCover(argv[1],160,220);
    if(cover) {
        BITMAP bitmap{}; GetObjectW(cover,sizeof(bitmap),&bitmap);
        std::cout<<bitmap.bmWidth<<"x"<<bitmap.bmHeight<<"\n";
        DeleteObject(cover);
    }
    CoUninitialize(); return cover ? 0 : 1;
}
