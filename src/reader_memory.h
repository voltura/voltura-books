#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace books {
// A renderer budget, not a document-size limit. Leave most of the machine's
// available physical memory and commit headroom to Windows and other apps.
inline uint64_t readerMemoryBudget() {
    MEMORYSTATUSEX state{sizeof(state)};
    if(!GlobalMemoryStatusEx(&state))return 0;
    return (std::min)({state.ullAvailPhys/8,state.ullAvailPageFile/8,1ULL<<30});
}
inline bool readerMemoryPressure() {
    MEMORYSTATUSEX state{sizeof(state)};
    return !GlobalMemoryStatusEx(&state)||state.ullAvailPhys<state.ullTotalPhys/20||state.ullAvailPageFile<state.ullTotalPageFile/20;
}
}
