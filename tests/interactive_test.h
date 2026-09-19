#pragma once
#include <windows.h>
#include <cstdio>
// A direct executable/CTest invocation must also be safe on a working desktop.
inline bool interactiveTestsEnabled(){
    wchar_t value[2]{};
    if(GetEnvironmentVariableW(L"BOOKS_RUN_INTERACTIVE_TESTS",value,2)==1&&value[0]==L'1')return true;
    std::fputs("SKIP: interactive test. Use scripts/test-ui.ps1 with an explicit test scope.\n",stdout);
    return false;
}
