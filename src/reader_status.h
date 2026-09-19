#pragma once
#include "reader_loading.h"
#include "theme.h"
#include <commctrl.h>
#include <cmath>

namespace books {
// Owned popup above both native content and the WebView child HWND.
class ReaderStatus {
    HWND window{},owner{},cover{};
    unsigned frame=0;
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto self=reinterpret_cast<ReaderStatus*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<ReaderStatus*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,wp,lp);
        if(message==WM_NCHITTEST)return HTTRANSPARENT;
        if(message==WM_TIMER){++self->frame;InvalidateRect(window,nullptr,FALSE);return 0;}
        if(message==WM_PAINT){self->paint();return 0;}
        if(message==WM_ERASEBKGND)return 1;
        return DefWindowProcW(window,message,wp,lp);
    }
    void paint(){
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        bool high=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0,dark=usesDarkTheme(owner);
        COLORREF background=high?GetSysColor(COLOR_WINDOW):dark?RGB(40,40,40):RGB(250,250,250);
        COLORREF foreground=high?GetSysColor(COLOR_WINDOWTEXT):dark?RGB(245,245,245):RGB(30,30,30);
        auto brush=CreateSolidBrush(background);FillRect(dc,&rect,brush);DeleteObject(brush);
        UINT dpi=GetDpiForWindow(owner);int cx=MulDiv(28,dpi,96),cy=rect.bottom/2,radius=MulDiv(10,dpi,96);
        auto pen=CreatePen(PS_SOLID,(std::max)(1,MulDiv(3,dpi,96)),foreground);auto oldPen=SelectObject(dc,pen);
        for(int n=0;n<9;++n){double angle=(n+frame%12)*3.141592653589793/6;int x=static_cast<int>(std::cos(angle)*radius),y=static_cast<int>(std::sin(angle)*radius);MoveToEx(dc,cx+x,cy+y,nullptr);LineTo(dc,cx+x*13/10,cy+y*13/10);}
        SelectObject(dc,oldPen);DeleteObject(pen);
        auto font=CreateFontW(-MulDiv(16,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");auto oldFont=SelectObject(dc,font);
        wchar_t text[80]{};GetWindowTextW(window,text,80);rect.left=MulDiv(52,dpi,96);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,foreground);DrawTextW(dc,text,-1,&rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,oldFont);DeleteObject(font);EndPaint(window,&paint);
    }
public:
    void init(HWND dialog,HWND preview){
        owner=dialog;cover=preview;
        WNDCLASSW type{};type.lpfnWndProc=proc;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"VolturaBooksReaderStatus";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        RegisterClassW(&type);
        // An owned, non-activating popup has its own native surface above the
        // WebView child HWND. A layered child can lose its custom composition
        // when WebView2 creates or resizes its controller.
        window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,type.lpszClassName,L"",WS_POPUP,0,0,0,0,dialog,nullptr,type.hInstance,this);
    }
    void close(){if(window)DestroyWindow(window);window=nullptr;}
    void hide(){if(window){KillTimer(window,1);ShowWindow(window,SW_HIDE);}}
    void position(){if(!window)return;RECT r{};GetWindowRect(cover,&r);int width=(std::min)(static_cast<int>(r.right-r.left),MulDiv(260,GetDpiForWindow(owner),96)),height=MulDiv(64,GetDpiForWindow(owner),96);SetWindowPos(window,HWND_TOP,r.left+(r.right-r.left-width)/2,r.top+(r.bottom-r.top-height)/2,width,height,SWP_NOACTIVATE);}
    void show(const wchar_t* text){
        // Owned popups do not inherit their owner's visibility like children do.
        if(!IsWindowVisible(owner)||IsIconic(owner)){hide();return;}
        bool wasVisible=(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)!=0;
        SetWindowTextW(window,text);position();ShowWindow(window,SW_SHOWNOACTIVATE);SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        // Progress must remain perceptible while work is active. The broad
        // ClientAreaAnimation desktop preference is also disabled by some
        // Windows configurations that have not requested reduced motion, so
        // it must not turn a progress spinner into an apparently frozen icon.
        if(!wasVisible){SetTimer(window,1,80,nullptr);NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,window,OBJID_CLIENT,CHILDID_SELF);UpdateWindow(window);}
    }
    HWND handle()const{return window;}
    unsigned animationFrame()const{return frame;}
};
}
