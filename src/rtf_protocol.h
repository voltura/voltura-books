#pragma once
#include <cstdint>
namespace books {
// Private pipe protocol. A response is followed by width*height BGRA pixels.
struct RtfRequest { int32_t width=0,height=0,dpi=96,anchor=0,previous=0; };
struct RtfResponse { int32_t width=0,height=0,next=0,length=0,anchor=0; };
}
