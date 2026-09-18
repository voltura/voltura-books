#include "book_information.h"
#include "formats.h"
#include <vector>
namespace books {
using Row=std::pair<std::wstring,std::wstring>;
static std::wstring localFileDate(FILETIME time) {
    if(!time.dwHighDateTime && !time.dwLowDateTime) return {};
    SYSTEMTIME utc{},local{};
    if(!FileTimeToSystemTime(&time,&utc) || !SystemTimeToTzSpecificLocalTime(nullptr,&utc,&local)) return {};
    wchar_t date[128]{},clock[128]{};
    if(!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,&local,nullptr,date,128,nullptr) || !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&local,nullptr,clock,128)) return {};
    return std::wstring(date)+L"  "+clock;
}
static std::vector<Row> rows(const std::filesystem::path& path,const BookDetails& details,bool fileDates=false) {
    std::vector<Row> result;
    auto format=path.extension().wstring(); if(!format.empty()) format.erase(0,1);
    std::transform(format.begin(),format.end(),format.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    if(auto info=fileFormat(path.extension().wstring())) format+=L" - "+std::wstring(info->name);
    auto add=[&](const wchar_t* label,std::wstring value) {
        for(auto& c:value) if(c<32) c=L' ';
        if(value.size()>160) value=value.substr(0,157)+L"...";
        if(!value.empty()) result.emplace_back(label,std::move(value));
    };
    add(L"Title",details.title); add(L"Author",details.author); add(L"Publisher",details.publisher);
    if(details.pages) result.emplace_back(L"Pages",std::to_wstring(details.pages)+(details.savedPageCount ? L" (saved in file)" : L""));
    result.emplace_back(L"Format",format);
    std::error_code error; auto bytes=std::filesystem::file_size(path,error);
    if(!error) result.emplace_back(L"Size",readableFileSize(bytes));
    if(fileDates) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)) {
            add(L"Created",localFileDate(attributes.ftCreationTime));
            add(L"Modified",localFileDate(attributes.ftLastWriteTime));
        }
    }
    return result;
}
std::wstring bookInformationText(const std::filesystem::path& path,const BookDetails& details) {
    auto result=path.filename().wstring();
    for(const auto& [label,value]:rows(path,details)) result+=L"\n"+label+L": "+value;
    return result;
}
int drawBookInformation(HDC dc,RECT bounds,HFONT font,const std::filesystem::path& path,const BookDetails& details,bool paint,bool typeCard,HICON icon,bool prominentTitle,bool fileDates) {
    int saved=SaveDC(dc); SelectObject(dc,font); SetBkMode(dc,TRANSPARENT);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
    LOGFONTW log{}; GetObjectW(font,sizeof(log),&log); log.lfWeight=FW_SEMIBOLD;
    HFONT bold=CreateFontIndirectW(&log);
    log.lfHeight=MulDiv(log.lfHeight,4,3);
    HFONT heading=CreateFontIndirectW(&log);
    const int gap=(std::max)(3,static_cast<int>(metrics.tmHeight)/4);
    const int labelWidth=(std::min)(static_cast<int>(bounds.right-bounds.left)/3,static_cast<int>(metrics.tmAveCharWidth)*12);
    RECT title{bounds.left,bounds.top,bounds.right,bounds.top}; auto filename=path.filename().wstring();
    if(prominentTitle && !details.title.empty()) filename=details.title;
    if(typeCard || prominentTitle) SelectObject(dc,bold);
    if(prominentTitle && !details.title.empty()) SelectObject(dc,heading);
    DrawTextW(dc,filename.c_str(),-1,&title,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    if(paint) DrawTextW(dc,filename.c_str(),-1,&title,DT_WORDBREAK|DT_NOPREFIX);
    int y=title.bottom+metrics.tmHeight;
    if(prominentTitle && !details.title.empty()) {
        SelectObject(dc,font); RECT file{bounds.left,y,bounds.right,y}; auto name=path.filename().wstring();
        DrawTextW(dc,name.c_str(),-1,&file,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        if(paint) DrawTextW(dc,name.c_str(),-1,&file,DT_WORDBREAK|DT_NOPREFIX);
        y=file.bottom+metrics.tmHeight;
    }
    if(typeCard) {
        const int height=metrics.tmHeight*4;
        RECT card{bounds.left,y,bounds.right,y+height};
        if(paint) {
            const auto textColor=GetTextColor(dc);
            const bool dark=GetRValue(textColor)+GetGValue(textColor)+GetBValue(textColor)>384;
            auto cardBrush=CreateSolidBrush(dark ? RGB(43,43,43) : RGB(245,245,245));
            auto pen=CreatePen(PS_SOLID,(std::max)(1,static_cast<int>(metrics.tmHeight)/12),GetSysColor(COLOR_GRAYTEXT)); auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,cardBrush);
            RoundRect(dc,card.left,card.top,card.right,card.bottom,gap*3,gap*3);
            SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); DeleteObject(cardBrush);
            const int size=metrics.tmHeight*2;
            if(icon) DrawIconEx(dc,card.left+gap*3,card.top+(height-size)/2,icon,size,size,0,nullptr,DI_NORMAL);
            RECT text{card.left+size+gap*6,card.top+gap*2,card.right-gap*2,card.bottom};
            auto extension=path.extension().wstring().substr(1); std::transform(extension.begin(),extension.end(),extension.begin(),towupper);
            SelectObject(dc,bold); DrawTextW(dc,extension.c_str(),-1,&text,DT_SINGLELINE|DT_NOPREFIX);
            text.top+=metrics.tmHeight+gap; SelectObject(dc,font);
            if(auto format=fileFormat(path.extension().wstring())) DrawTextW(dc,format->name,-1,&text,DT_WORDBREAK|DT_NOPREFIX);
        }
        y=card.bottom+metrics.tmHeight;
    }
    for(const auto& [label,value]:rows(path,details,fileDates)) {
        if(typeCard && label==L"Format") continue;
        if(prominentTitle && label==L"Title") continue;
        RECT valueRect{bounds.left+labelWidth,y,bounds.right,y}; SelectObject(dc,font);
        DrawTextW(dc,value.c_str(),-1,&valueRect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        RECT labelRect{bounds.left,y,bounds.left+labelWidth-gap,valueRect.bottom};
        if(paint) {
            DrawTextW(dc,value.c_str(),-1,&valueRect,DT_WORDBREAK|DT_NOPREFIX);
            SelectObject(dc,bold); DrawTextW(dc,label.c_str(),-1,&labelRect,DT_WORDBREAK|DT_NOPREFIX);
        }
        y=valueRect.bottom+gap;
    }
    RestoreDC(dc,saved); DeleteObject(heading); DeleteObject(bold); return y-bounds.top;
}
}
