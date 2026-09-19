#include "theme.h"
#include "resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <string>

namespace books {
namespace {
constexpr wchar_t ThemeProperty[] = L"VolturaBooks.Theme";
struct Theme {
    bool dark = false;
    bool messageBox = false;
    HBRUSH background = nullptr, field = nullptr, panelBrush = nullptr;
    COLORREF foreground = 0, surface = 0, input = 0, panel = 0;
    HFONT heading = nullptr;
    HFONT statusHeading = nullptr;
    HFONT sectionHeading = nullptr;
    ~Theme() { DeleteObject(background); DeleteObject(field); DeleteObject(panelBrush); DeleteObject(heading); DeleteObject(statusHeading); DeleteObject(sectionHeading); }
};
Theme* theme(HWND window) { return reinterpret_cast<Theme*>(GetPropW(window, ThemeProperty)); }
thread_local HHOOK focusInputHook=nullptr;
thread_local unsigned themedWindows=0;
bool highContrast() {HIGHCONTRASTW value{sizeof(value)};return SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(value),&value,0)&&(value.dwFlags&HCF_HIGHCONTRASTON);}
bool alwaysShowCues() {BOOL enabled=FALSE;SystemParametersInfoW(SPI_GETKEYBOARDCUES,0,&enabled,0);return enabled||highContrast();}
HWND themedAncestor(HWND window) {for(auto current=window;current;current=GetParent(current))if(theme(current))return current;return nullptr;}
// Observe this UI thread only, before dialog keyboard processing consumes Tab.
// Native focus, tab order and control semantics remain untouched.
LRESULT CALLBACK focusInput(int code,WPARAM wp,LPARAM lp) {
    if(code==HC_ACTION&&wp==PM_REMOVE){auto message=reinterpret_cast<MSG*>(lp);
        const bool keyboard=message->message==WM_KEYDOWN||message->message==WM_SYSKEYDOWN;
        const bool mouse=message->message==WM_LBUTTONDOWN||message->message==WM_RBUTTONDOWN||message->message==WM_MBUTTONDOWN||message->message==WM_XBUTTONDOWN;
        if(keyboard||mouse)updateFocusCues(message->hwnd,keyboard);
        if(message->message==WM_KEYDOWN&&(message->wParam==VK_F11||message->wParam==VK_F5)){
            auto root=themedAncestor(message->hwnd);
            if(root&&SendMessageW(root,DialogShortcut,message->wParam,0))message->message=WM_NULL;
        }
    }
    return CallNextHookEx(focusInputHook,code,wp,lp);
}
bool prefersDark() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0) && (contrast.dwFlags&HCF_HIGHCONTRASTON)) return false;
    DWORD light=1, size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    return light==0;
}
std::wstring label(HWND control) {
    std::wstring value(GetWindowTextLengthW(control)+1,0);
    value.resize(GetWindowTextW(control,value.data(),static_cast<int>(value.size())));
    return value;
}
void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc,color); FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void border(HDC dc, RECT rect, COLORREF color) {
    auto brush=CreateSolidBrush(color); FrameRect(dc,&rect,brush); DeleteObject(brush);
}
void rounded(HDC dc,RECT rect,COLORREF background,COLORREF line,int radius) {
    auto brush=CreateSolidBrush(background); auto pen=CreatePen(PS_SOLID,1,line);
    auto oldBrush=SelectObject(dc,brush), oldPen=SelectObject(dc,pen);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,radius,radius);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(brush); DeleteObject(pen);
}
LRESULT CALLBACK sectionProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto t=theme(GetParent(window));
    if(message==WM_PAINT && t) {
        PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint); const int saved=SaveDC(dc);
        RECT r{}; GetClientRect(window,&r); fill(dc,r,t->panel);
        SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)));
        SetBkMode(dc,TRANSPARENT);
        RECT badge{0,0,r.bottom,r.bottom};
        rounded(dc,badge,GetSysColor(COLOR_HIGHLIGHT),GetSysColor(COLOR_HIGHLIGHT),MulDiv(8,GetDpiForWindow(window),96));
        SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));
        auto value=label(window); DrawTextW(dc,value.c_str(),1,&badge,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        const auto title=value.find_first_not_of(L" ",1);
        r.left=badge.right+MulDiv(10,GetDpiForWindow(window),96); SetTextColor(dc,t->foreground);
        if(title!=std::wstring::npos) DrawTextW(dc,value.c_str()+title,-1,&r,DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        RestoreDC(dc,saved); EndPaint(window,&paint); return 0;
    }
    return DefSubclassProc(window,message,wp,lp);
}
LRESULT CALLBACK editProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto t=theme(GetParent(window));
    if(GetDlgCtrlID(window)==IDC_HOST || GetDlgCtrlID(window)==IDC_PORT) {
        const bool port=GetDlgCtrlID(window)==IDC_PORT;
        auto allowed=[port](wchar_t c) { if(port) return c>=L'0' && c<=L'9'; return (c>=L'a' && c<=L'z') || (c>=L'A' && c<=L'Z') || (c>=L'0' && c<=L'9') || c==L'.' || c==L'-' || c==L':'; };
        bool reject=false;
        if(message==WM_CHAR && wp>=32 && !allowed(static_cast<wchar_t>(wp))) reject=true;
        if(message==WM_PASTE && OpenClipboard(window)) {
            if(auto handle=GetClipboardData(CF_UNICODETEXT)) {
                if(auto value=static_cast<const wchar_t*>(GlobalLock(handle))) {
                    for(auto c=value;*c;++c) if(!allowed(*c)) { reject=true; break; }
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        if(reject) { PostMessageW(GetParent(window),WM_HOST_INPUT_REJECTED,port ? IDC_PORT : IDC_HOST,0); return 0; }
    }
    const int pad=MulDiv(7,GetDpiForWindow(window),96);
    if(message==WM_NCCALCSIZE && t) {
        auto r=wp ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] : reinterpret_cast<RECT*>(lp);
        // A single-line edit draws at the top of its client area. Center that
        // area using the actual font height instead of a fixed top inset.
        HDC dc=GetDC(window);
        auto font=reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0));
        auto previous=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
        SelectObject(dc,previous); ReleaseDC(window,dc);
        const int available=r->bottom-r->top;
        const int height=(std::min)(available,static_cast<int>(metrics.tmHeight));
        r->top+=(available-height)/2; r->bottom=r->top+height;
        r->left+=pad+(GetDlgCtrlID(window)==IDC_SEARCH ? MulDiv(22,GetDpiForWindow(window),96) : 0);
        r->right-=pad+(GetDlgCtrlID(window)==IDC_SEARCH ? MulDiv(30,GetDpiForWindow(window),96) : 0); return 0;
    }
    if(message==WM_NCPAINT && t) {
        HDC dc=GetWindowDC(window); RECT r{},client{}; GetWindowRect(window,&r); GetClientRect(window,&client);
        POINT origin{}; ClientToScreen(window,&origin); OffsetRect(&client,origin.x-r.left,origin.y-r.top);
        ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
        OffsetRect(&r,-r.left,-r.top); fill(dc,r,t->surface);
        rounded(dc,r,t->input,GetFocus()==window&&focusCuesVisible(window) ? (t->dark ? RGB(96,205,255) : GetSysColor(COLOR_HIGHLIGHT)) : (t->dark ? RGB(78,78,78) : GetSysColor(COLOR_WINDOWFRAME)),pad);
        if(GetDlgCtrlID(window)==IDC_SEARCH) {
            const int size=MulDiv(16,GetDpiForWindow(window),96);
            auto font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons");
            auto oldFont=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,t->foreground);
            RECT icon{pad,0,pad+size,r.bottom};
            DrawTextW(dc,L"\xE721",1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,oldFont); DeleteObject(font);
        }
        ReleaseDC(window,dc); return 0;
    }
    auto result=DefSubclassProc(window,message,wp,lp);
    if(message==WM_SETFONT)
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    if(message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_UPDATEUISTATE) RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
    return result;
}
LRESULT CALLBACK comboProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    auto t=theme(GetParent(window));
    if(message==WM_PAINT && t && t->dark) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint);
        RECT r{}; GetClientRect(window,&r); fill(dc,r,t->surface);
        rounded(dc,r,t->input,GetFocus()==window&&focusCuesVisible(window) ? RGB(96,205,255) : RGB(78,78,78),MulDiv(7,GetDpiForWindow(window),96));
        auto font=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)));
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,t->foreground);
        RECT text=r; const int pad=MulDiv(6,GetDpiForWindow(window),96); text.left+=pad; text.right-=pad*4;
        auto value=label(window); DrawTextW(dc,value.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        RECT arrow=r; arrow.left=arrow.right-pad*4; DrawTextW(dc,L"\x2304",1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,font); EndPaint(window,&paint); return 0;
    }
    auto result=DefSubclassProc(window,message,wp,lp);
    if(message==WM_SETFOCUS || message==WM_KILLFOCUS || message==CB_SETCURSEL || message==WM_UPDATEUISTATE) InvalidateRect(window,nullptr,FALSE);
    return result;
}
const wchar_t* actionButtonGlyph(HWND button) {
    const auto parent=GetParent(button);
    switch(GetDlgCtrlID(button)) {
        case IDOK:
            if(GetDlgItem(parent,IDC_SEARCH)) return L"\xE724";
            if(GetDlgItem(parent,IDC_METHOD_DIRECT)) return L"\xE74E";
            return L"\xE73E";
        case IDYES: return L"\xE73E";
        case IDNO: case IDCANCEL: case IDABORT: return L"\xE711";
        case IDRETRY: case IDTRYAGAIN: case IDC_RETRY: case IDC_UPDATE_CHECK: return L"\xE72C";
        case IDIGNORE: case IDCONTINUE: case IDC_SKIP: return L"\xE893";
        case IDC_SETTINGS: return L"\xE713";
        case IDC_ABOUT: return L"\xE946";
        case IDC_CHOOSE_BOOK: case IDC_ABOUT_LICENSE: return L"\xE8A5";
        case IDC_BROWSE_FOLDER: case IDC_FOLDER: case IDC_OPEN_FOLDER: return L"\xE8B7";
        case IDC_OPEN_FILE: return L"\xE8A7";
        case IDC_COPY_PATH: return L"\xE8C8";
        case IDC_REFRESH: return L"\xE72C";
        case IDC_FULLSCREEN_READER: return L"\xE740";
        case IDC_UPDATE_OPEN: return L"\xE896";
        case IDC_ABOUT_WEBSITE: return L"\xE774";
        case IDC_ABOUT_ISSUES: return L"\xEBE8";
        case IDC_ABOUT_SUPPORT: return L"\xEB51";
        case IDC_ABOUT_COFFEE: return L"\xE719";
        default: return nullptr;
    }
}
void paintButton(NMCUSTOMDRAW* draw, const Theme& t) {
    int saved=SaveDC(draw->hdc);
    SelectObject(draw->hdc,reinterpret_cast<HFONT>(SendMessageW(draw->hdr.hwndFrom,WM_GETFONT,0,0)));
            HWND button=draw->hdr.hwndFrom;
            bool disabled=!IsWindowEnabled(button), pressed=(draw->uItemState&CDIS_SELECTED)!=0;
            const auto type=GetWindowLongPtrW(button,GWL_STYLE)&BS_TYPEMASK;
            const bool check=type==BS_AUTOCHECKBOX || type==BS_CHECKBOX;
            RECT r{}; GetClientRect(button,&r); RECT text=r;
            const auto controlId=GetDlgCtrlID(button);
            if(controlId==IDC_PASSWORD_SHOW || controlId==IDC_PASSWORD_COPY) {
                fill(draw->hdc,r,t.panel);
                rounded(draw->hdc,r,t.input,t.input,MulDiv(7,GetDpiForWindow(button),96));
                const int size=MulDiv(18,GetDpiForWindow(button),96);
                auto glyph=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons");
                SelectObject(draw->hdc,glyph); SetBkMode(draw->hdc,TRANSPARENT); SetTextColor(draw->hdc,t.foreground);
                const wchar_t* icon=controlId==IDC_PASSWORD_COPY ? L"\xE8C8" : SendDlgItemMessageW(GetParent(button),IDC_PASSWORD,EM_GETPASSWORDCHAR,0,0) ? L"\xE890" : L"\xED1A";
                DrawTextW(draw->hdc,icon,1,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r);
                RestoreDC(draw->hdc,saved); DeleteObject(glyph); return;
            }
            if(controlId==IDC_ADVANCED && GetDlgItem(GetParent(button),IDC_METHOD_DIRECT)) {
                fill(draw->hdc,r,t.panel); SetBkMode(draw->hdc,TRANSPARENT); SetTextColor(draw->hdc,t.foreground);
                text.left+=MulDiv(24,GetDpiForWindow(button),96);
                auto value=label(button); DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                const int size=MulDiv(14,GetDpiForWindow(button),96);
                auto glyph=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons"); SelectObject(draw->hdc,glyph);
                RECT gear{0,0,size,r.bottom}; DrawTextW(draw->hdc,L"\xE713",1,&gear,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
                RECT icon{r.right-size,0,r.right,r.bottom}; DrawTextW(draw->hdc,IsDlgButtonChecked(GetParent(button),IDC_ADVANCED)==BST_CHECKED ? L"\xE70E" : L"\xE70D",1,&icon,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r);
                RestoreDC(draw->hdc,saved); DeleteObject(glyph); return;
            }
            if(controlId==IDC_METHOD_DIRECT || controlId==IDC_METHOD_PROVIDER) {
                const bool selected=SendMessageW(button,BM_GETCHECK,0,0)==BST_CHECKED;
                fill(draw->hdc,r,t.panel);
                rounded(draw->hdc,r,selected ? GetSysColor(COLOR_HIGHLIGHT) : t.input,selected ? GetSysColor(COLOR_HIGHLIGHT) : t.dark ? RGB(78,78,78) : GetSysColor(COLOR_3DSHADOW),MulDiv(8,GetDpiForWindow(button),96));
                SetBkMode(draw->hdc,TRANSPARENT); SetTextColor(draw->hdc,selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground);
                const int pad=MulDiv(12,GetDpiForWindow(button),96),iconSize=MulDiv(20,GetDpiForWindow(button),96);
                auto font=reinterpret_cast<HFONT>(SendMessageW(button,WM_GETFONT,0,0)); LOGFONTW lf{}; GetObjectW(font,sizeof(lf),&lf); lf.lfWeight=FW_SEMIBOLD;
                auto bold=CreateFontIndirectW(&lf); SelectObject(draw->hdc,bold);
                TEXTMETRICW metrics{}; GetTextMetricsW(draw->hdc,&metrics);
                text.left=pad*2+iconSize; text.top=(r.bottom-metrics.tmHeight*2-pad/2)/2;
                auto value=label(button); DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_SINGLELINE|DT_NOPREFIX);
                text.top+=metrics.tmHeight+pad/2; SelectObject(draw->hdc,font);
                DrawTextW(draw->hdc,controlId==IDC_METHOD_DIRECT ? L"No password needed" : L"Your existing email account",-1,&text,DT_SINGLELINE|DT_NOPREFIX);
                auto glyph=CreateFontW(-iconSize,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons"); SelectObject(draw->hdc,glyph);
                RECT icon{pad,0,pad+iconSize,r.bottom}; DrawTextW(draw->hdc,controlId==IDC_METHOD_DIRECT ? L"\xE724" : L"\xE715",1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r,selected);
                RestoreDC(draw->hdc,saved); DeleteObject(glyph); DeleteObject(bold); return;
            }
            if(check && (GetWindowLongPtrW(button,GWL_STYLE)&BS_PUSHLIKE)) {
                const bool selected=SendMessageW(button,BM_GETCHECK,0,0)==BST_CHECKED;
                fill(draw->hdc,r,t.surface);
                const auto background=pressed ? (t.dark ? RGB(65,65,65) : GetSysColor(COLOR_3DSHADOW))
                    : selected ? GetSysColor(COLOR_HIGHLIGHT) : t.input;
                rounded(draw->hdc,r,background,background,MulDiv(6,GetDpiForWindow(button),96));
                SetTextColor(draw->hdc,selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground); SetBkMode(draw->hdc,TRANSPARENT);
                auto value=label(button);
                HFONT glyphFont=nullptr; auto id=GetDlgCtrlID(button);
                if(id==IDC_VIEW_LIST || id==IDC_VIEW_THUMBS) {
                    value=id==IDC_VIEW_LIST ? L"\xE8FD" : L"\xE8A9";
                    glyphFont=CreateFontW(-MulDiv(18,GetDpiForWindow(button),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons");
                    SelectObject(draw->hdc,glyphFont);
                }
                DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r,selected);
                RestoreDC(draw->hdc,saved); if(glyphFont) DeleteObject(glyphFont); return;
            }
            const bool primary=(GetDlgItem(GetParent(button),IDC_DROP_ZONE) ? GetDlgCtrlID(button)==IDC_CHOOSE_BOOK : type==BS_DEFPUSHBUTTON) && !disabled;
            const auto id=GetDlgCtrlID(button);
            fill(draw->hdc,r,GetDlgItem(GetParent(button),IDC_SEARCH) && (id==IDC_OPEN_FILE || id==IDC_OPEN_FOLDER || id==IDC_COPY_PATH || id==IDC_FULLSCREEN_READER) ? t.panel : t.surface);
            if(auto glyph=actionButtonGlyph(button)) {
                const bool refreshHot=id==IDC_REFRESH && !disabled && (draw->uItemState&CDIS_HOT);
                const auto background=primary ? GetSysColor(COLOR_HIGHLIGHT) : t.dark ? (pressed ? RGB(65,65,65) : refreshHot ? RGB(58,58,58) : RGB(48,48,48)) : id==IDC_REFRESH && pressed ? GetSysColor(COLOR_3DSHADOW) : refreshHot ? GetSysColor(COLOR_3DLIGHT) : GetSysColor(COLOR_BTNFACE);
                rounded(draw->hdc,r,background,primary ? background : t.dark ? RGB(78,78,78) : GetSysColor(COLOR_3DSHADOW),MulDiv(7,GetDpiForWindow(button),96));
                SetTextColor(draw->hdc,disabled ? GetSysColor(COLOR_GRAYTEXT) : primary ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground); SetBkMode(draw->hdc,TRANSPARENT);
                auto value=id==IDC_REFRESH ? std::wstring{} : label(button); SIZE size{}; GetTextExtentPoint32W(draw->hdc,value.c_str(),static_cast<int>(value.size()),&size);
                const int iconSize=MulDiv(18,GetDpiForWindow(button),96),gap=MulDiv(8,GetDpiForWindow(button),96);
                // Centre one shared content column for the browser details actions,
                // rather than centring each differently sized label independently.
                LONG columnWidth=size.cx;
                if(GetDlgItem(GetParent(button),IDC_SEARCH) && (id==IDC_OPEN_FILE || id==IDC_OPEN_FOLDER || id==IDC_COPY_PATH || id==IDC_FULLSCREEN_READER)) {
                    for(auto action:{IDC_FULLSCREEN_READER,IDC_OPEN_FILE,IDC_OPEN_FOLDER,IDC_COPY_PATH}) {
                        auto sibling=GetDlgItem(GetParent(button),action);
                        if(!sibling)continue;
                        auto siblingLabel=label(sibling);SIZE measured{};
                        auto font=reinterpret_cast<HFONT>(SendMessageW(sibling,WM_GETFONT,0,0));
                        auto oldFont=font?SelectObject(draw->hdc,font):nullptr;
                        GetTextExtentPoint32W(draw->hdc,siblingLabel.c_str(),static_cast<int>(siblingLabel.size()),&measured);
                        if(oldFont)SelectObject(draw->hdc,oldFont);
                        columnWidth=(std::max)(columnWidth,measured.cx);
                    }
                }
                text.left=(r.right-columnWidth-iconSize-gap)/2+iconSize+gap;
                DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                auto iconFont=CreateFontW(-iconSize,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe Fluent Icons");
                auto old=SelectObject(draw->hdc,iconFont); RECT icon{text.left-iconSize-gap,0,text.left-gap,r.bottom};
                if(id==IDC_REFRESH)icon=r;
                if(id==IDC_FULLSCREEN_READER)drawFullscreenIcon(draw->hdc,icon,GetDpiForWindow(button),GetTextColor(draw->hdc));
                else DrawTextW(draw->hdc,glyph,1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                SelectObject(draw->hdc,old); DeleteObject(iconFont);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r,primary);
                RestoreDC(draw->hdc,saved); return;
            }
            if(GetDlgItem(GetParent(button),IDC_METHOD_DIRECT) && !check) {
                rounded(draw->hdc,r,primary ? GetSysColor(COLOR_HIGHLIGHT) : t.input,primary ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_GRAYTEXT),MulDiv(7,GetDpiForWindow(button),96));
                SetTextColor(draw->hdc,primary ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground); SetBkMode(draw->hdc,TRANSPARENT);
                auto value=label(button); DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,r,primary);
                RestoreDC(draw->hdc,saved); return;
            }
            if(!check) rounded(draw->hdc,r,primary ? GetSysColor(COLOR_HIGHLIGHT) : t.dark ? (pressed ? RGB(65,65,65) : RGB(48,48,48)) : GetSysColor(COLOR_BTNFACE),primary ? GetSysColor(COLOR_HIGHLIGHT) : t.dark ? RGB(78,78,78) : GetSysColor(COLOR_3DSHADOW),MulDiv(7,GetDpiForWindow(button),96));
            if(check) {
                const int size=MulDiv(14,GetDpiForWindow(button),96);
                RECT box{r.left,(r.bottom+r.top-size)/2,r.left+size,(r.bottom+r.top+size)/2};
                border(draw->hdc,box,disabled?GetSysColor(COLOR_GRAYTEXT):t.dark?RGB(160,160,160):GetSysColor(COLOR_WINDOWFRAME));
                if(draw->uItemState&CDIS_FOCUS)drawKeyboardFocus(button,draw->hdc,box);
                if(SendMessageW(button,BM_GETCHECK,0,0)==BST_CHECKED) {
                    SetTextColor(draw->hdc,disabled?GetSysColor(COLOR_GRAYTEXT):t.dark?RGB(96,205,255):GetSysColor(COLOR_HIGHLIGHT)); SetBkMode(draw->hdc,TRANSPARENT);
                    DrawTextW(draw->hdc,L"\x2713",1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                }
                text.left=box.right+MulDiv(6,GetDpiForWindow(button),96);
            }
            SetTextColor(draw->hdc,disabled ? GetSysColor(COLOR_GRAYTEXT) : primary ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground); SetBkMode(draw->hdc,TRANSPARENT);
            auto value=label(button); DrawTextW(draw->hdc,value.c_str(),-1,&text,DT_VCENTER|DT_SINGLELINE|(check ? DT_LEFT : DT_CENTER));
            if(!check&&(draw->uItemState&CDIS_FOCUS))drawKeyboardFocus(button,draw->hdc,r,primary);
    RestoreDC(draw->hdc,saved);
}
LRESULT CALLBACK buttonProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    // Owner-drawn controls (including reader overlays) supply their own visuals.
    if((GetWindowLongPtrW(window,GWL_STYLE)&BS_TYPEMASK)==BS_OWNERDRAW)
        return DefSubclassProc(window,message,wp,lp);
    auto t=theme(GetParent(window));
    if(message==WM_PAINT && t && !highContrast()) {
        PAINTSTRUCT paint{}; NMCUSTOMDRAW draw{}; draw.hdc=BeginPaint(window,&paint); draw.hdr.hwndFrom=window;
        auto state=SendMessageW(window,BM_GETSTATE,0,0);
        draw.uItemState=((state&BST_PUSHED) ? CDIS_SELECTED : 0) | ((state&BST_FOCUS) ? CDIS_FOCUS : 0);
        paintButton(&draw,*t); EndPaint(window,&paint); return 0;
    }
    auto result=DefSubclassProc(window,message,wp,lp);
    if(message==BM_SETCHECK || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_ENABLE || message==WM_UPDATEUISTATE)
        InvalidateRect(window,nullptr,FALSE);
    return result;
}
void refresh(HWND window,Theme& t) {
    t.dark=prefersDark();
    t.surface=t.dark ? RGB(20,20,20) : GetSysColor(COLOR_3DFACE);
    t.panel=t.dark ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW);
    t.input=t.dark ? RGB(45,45,45) : GetSysColor(COLOR_WINDOW);
    t.foreground=t.dark ? RGB(240,240,240) : GetSysColor(COLOR_WINDOWTEXT);
    DeleteObject(t.background); DeleteObject(t.field); DeleteObject(t.panelBrush);
    t.background=CreateSolidBrush(t.surface); t.field=CreateSolidBrush(t.input); t.panelBrush=CreateSolidBrush(t.panel);
    BOOL dark=t.dark; DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    if(GetDlgItem(window,IDC_SEND_HELP)) {
        LOGFONTW font{}; GetObjectW(reinterpret_cast<HFONT>(SendDlgItemMessageW(window,IDC_STATUS,WM_GETFONT,0,0)),sizeof(font),&font);
        font.lfHeight=-MulDiv(12,GetDpiForWindow(window),72); font.lfWeight=FW_SEMIBOLD;
        auto replacement=CreateFontIndirectW(&font); SendDlgItemMessageW(window,IDC_STATUS,WM_SETFONT,reinterpret_cast<WPARAM>(replacement),FALSE);
        DeleteObject(t.statusHeading); t.statusHeading=replacement;
    }
    if(auto heading=GetDlgItem(window,IDC_HEADING)) {
        LOGFONTW font{}; GetObjectW(reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),sizeof(font),&font);
        font.lfHeight=-MulDiv(GetDlgItem(window,IDC_DROP_ZONE) || GetDlgItem(window,IDC_SEND_HELP) || GetDlgItem(window,IDC_METHOD_DIRECT) || GetDlgItem(window,IDC_UPDATE_CHECK) ? 18 : 12,GetDpiForWindow(window),72); font.lfWeight=FW_SEMIBOLD;
        auto replacement=CreateFontIndirectW(&font); SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(replacement),FALSE);
        DeleteObject(t.heading); t.heading=replacement;
    }
    if(GetDlgItem(window,IDC_METHOD_DIRECT)) {
        LOGFONTW font{}; GetObjectW(reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),sizeof(font),&font); font.lfWeight=FW_SEMIBOLD;
        auto replacement=CreateFontIndirectW(&font);
        for(auto id:{IDC_DELIVERY_HEADING,IDC_DESTINATION_HEADING,IDC_ACCOUNT_HEADING,IDC_ADVANCED}) SendDlgItemMessageW(window,id,WM_SETFONT,reinterpret_cast<WPARAM>(replacement),FALSE);
        DeleteObject(t.sectionHeading); t.sectionHeading=replacement;
    }
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        wchar_t cls[64]{}; GetClassNameW(child,cls,64);
        if(lstrcmpiW(cls,L"Edit")==0 || lstrcmpiW(cls,L"ComboBox")==0 || lstrcmpiW(cls,PROGRESS_CLASSW)==0)
            SetWindowTheme(child,t.dark ? L"" : nullptr,t.dark ? L"" : nullptr);
        const auto id=GetDlgCtrlID(child);
        if(id==IDC_DELIVERY_HEADING || id==IDC_DESTINATION_HEADING || id==IDC_ACCOUNT_HEADING) SetWindowSubclass(child,sectionProc,1,0);
        if(lstrcmpiW(cls,L"Button")==0) SetWindowSubclass(child,buttonProc,1,0);
        if(lstrcmpiW(cls,L"Edit")==0) {
            SetWindowSubclass(child,editProc,1,0);
            auto style=GetWindowLongPtrW(child,GWL_EXSTYLE);
            SetWindowLongPtrW(child,GWL_EXSTYLE,style&~WS_EX_CLIENTEDGE);
            SetWindowPos(child,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
        }
        if(lstrcmpiW(cls,L"ComboBox")==0) {
            SetWindowSubclass(child,comboProc,1,0);
            HDC dc=GetDC(child); auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
            TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SelectObject(dc,old); ReleaseDC(child,dc);
            const auto height=metrics.tmHeight+MulDiv(GetDlgCtrlID(child)==IDC_SORT ? 4 : 10,GetDpiForWindow(child),96);
            SendMessageW(child,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),height);
            SendMessageW(child,CB_SETITEMHEIGHT,0,height);
        }
        if(lstrcmpiW(cls,WC_LINK)==0) {
            LITEM item{}; item.mask=LIF_ITEMINDEX|LIF_STATE; item.iLink=0; item.stateMask=LIS_DEFAULTCOLORS; item.state=LIS_DEFAULTCOLORS;
            SendMessageW(child,LM_SETITEM,0,reinterpret_cast<LPARAM>(&item));
        }
        if(lstrcmpiW(cls,PROGRESS_CLASSW)==0) {
            SendMessageW(child,PBM_SETBKCOLOR,0,t.input);
            SendMessageW(child,PBM_SETBARCOLOR,0,t.dark ? RGB(96,205,255) : CLR_DEFAULT);
        }
    }
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME);
}
LRESULT CALLBACK dialogProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& t=*reinterpret_cast<Theme*>(data);
    if(message==WM_PAINT && GetDlgItem(window,IDC_SEND_HELP)) {
        PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint); FillRect(dc,&paint.rcPaint,t.background);
        auto pen=CreatePen(PS_SOLID,(std::max)(1,MulDiv(1,GetDpiForWindow(window),96)),t.dark ? RGB(105,105,105) : GetSysColor(COLOR_3DSHADOW));
        auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,t.panelBrush);
        const int pad=MulDiv(12,GetDpiForWindow(window),96);
        for(auto pair:{std::pair{IDC_COVER,IDC_FILENAME},std::pair{IDC_STATUS,IDC_PROGRESS}}) {
            RECT a{},b{}; GetWindowRect(GetDlgItem(window,pair.first),&a); GetWindowRect(GetDlgItem(window,pair.second),&b);
            RECT r{}; UnionRect(&r,&a,&b); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2); InflateRect(&r,pad,pad);
            RoundRect(dc,r.left,r.top,r.right,r.bottom,pad,pad);
        }
        SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); EndPaint(window,&paint); return 0;
    }
    // The system message box paints its button band independently of
    // WM_CTLCOLORDLG. Paint the entire surface; its child controls paint normally.
    if(t.dark && t.messageBox && message==WM_ERASEBKGND) {
        RECT rect{}; GetClientRect(window,&rect);
        FillRect(reinterpret_cast<HDC>(wp),&rect,t.background); return 1;
    }
    if(t.dark && t.messageBox && message==WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc=BeginPaint(window,&paint);
        FillRect(dc,&paint.rcPaint,t.background); EndPaint(window,&paint); return 0;
    }
    if(message==WM_DPICHANGED) {
        // Per-Monitor V2's dialog manager rescales fonts, controls and the window first.
        auto result=DefSubclassProc(window,message,wp,lp);
        refresh(window,t);
        auto dpi=GetDpiForWindow(window);
        SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_BOOK),IMAGE_ICON,GetSystemMetricsForDpi(SM_CXSMICON,dpi),GetSystemMetricsForDpi(SM_CYSMICON,dpi),LR_SHARED)));
        return result;
    }
    if(message==WM_SETTINGCHANGE || message==WM_SYSCOLORCHANGE || message==WM_THEMECHANGED) refresh(window,t);
    if(message==WM_CTLCOLORDLG || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX || message==WM_CTLCOLORBTN) {
        if(t.dark || GetDlgItem(window,IDC_METHOD_DIRECT)) {
            auto dc=reinterpret_cast<HDC>(wp); auto control=reinterpret_cast<HWND>(lp);
            const bool field=message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX;
            wchar_t cls[64]{}; GetClassNameW(control,cls,64);
            const auto id=GetDlgCtrlID(control);
            const bool inPanel=!field && ((GetDlgItem(window,IDC_UPDATE_CHECK) && message==WM_CTLCOLORSTATIC) || (GetDlgItem(window,IDC_METHOD_DIRECT) && id!=IDC_HEADING && id!=IDC_SETUP_SUBTITLE) || id==IDC_COVER || id==IDC_FILENAME || id==IDC_STATUS || id==IDC_SEND_HELP || id==IDC_FILE_LIST);
            const bool secondary=id==IDC_DISCOVERY_STATUS || id==IDC_APPROVAL_HELP;
            SetTextColor(dc,!IsWindowEnabled(control) ? GetSysColor(COLOR_GRAYTEXT) : lstrcmpiW(cls,WC_LINK)==0 ? GetSysColor(COLOR_HIGHLIGHT) : secondary ? (t.dark ? RGB(190,190,190) : GetSysColor(COLOR_WINDOWTEXT)) : t.foreground);
            SetBkColor(dc,inPanel ? t.panel : field ? t.input : t.surface);
            return reinterpret_cast<LRESULT>(inPanel ? t.panelBrush : field ? t.field : t.background);
        }
    }
    if(message==WM_DRAWITEM) {
        auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(draw && draw->CtlID==IDC_DROP_ZONE) {
            int saved=SaveDC(draw->hDC); auto r=draw->rcItem;
            fill(draw->hDC,r,t.surface); const int pad=MulDiv(8,GetDpiForWindow(window),96);
            rounded(draw->hDC,r,t.input,t.dark ? RGB(70,70,70) : GetSysColor(COLOR_3DSHADOW),pad*2);
            InflateRect(&r,-pad,-pad);
            const auto dpi=GetDpiForWindow(window);
            const DWORD dashes[]={static_cast<DWORD>(MulDiv(10,dpi,96)),static_cast<DWORD>(MulDiv(6,dpi,96))};
            LOGBRUSH stroke{BS_SOLID,GetSysColor(COLOR_HIGHLIGHT),0};
            auto pen=ExtCreatePen(PS_GEOMETRIC|PS_USERSTYLE|PS_ENDCAP_FLAT|PS_JOIN_MITER,MulDiv(2,dpi,96),&stroke,2,dashes);
            auto oldPen=SelectObject(draw->hDC,pen); auto oldBrush=SelectObject(draw->hDC,GetStockObject(NULL_BRUSH));
            RoundRect(draw->hDC,r.left,r.top,r.right,r.bottom,pad*2,pad*2); SelectObject(draw->hDC,oldPen); SelectObject(draw->hDC,oldBrush); DeleteObject(pen);
            const int size=MulDiv(48,GetDpiForWindow(window),96);
            const int contentTop=r.top+(r.bottom-r.top-size-pad*7)/2;
            auto icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_BOOK),IMAGE_ICON,size,size,LR_SHARED));
            DrawIconEx(draw->hDC,(r.right+r.left-size)/2,contentTop,icon,size,size,0,nullptr,DI_NORMAL);
            SelectObject(draw->hDC,reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem,WM_GETFONT,0,0)));
            SetTextColor(draw->hDC,t.foreground); SetBkMode(draw->hDC,TRANSPARENT);
            RECT text{r.left,contentTop+pad*2+size,r.right,r.bottom};
            DrawTextW(draw->hDC,L"Drag books and documents here",-1,&text,DT_CENTER|DT_SINGLELINE);
            TEXTMETRICW metrics{}; GetTextMetricsW(draw->hDC,&metrics); text.top+=metrics.tmHeight+pad;
            SetTextColor(draw->hDC,t.dark ? RGB(185,185,185) : GetSysColor(COLOR_WINDOWTEXT));
            DrawTextW(draw->hDC,L"EPUB, PDF, Word, images and more",-1,&text,DT_CENTER|DT_SINGLELINE);
            RestoreDC(draw->hDC,saved); return TRUE;
        }
        if(draw && draw->CtlType==ODT_COMBOBOX) {
            const bool selected=(draw->itemState&ODS_SELECTED)!=0;
            fill(draw->hDC,draw->rcItem,selected ? GetSysColor(COLOR_HIGHLIGHT) : t.input);
            SetBkMode(draw->hDC,TRANSPARENT); SetTextColor(draw->hDC,selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.foreground);
            if(draw->itemID!=static_cast<UINT>(-1)) {
                auto length=SendMessageW(draw->hwndItem,CB_GETLBTEXTLEN,draw->itemID,0);
                if(length>=0) {
                    std::wstring value(static_cast<size_t>(length)+1,0); SendMessageW(draw->hwndItem,CB_GETLBTEXT,draw->itemID,reinterpret_cast<LPARAM>(value.data()));
                    RECT r=draw->rcItem; r.left+=MulDiv(5,GetDpiForWindow(window),96);
                    DrawTextW(draw->hDC,value.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                }
            }
            if(!t.dark && (draw->itemState&ODS_FOCUS))drawKeyboardFocus(window,draw->hDC,draw->rcItem,selected);
            return TRUE;
        }
    }
    if(message==WM_NOTIFY && t.dark) {
        auto draw=reinterpret_cast<NMCUSTOMDRAW*>(lp);
        wchar_t cls[64]{}; GetClassNameW(draw->hdr.hwndFrom,cls,64);
        if(draw->hdr.code==NM_CUSTOMDRAW && lstrcmpiW(cls,L"Button")==0 && (GetWindowLongPtrW(draw->hdr.hwndFrom,GWL_STYLE)&BS_TYPEMASK)!=BS_OWNERDRAW && draw->dwDrawStage==CDDS_PREPAINT) {
            paintButton(draw,t);
            return CDRF_SKIPDEFAULT;
        }
    }
    if(message==WM_NCDESTROY) {
        RemovePropW(window,ThemeProperty); RemoveWindowSubclass(window,dialogProc,1);
        if(themedWindows&&!--themedWindows){if(focusInputHook)UnhookWindowsHookEx(focusInputHook);focusInputHook=nullptr;}
        auto result=DefSubclassProc(window,message,wp,lp); delete &t; return result;
    }
    return DefSubclassProc(window,message,wp,lp);
}
}
void applyTheme(HWND window) {
    if(theme(window)) return;
    // Composite the dialog and its children so background, borders and text
    // become visible together rather than exposing intermediate erase passes.

    SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_CLIPCHILDREN);
    auto t=new Theme;
    if(!SetPropW(window,ThemeProperty,t) || !SetWindowSubclass(window,dialogProc,1,reinterpret_cast<DWORD_PTR>(t))) {
        RemovePropW(window,ThemeProperty); delete t; return;
    }
    refresh(window,*t);
    if(!themedWindows++)focusInputHook=SetWindowsHookExW(WH_GETMESSAGE,focusInput,nullptr,GetCurrentThreadId());
    SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_INITIALIZE,0),0);
    if(alwaysShowCues())updateFocusCues(window,true);
}
bool focusCuesVisible(HWND window) {return alwaysShowCues()||!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);}
void updateFocusCues(HWND window,bool keyboard) {
    auto root=themedAncestor(window);if(!root)return;
    const bool show=keyboard||alwaysShowCues();
    if(((SendMessageW(root,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)==0)==show)return;
    SendMessageW(root,WM_CHANGEUISTATE,MAKEWPARAM(show?UIS_CLEAR:UIS_SET,UISF_HIDEFOCUS),0);
    RedrawWindow(root,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);
}
void drawKeyboardFocus(HWND window,HDC dc,RECT bounds,bool selected) {
    if(!focusCuesVisible(window))return;
    if(highContrast()){InflateRect(&bounds,-2,-2);DrawFocusRect(dc,&bounds);return;}
    auto root=themedAncestor(window);const bool dark=root&&usesDarkTheme(root);
    const int inset=MulDiv(2,GetDpiForWindow(window),96);InflateRect(&bounds,-inset,-inset);
    auto pen=CreatePen(PS_SOLID,MulDiv(2,GetDpiForWindow(window),96),selected?GetSysColor(COLOR_HIGHLIGHTTEXT):dark?RGB(96,205,255):GetSysColor(COLOR_HIGHLIGHT));
    auto oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    const int radius=MulDiv(6,GetDpiForWindow(window),96);RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,radius,radius);
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
}
bool usesDarkTheme(HWND window) { auto t=theme(window); return t && t->dark; }
HBRUSH dialogBackground(HWND window) { auto t=theme(window); return t ? t->background : GetSysColorBrush(COLOR_3DFACE); }
HBRUSH panelBackground(HWND window) { auto t=theme(window); return t ? t->panelBrush : GetSysColorBrush(COLOR_WINDOW); }
namespace {
LRESULT CALLBACK messageHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_ACTIVATE) {
        auto window=reinterpret_cast<HWND>(wp); wchar_t cls[32]{};
        GetClassNameW(window,cls,32);
        if(lstrcmpW(cls,L"#32770")==0) {
            applyTheme(window);
            if(auto t=theme(window)) t->messageBox=true;
            InvalidateRect(window,nullptr,TRUE);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
}
int themedMessageBox(HWND owner,const wchar_t* text,const wchar_t* title,UINT flags) {
    auto hook=SetWindowsHookExW(WH_CBT,messageHook,nullptr,GetCurrentThreadId());
    int result=MessageBoxW(owner,text,title,flags);
    if(hook) UnhookWindowsHookEx(hook);
    return result;
}
}
