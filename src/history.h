#pragma once
#include "core.h"

namespace books {
// Keeps the file read-locked between identification and submission.
class BookIdentity {
public:
    explicit BookIdentity(const fs::path& path);
    ~BookIdentity();
    BookIdentity(const BookIdentity&) = delete;
    BookIdentity& operator=(const BookIdentity&) = delete;
    std::string hash;
private:
    HANDLE file = INVALID_HANDLE_VALUE;
};
bool sentBefore(const fs::path& history, const std::string& hash, const std::wstring& recipient);
void recordSent(const fs::path& history, const std::string& hash, const std::wstring& recipient, const std::wstring& filename);
}
