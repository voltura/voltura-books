#include "list_scrollbar.h"
#include "theme.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
namespace books {
namespace {
struct Scroll { HWND list; int grab=0; bool dragging=false; };
int visibleRows(HWND list) { RECT r{}; GetClientRect(list,&r); int height=static_cast<int>(SendMessageW(list,LB_GETITEMHEIGHT,0,0)); return (std::max)(1,static_cast<int>(r.bottom)/(std::max)(1,height)); }
void range(HWND bar,HWND list,int& count,int& page,int& top) {
    if(list==GetParent(bar)) { SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(bar,SB_CTL,&info); count=info.nMax+1; page=info.nPage; top=info.nPos; }
    else { count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0)); page=visibleRows(list); top=static_cast<int>(SendMessageW(list,LB_GETTOPINDEX,0,0)); }
}
RECT thumb(HWND bar,HWND list) {
    RECT r{}; GetClientRect(bar,&r);
    int count,page,top; range(bar,list,count,page,top);
    if(count<=page) return {};
    int size=(std::max)(static_cast<int>(r.right)*2,static_cast<int>(r.bottom)*page/count);
    size=(std::min)(size,static_cast<int>(r.bottom));
    int y=(static_cast<int>(r.bottom)-size)*top/(count-page);
    int inset=(std::max)(2,static_cast<int>(r.right)/4);
    return {inset,y,r.right-inset,y+size};
}
void move(HWND bar,HWND list,int top) {
    if(list==GetParent(bar)) { SendMessageW(list,ContentScroll,top,0); InvalidateRect(bar,nullptr,FALSE); return; }
    int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
    top=std::clamp(top,0,(std::max)(0,count-visibleRows(list)));
    SendMessageW(list,LB_SETTOPINDEX,top,0); updateListScrollbar(list,bar);
}
LRESULT CALLBACK barProc(HWND bar,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto state=reinterpret_cast<Scroll*>(data);
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(bar,&paint); RECT r{}; GetClientRect(bar,&r);
        FillRect(dc,&r,panelBackground(GetParent(bar)));
        auto t=thumb(bar,state->list);
        if(t.bottom>t.top) {
            bool dark=usesDarkTheme(GetParent(bar)); auto color=dark ? (state->dragging ? RGB(190,190,190) : RGB(120,120,120)) : GetSysColor(COLOR_3DSHADOW);
            auto brush=CreateSolidBrush(color); auto old=SelectObject(dc,brush); auto pen=SelectObject(dc,GetStockObject(NULL_PEN));
            RoundRect(dc,t.left,t.top,t.right,t.bottom,t.right-t.left,t.right-t.left);
            SelectObject(dc,old); SelectObject(dc,pen); DeleteObject(brush);
        }
        EndPaint(bar,&paint); return 0;
    }
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_LBUTTONDOWN) {
        auto t=thumb(bar,state->list); int y=GET_Y_LPARAM(lp);
        if(t.bottom<=t.top) return 0;
        if(y>=t.top && y<t.bottom) { state->dragging=true; state->grab=y-t.top; SetCapture(bar); }
        else { int count,page,top; range(bar,state->list,count,page,top); move(bar,state->list,top+(y<t.top ? -page : page)); }
        InvalidateRect(bar,nullptr,FALSE); return 0;
    }
    if(message==WM_MOUSEMOVE && state->dragging) {
        RECT r{}; GetClientRect(bar,&r); auto t=thumb(bar,state->list);
        int travel=static_cast<int>(r.bottom)-(t.bottom-t.top);
        int count,page,top; range(bar,state->list,count,page,top);
        if(travel>0) move(bar,state->list,MulDiv(GET_Y_LPARAM(lp)-state->grab,count-page,travel));
        return 0;
    }
    if(message==WM_LBUTTONUP || message==WM_CAPTURECHANGED) { state->dragging=false; if(GetCapture()==bar) ReleaseCapture(); InvalidateRect(bar,nullptr,FALSE); return 0; }
    if(message==WM_MOUSEWHEEL) return SendMessageW(state->list,message,wp,lp);
    if(message==WM_NCDESTROY) { RemoveWindowSubclass(bar,barProc,1); delete state; }
    return DefSubclassProc(bar,message,wp,lp);
}
LRESULT CALLBACK listProc(HWND list,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto result=DefSubclassProc(list,message,wp,lp);
    if(message==WM_PAINT || message==WM_SIZE || message==WM_KEYDOWN || message==WM_MOUSEWHEEL || message==LB_SETTOPINDEX || message==LB_RESETCONTENT || message==LB_ADDSTRING)
        updateListScrollbar(list,reinterpret_cast<HWND>(data));
    return result;
}
}
void updateListScrollbar(HWND list,HWND bar) {
    if(!IsWindow(bar)) return;
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
    int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
    info.nMax=(std::max)(0,count-1); info.nPage=visibleRows(list); info.nPos=static_cast<int>(SendMessageW(list,LB_GETTOPINDEX,0,0));
    SetScrollInfo(bar,SB_CTL,&info,FALSE); InvalidateRect(bar,nullptr,FALSE);
}
void attachListScrollbar(HWND list,HWND bar) {
    SetWindowSubclass(list,listProc,1,reinterpret_cast<DWORD_PTR>(bar));
    SetWindowSubclass(bar,barProc,1,reinterpret_cast<DWORD_PTR>(new Scroll{list})); updateListScrollbar(list,bar);
}
void attachContentScrollbar(HWND owner,HWND bar) { SetWindowSubclass(bar,barProc,1,reinterpret_cast<DWORD_PTR>(new Scroll{owner})); }
}
