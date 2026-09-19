#include "list_scrollbar.h"
#include "theme.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
namespace books {
namespace {
struct Scroll { HWND list; int grab=0; bool dragging=false; HWND themeOwner=nullptr; int wheelRemainder=0; };
bool grid(HWND list){wchar_t name[32]{};GetClassNameW(list,name,32);return lstrcmpiW(name,WC_LISTVIEWW)==0;}
int visibleRows(HWND list) { RECT r{}; GetClientRect(list,&r); int height=static_cast<int>(SendMessageW(list,LB_GETITEMHEIGHT,0,0)); return (std::max)(1,static_cast<int>(r.bottom)/(std::max)(1,height)); }
void range(HWND bar,HWND list,int& count,int& page,int& top) {
    if(list==GetParent(bar)) { SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(bar,SB_CTL,&info); count=info.nMax+1; page=info.nPage; top=info.nPos; }
    else if(grid(list)){RECT view{},client{};POINT origin{};ListView_GetViewRect(list,&view);ListView_GetOrigin(list,&origin);GetClientRect(list,&client);count=(std::max)(0L,view.bottom+origin.y);page=client.bottom;top=origin.y;}
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
    if(grid(list)){int count,page,current;range(bar,list,count,page,current);top=std::clamp(top,0,(std::max)(0,count-page));ListView_Scroll(list,0,top-current);updateListScrollbar(list,bar);return;}
    int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
    top=std::clamp(top,0,(std::max)(0,count-visibleRows(list)));
    SendMessageW(list,LB_SETTOPINDEX,top,0); updateListScrollbar(list,bar);
}
void wheel(HWND bar,Scroll& state,int delta) {
    UINT lines=3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    int count,page,top;range(bar,state.list,count,page,top);
    if(!lines || count<=page) { state.wheelRemainder=0; return; }
    // List boxes scroll in items; icon views scroll in pixels. Use the list's
    // text-line height for tile wheel steps, not the much taller cover height.
    int line=1;
    if(grid(state.list)) {
        auto dc=GetDC(state.list);auto font=reinterpret_cast<HFONT>(SendMessageW(state.list,WM_GETFONT,0,0));
        auto previous=font?SelectObject(dc,font):nullptr;TEXTMETRICW metrics{};
        GetTextMetricsW(dc,&metrics);line=(std::max)(1,static_cast<int>(metrics.tmHeight));
        if(previous)SelectObject(dc,previous);ReleaseDC(state.list,dc);
    }
    auto step=lines==WHEEL_PAGESCROLL?static_cast<long long>(page):(std::min)(static_cast<long long>(page),static_cast<long long>(lines)*line);
    auto amount=static_cast<long long>(state.wheelRemainder)-static_cast<long long>(delta)*step;
    auto target=std::clamp(static_cast<long long>(top)+amount/WHEEL_DELTA,0LL,static_cast<long long>((std::max)(0,count-page)));
    state.wheelRemainder=static_cast<int>(amount%WHEEL_DELTA);
    if((target==0 && amount<0)||(target==count-page && amount>0))state.wheelRemainder=0;
    move(bar,state.list,static_cast<int>(target));
}
LRESULT CALLBACK barProc(HWND bar,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto state=reinterpret_cast<Scroll*>(data);
    if(message==WM_PAINT||message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{}; HDC dc=message==WM_PRINTCLIENT?reinterpret_cast<HDC>(wp):BeginPaint(bar,&paint); RECT r{}; GetClientRect(bar,&r);
        auto themeOwner=state->themeOwner?state->themeOwner:GetParent(bar);
        FillRect(dc,&r,panelBackground(themeOwner));
        auto t=thumb(bar,state->list);
        if(t.bottom>t.top) {
            bool dark=usesDarkTheme(themeOwner); auto color=dark ? (state->dragging ? RGB(190,190,190) : RGB(120,120,120)) : GetSysColor(COLOR_3DSHADOW);
            auto brush=CreateSolidBrush(color); auto old=SelectObject(dc,brush); auto pen=SelectObject(dc,GetStockObject(NULL_PEN));
            RoundRect(dc,t.left,t.top,t.right,t.bottom,t.right-t.left,t.right-t.left);
            SelectObject(dc,old); SelectObject(dc,pen); DeleteObject(brush);
        }
        if(message==WM_PAINT)EndPaint(bar,&paint); return 0;
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
    auto bar=reinterpret_cast<HWND>(data);
    if(message==WM_MOUSEWHEEL) {
        DWORD_PTR state=0;
        if(GetWindowSubclass(bar,barProc,1,&state) && reinterpret_cast<Scroll*>(state)->list==list)
            wheel(bar,*reinterpret_cast<Scroll*>(state),GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    // Common controls can restore scrollbar styles inside scrolling/layout.
    // Remove them before Windows calculates or paints the non-client area,
    // rather than hiding an already painted bar after LVM_SCROLL/WM_PAINT.
    if(message==WM_STYLECHANGING && static_cast<int>(wp)==GWL_STYLE)
        reinterpret_cast<STYLESTRUCT*>(lp)->styleNew&=~(WS_VSCROLL|WS_HSCROLL);
    if(message==WM_NCCALCSIZE || message==WM_NCPAINT) {
        auto style=GetWindowLongPtrW(list,GWL_STYLE);
        if(style&(WS_VSCROLL|WS_HSCROLL))SetWindowLongPtrW(list,GWL_STYLE,style&~(WS_VSCROLL|WS_HSCROLL));
    }
    auto result=DefSubclassProc(list,message,wp,lp);
    if(message==WM_PAINT || message==WM_SIZE || message==WM_KEYDOWN || message==LB_SETTOPINDEX || message==LB_RESETCONTENT || message==LB_ADDSTRING || message==LVM_SCROLL || message==LVM_ENSUREVISIBLE || message==LVM_INSERTITEMW || message==LVM_DELETEALLITEMS)
        updateListScrollbar(list,reinterpret_cast<HWND>(data));
    return result;
}
}
void updateListScrollbar(HWND list,HWND bar) {
    if(!IsWindow(bar)) return;
    DWORD_PTR data=0;if(!GetWindowSubclass(bar,barProc,1,&data)||reinterpret_cast<Scroll*>(data)->list!=list)return;
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};int count,page,top;range(bar,list,count,page,top);
    info.nMax=(std::max)(0,count-1);info.nPage=page;info.nPos=top;
    SetScrollInfo(bar,SB_CTL,&info,FALSE); InvalidateRect(bar,nullptr,FALSE);
}
void attachListScrollbar(HWND list,HWND bar) {
    SetWindowSubclass(list,listProc,1,reinterpret_cast<DWORD_PTR>(bar));
    SetWindowSubclass(bar,barProc,1,reinterpret_cast<DWORD_PTR>(new Scroll{list})); updateListScrollbar(list,bar);
}
void useListScrollbar(HWND list,HWND bar) {
    DWORD_PTR data=0;if(!GetWindowSubclass(bar,barProc,1,&data))return;
    auto state=reinterpret_cast<Scroll*>(data);
    if(state->list!=list)state->wheelRemainder=0;
    state->list=list;
    SetWindowSubclass(list,listProc,1,reinterpret_cast<DWORD_PTR>(bar));
    SetWindowLongPtrW(list,GWL_STYLE,GetWindowLongPtrW(list,GWL_STYLE)&~(WS_VSCROLL|WS_HSCROLL));updateListScrollbar(list,bar);
}
void attachContentScrollbar(HWND owner,HWND bar,HWND themeOwner) { SetWindowSubclass(bar,barProc,1,reinterpret_cast<DWORD_PTR>(new Scroll{owner,0,false,themeOwner})); }
}
