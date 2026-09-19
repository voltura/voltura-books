#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include <memory>

namespace books {
struct BookDetails {
    std::wstring title, author, publisher;
    unsigned pages = 0;
    bool savedPageCount = false;
};
BookDetails loadBookDetails(const std::filesystem::path& file) noexcept;
// Best-effort local thumbnail. Caller owns the returned bitmap; nullptr means no cover.
HBITMAP loadCover(const std::filesystem::path& epub, int width, int height) noexcept;
HBITMAP loadPdfPage(const std::filesystem::path& file, unsigned page, int width, int height) noexcept;
struct EpubResources {
    std::string package;
    std::string coverImage;
    std::map<std::string, std::vector<BYTE>> files;
};
std::shared_ptr<EpubResources> loadEpubResources(const std::filesystem::path& file) noexcept;
}
