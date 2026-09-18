#pragma once
#include <string>
#include <algorithm>
namespace books {
struct FileFormat { const wchar_t* extension; const char* mime; const wchar_t* name; };
inline constexpr FileFormat FileFormats[]={
 {L".epub","application/epub+zip",L"Electronic Publication"},{L".pdf","application/pdf",L"Portable Document Format"},{L".rtf","application/rtf",L"Rich Text Format"},
 {L".txt","text/plain",L"Plain Text"},{L".html","text/html",L"HyperText Markup Language"},{L".htm","text/html",L"HyperText Markup Language"},
 {L".doc","application/msword",L"Microsoft Word Document"},{L".docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document",L"Microsoft Word Document"},
 {L".jpg","image/jpeg",L"JPEG Image"},{L".jpeg","image/jpeg",L"JPEG Image"},{L".png","image/png",L"Portable Network Graphics"},{L".gif","image/gif",L"Graphics Interchange Format"},{L".bmp","image/bmp",L"Bitmap Image"}
};
inline const FileFormat* fileFormat(std::wstring extension) {
    std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    for(const auto& format:FileFormats) if(extension==format.extension) return &format;
    return nullptr;
}
inline std::wstring readableFileSize(unsigned long long bytes) {
    if(bytes<1000) return std::to_wstring(bytes)+L" B";
    if(bytes<1000000) return std::to_wstring((bytes+500)/1000)+L" KB";
    auto tenths=(bytes+50000)/100000;
    return std::to_wstring(tenths/10)+(tenths%10 ? L"."+std::to_wstring(tenths%10) : L"")+L" MB";
}
inline constexpr wchar_t FileFilter[]=L"Books and documents\0*.epub;*.pdf;*.rtf;*.txt;*.html;*.htm;*.doc;*.docx;*.jpg;*.jpeg;*.png;*.gif;*.bmp\0\0";
}
