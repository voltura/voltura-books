#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace books {
struct VerifiedUpdate { std::wstring version, name, sha256; size_t size{}; };
VerifiedUpdate verifyUpdate(const std::string& manifest, const std::vector<unsigned char>& signature,
    const std::string& publicKey, const std::wstring& current);
bool newerVersion(const std::wstring& candidate, const std::wstring& current);
struct UpdateResult { std::wstring message; std::filesystem::path installer; std::wstring sha256; };
UpdateResult downloadUpdate();
bool updateFileMatches(const std::filesystem::path& file, const std::wstring& sha256);
}
