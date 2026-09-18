#include "placeholder.h"
#include <algorithm>
#include <cwctype>
namespace books {
void drawBookCover(HDC dc,RECT bounds,HBITMAP bitmap,const std::wstring& extension) {
    if(!bitmap) { drawPlaceholderCover(dc,bounds,extension); return; }
    BITMAP info{}; GetObjectW(bitmap,sizeof(info),&info);
    if(!info.bmWidth || !info.bmHeight) { drawPlaceholderCover(dc,bounds,extension); return; }
    HDC source=CreateCompatibleDC(dc); auto previous=SelectObject(source,bitmap);
    const double scale=(std::min)(static_cast<double>(bounds.right-bounds.left)/info.bmWidth,static_cast<double>(bounds.bottom-bounds.top)/info.bmHeight);
    int width=static_cast<int>(info.bmWidth*scale),height=static_cast<int>(info.bmHeight*scale);
    AlphaBlend(dc,bounds.left+(bounds.right-bounds.left-width)/2,bounds.top+(bounds.bottom-bounds.top-height)/2,width,height,source,0,0,info.bmWidth,info.bmHeight,{AC_SRC_OVER,0,255,AC_SRC_ALPHA});
    SelectObject(source,previous); DeleteDC(source);
}
void drawPlaceholderCover(HDC dc, RECT bounds, const std::wstring& extension) {
    const int saved=SaveDC(dc);
    const int height=bounds.bottom-bounds.top;
    const int width=(std::min)(static_cast<int>(bounds.right-bounds.left),height*2/3);
    RECT book{bounds.left+(bounds.right-bounds.left-width)/2,bounds.top,bounds.left+(bounds.right-bounds.left+width)/2,bounds.bottom};
    auto paint=[&](RECT r,COLORREF color) { SetDCBrushColor(dc,color); FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); };
    paint(book,RGB(13,102,110));
    paint({book.left,book.top,book.left+width/12,book.bottom},RGB(8,73,83));
    paint({book.left+width/12,book.top,book.left+width/12+1,book.bottom},RGB(61,143,149));
    auto brush=CreateSolidBrush(RGB(239,189,89)); auto oldBrush=SelectObject(dc,brush); auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    POINT ribbon[]={{book.right-width/4,book.top},{book.right-width/8,book.top},{book.right-width/8,book.top+height/5},{book.right-width*3/16,book.top+height/6},{book.right-width/4,book.top+height/5}};
    Polygon(dc,ribbon,5); SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(brush);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(237,246,239));
    auto text=[&](const wchar_t* value,RECT rect,int size,int weight,UINT flags) {
        auto font=CreateFontW(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto previous=SelectObject(dc,font); DrawTextW(dc,value,-1,&rect,flags|DT_NOPREFIX);
        SelectObject(dc,previous); DeleteObject(font);
    };
    RECT title{book.left+width/6,book.top+height/3,book.right-width/10,book.top+height*2/3};
    text(L"Your next\nchapter",title,(std::max)(10,height/12),FW_SEMIBOLD,DT_LEFT|DT_WORDBREAK);
    paint({title.left,book.top+height*2/3,title.left+width/3,book.top+height*2/3+(std::max)(1,height/150)},RGB(239,189,89));
    std::wstring type=extension; if(!type.empty() && type.front()==L'.') type.erase(0,1);
    std::transform(type.begin(),type.end(),type.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    if(type.empty()) type=L"BOOK";
    RECT label{title.left,book.bottom-height/5,book.right-width/10,book.bottom-height/10};
    text(type.c_str(),label,(std::max)(9,height/20),FW_SEMIBOLD,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    RestoreDC(dc,saved);
}
}

