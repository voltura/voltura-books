#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace books {
struct BookDetails {
    std::wstring title, author, publisher;
    unsigned pages = 0;
    bool savedPageCount = false;
};
BookDetails loadBookDetails(const std::filesystem::path& file) noexcept;
// Best-effort local thumbnail. Caller owns the returned bitmap; nullptr means no cover.
HBITMAP loadCover(const std::filesystem::path& epub, int width, int height) noexcept;
}
