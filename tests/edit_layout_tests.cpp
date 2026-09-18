// Verify real edit-control geometry without changing Windows theme preferences.
#include "../src/theme.cpp"
#include <iostream>
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    HWND parent=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HWND edit=CreateWindowExW(0,L"EDIT",L"Example text",WS_CHILD|ES_AUTOHSCROLL,0,0,400,40,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    books::applyTheme(parent);
    int checks=0;
    for(bool dark : {false,true}) {
        books::theme(parent)->dark=dark;
        for(int scale : {100,125,150,175,200,250,300}) {
            HFONT font=CreateFontW(-MulDiv(16,scale,100),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
            SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
            for(int height : {MulDiv(32,scale,100),MulDiv(40,scale,100)}) {
                SetWindowPos(edit,nullptr,0,0,400,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
                RECT outer{},client{}; GetWindowRect(edit,&outer); GetClientRect(edit,&client);
                POINT origin{}; ClientToScreen(edit,&origin);
                HDC dc=GetDC(edit); auto previous=SelectObject(dc,font); TEXTMETRICW metric{}; GetTextMetricsW(dc,&metric); SelectObject(dc,previous); ReleaseDC(edit,dc);
                int top=origin.y-outer.top, bottom=outer.bottom-origin.y-client.bottom;
                if(abs(top-bottom)>1 || client.bottom!=metric.tmHeight) { std::cerr<<"Edit alignment failed at "<<scale<<"%, dark="<<dark<<"\n"; return 1; }
                ++checks;
            }
            SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);
            DeleteObject(font);
        }
    }
    DestroyWindow(parent);
    std::cout<<checks<<" edit geometry checks passed (light/dark, 100-300% font and control sizes).\n";
}
