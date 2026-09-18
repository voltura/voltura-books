#include "file_icons.h"
#include "formats.h"
#include <shellapi.h>
namespace books {
static HICON documentIcon(const std::wstring& label,COLORREF color) {
    constexpr int size=96;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=size; info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr; auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap) return nullptr;
    ZeroMemory(pixels,size*size*4);
    auto dc=CreateCompatibleDC(nullptr); auto previous=SelectObject(dc,bitmap);
    auto brush=CreateSolidBrush(color); auto oldBrush=SelectObject(dc,brush); auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    POINT page[]={{18,4},{61,4},{80,23},{80,91},{18,91}}; Polygon(dc,page,5);
    auto fold=CreateSolidBrush(RGB((GetRValue(color)+255)/2,(GetGValue(color)+255)/2,(GetBValue(color)+255)/2)); SelectObject(dc,fold);
    POINT corner[]={{61,4},{61,23},{80,23}}; Polygon(dc,corner,3);
    auto line=CreatePen(PS_SOLID,3,RGB(210,232,255)); SelectObject(dc,line);
    if(label!=L"PDF") {
        MoveToEx(dc,29,33,nullptr); LineTo(dc,68,33);
        MoveToEx(dc,29,41,nullptr); LineTo(dc,60,41);
    }
    auto font=CreateFontW(label.size()>3 ? -19 : -25,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");
    auto oldFont=SelectObject(dc,font); SetTextColor(dc,RGB(255,255,255)); SetBkMode(dc,TRANSPARENT);
    if(label==L"PDF") {
        // A self-contained PDF ribbon: independent of the user's file association.
        auto ribbon=CreatePen(PS_SOLID,3,RGB(255,255,255)); SelectObject(dc,ribbon);
        const POINT curve[]={
            {30,75},{22,77},{37,60},{46,39},
            {55,15},{37,17},{47,43},
            {57,68},{79,67},{70,59},
            {63,52},{34,60},{30,75}};
        PolyBezier(dc,curve,static_cast<DWORD>(sizeof(curve)/sizeof(curve[0])));
        SelectObject(dc,line); DeleteObject(ribbon);
    } else {
        RECT text{18,50,80,84}; DrawTextW(dc,label.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    GdiFlush();
    auto colors=static_cast<DWORD*>(pixels); for(int i=0;i<size*size;++i) if(colors[i]&0x00ffffff) colors[i]|=0xff000000;
    SelectObject(dc,oldFont); SelectObject(dc,oldBrush); SelectObject(dc,oldPen); SelectObject(dc,previous);
    DeleteObject(font); DeleteObject(line); DeleteObject(fold); DeleteObject(brush); DeleteDC(dc);
    BYTE maskBits[size*size/8]{}; auto mask=CreateBitmap(size,size,1,1,maskBits);
    ICONINFO icon{}; icon.fIcon=TRUE; icon.hbmColor=bitmap; icon.hbmMask=mask;
    auto result=mask ? CreateIconIndirect(&icon) : nullptr;
    DeleteObject(mask); DeleteObject(bitmap); return result;
}
HICON loadFileTypeIcon(const std::filesystem::path& path) {
    auto extension=path.extension().wstring();
    std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
    if(fileFormat(extension)) {
        COLORREF color=RGB(38,112,190);
        if(extension==L".epub") color=RGB(39,128,77);
        else if(extension==L".pdf") color=RGB(220,42,48);
        else if(extension==L".txt") color=RGB(92,106,123);
        else if(extension==L".html" || extension==L".htm") color=RGB(169,88,31);
        else if(extension==L".jpg" || extension==L".jpeg" || extension==L".png" || extension==L".gif" || extension==L".bmp") color=RGB(123,77,177);
        auto label=extension.substr(1);
        std::transform(label.begin(),label.end(),label.begin(),towupper);
        if(auto icon=documentIcon(label,color)) return icon;
    }
    SHFILEINFOW info{};
    SHGetFileInfoW(path.extension().c_str(),FILE_ATTRIBUTE_NORMAL,&info,sizeof(info),SHGFI_USEFILEATTRIBUTES|SHGFI_ICON|SHGFI_LARGEICON);
    return info.hIcon;
}
}
