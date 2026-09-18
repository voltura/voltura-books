#pragma once
#include "core.h"

namespace books {
struct MailSetup {
    std::wstring host, guidance;
    int port = 465;
    bool startTls = false;
};
struct PasswordHelp {
    std::wstring label, instructions, linkLabel, url;
};
PasswordHelp passwordHelp(std::wstring host);
MailSetup parseMailSetup(const std::string& xml);
MailSetup discoverMailSetup(const std::wstring& domain) noexcept;
}
