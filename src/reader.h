#pragma once
#include "cover.h"
#include <functional>
namespace books {
// UI-thread owner; asynchronous callbacks hold weak references to its implementation.
class Reader {
public:
    Reader(HWND dialog, HWND cover, std::function<void()> toggleFullscreen);
    ~Reader();
    void select(const std::filesystem::path& path);
    void previewReady(unsigned pages, bool hasCover);
    void imageLoading(uint64_t request);
    void imageLoaded(uint64_t request);
    void resize();
    void fullscreen(bool enabled);
    bool reading() const;
    bool canFullscreen() const;
    HWND window() const;
    void navigate(int direction);
    void openFullscreen();
private:
    friend struct ReaderTestAccess;
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
