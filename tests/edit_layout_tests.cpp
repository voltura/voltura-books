#include "interactive_test.h"
// Verify real edit-control geometry without changing Windows theme preferences.
#include "../src/theme.cpp"
#include <iostream>
#include <vector>
int main() {
    if(!interactiveTestsEnabled())return 77;
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
    // Shared input modality and checkbox painting, without modifying OS settings.
    auto check=CreateWindowExW(0,L"BUTTON",L"Don't send emails",WS_CHILD|BS_AUTOCHECKBOX|WS_TABSTOP,0,0,500,40,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(check,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);
    auto keyboardFocus=GetFocus();
    books::updateFocusCues(check,true);
    if(!books::focusCuesVisible(check)||GetFocus()!=keyboardFocus)return 2;
    books::updateFocusCues(check,false);
    if(books::focusCuesVisible(check)!=books::alwaysShowCues()||GetFocus()!=keyboardFocus)return 3;
    MSG input{};PostMessageW(check,WM_KEYDOWN,VK_TAB,0);PeekMessageW(&input,check,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE);
    if(!books::focusCuesVisible(check))return 4;
    PostMessageW(check,WM_LBUTTONDOWN,0,0);PeekMessageW(&input,check,WM_LBUTTONDOWN,WM_LBUTTONDOWN,PM_REMOVE);
    if(books::focusCuesVisible(check)!=books::alwaysShowCues())return 5;
    auto screen=GetDC(parent),dc=CreateCompatibleDC(screen);auto bitmap=CreateCompatibleBitmap(screen,500,40);auto oldBitmap=SelectObject(dc,bitmap);
    for(bool dark:{false,true}) {
        books::theme(parent)->dark=dark;
        NMCUSTOMDRAW draw{};draw.hdc=dc;draw.hdr.hwndFrom=check;
        books::updateFocusCues(check,true);books::paintButton(&draw,*books::theme(parent));
        std::vector<COLORREF> unfocused;for(int y=0;y<40;++y)for(int x=0;x<500;++x)unfocused.push_back(GetPixel(dc,x,y));
        draw.uItemState=CDIS_FOCUS;books::paintButton(&draw,*books::theme(parent));
        int changed=0;for(int y=0;y<40;++y)for(int x=0;x<500;++x)if(GetPixel(dc,x,y)!=unfocused[y*500+x]){++changed;if(x>=MulDiv(14,GetDpiForWindow(check),96)){std::cerr<<"Checkbox focus escaped the square\n";return 6;}}
        if(!changed){std::cerr<<"Missing checkbox keyboard cue\n";return 7;}
        books::updateFocusCues(check,false);books::paintButton(&draw,*books::theme(parent));
        if(!books::alwaysShowCues())for(int y=0;y<40;++y)for(int x=0;x<500;++x)if(GetPixel(dc,x,y)!=unfocused[y*500+x])return 8;
    }
    SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(parent,screen);
    SendMessageW(check,BM_CLICK,0,0);if(SendMessageW(check,BM_GETCHECK,0,0)!=BST_CHECKED)return 9;
    auto second=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);books::applyTheme(second);
    DestroyWindow(parent);if(!books::focusInputHook)return 10;
    DestroyWindow(second);if(books::focusInputHook||books::themedWindows)return 11;
    std::cout<<checks<<" edit geometry checks passed (light/dark, 100-300% font and control sizes).\n";
}
