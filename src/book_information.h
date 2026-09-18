#pragma once
#include "cover.h"
#include <string>
namespace books {
int drawBookInformation(HDC dc, RECT bounds, HFONT font, const std::filesystem::path& path, const BookDetails& details, bool paint, bool typeCard=false, HICON icon=nullptr, bool prominentTitle=false, bool fileDates=false);
std::wstring bookInformationText(const std::filesystem::path& path,const BookDetails& details);
}
