#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <functional>
#include <algorithm>

namespace books {
// Fixed source windows keep the decoder and edit control bounded, including
// single enormous lines. Boundaries preserve code points and CRLF pairs.
inline constexpr uint64_t ReaderTextWindow=32*1024;
struct TextChunk { std::wstring text; uint64_t index=0; bool last=true; UINT encoding=0; };
inline std::optional<TextChunk> loadReaderTextChunk(const std::filesystem::path& path,uint64_t index=0,const std::function<bool()>& cancelled=[] {return false;},UINT encoding=0) noexcept {
    try {
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return {};
        auto length=file.tellg();if(length<=0||cancelled())return {};
        uint64_t size=static_cast<uint64_t>(length);char signature[4]{};file.seekg(0);file.read(signature,4);file.clear();
        auto a=static_cast<unsigned char>(signature[0]),b=static_cast<unsigned char>(signature[1]);
        bool utf16=(a==255&&b==254)||(a==254&&b==255),big=a==254;
        bool bom=a==239&&b==187&&static_cast<unsigned char>(signature[2])==191;
        uint64_t skip=utf16?2:bom?3:0;
        if(size<=skip||index>(size-skip-1)/ReaderTextWindow||(utf16&&(size-skip)%2))return {};
        bool utf8=encoding!=1252;
        if(!utf16&&!bom&&!encoding) {
            std::string sample(static_cast<size_t>((std::min)(size,ReaderTextWindow+4)),0);file.seekg(0);file.read(sample.data(),sample.size());file.clear();
            if(sample.size()<size){while(!sample.empty()&&(static_cast<unsigned char>(sample.back())&0xc0)==0x80)sample.pop_back();if(!sample.empty()&&static_cast<unsigned char>(sample.back())>=0xc0)sample.pop_back();}
            utf8=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,sample.data(),static_cast<int>(sample.size()),nullptr,0)>0;
        }
        auto boundary=[&](uint64_t value) {
            if(value>=size)return size;
            if(value<=skip)return skip;
            file.clear();file.seekg(value);char bytes[4]{};file.read(bytes,4);file.clear();
            if(utf16){auto c=big?(static_cast<unsigned char>(bytes[0])<<8)|static_cast<unsigned char>(bytes[1]):static_cast<unsigned char>(bytes[0])|(static_cast<unsigned char>(bytes[1])<<8);if(c>=0xdc00&&c<=0xdfff)value+=2;}
            else if(utf8){for(int i=0;i<3&&(static_cast<unsigned char>(bytes[i])&0xc0)==0x80;++i)++value;}
            if(value<size){file.seekg(value-(utf16?2:1));char pair[4]{};file.read(pair,utf16?4:2);file.clear();if(utf16){if((big&&pair[0]==0&&pair[1]=='\r'&&pair[2]==0&&pair[3]=='\n')||(!big&&pair[0]=='\r'&&pair[1]==0&&pair[2]=='\n'&&pair[3]==0))value+=2;}else if(pair[0]=='\r'&&pair[1]=='\n')++value;}
            return (std::min)(value,size);
        };
        auto first=boundary(skip+index*ReaderTextWindow),end=boundary(skip+(index+1)*ReaderTextWindow);
        if(first>=end)return {};
        std::string bytes(static_cast<size_t>(end-first),0);file.seekg(first);if(!file.read(bytes.data(),bytes.size())||cancelled())return {};
        std::wstring decoded;
        if(utf16){for(size_t i=0;i<bytes.size();i+=2){auto x=static_cast<unsigned char>(bytes[i]),y=static_cast<unsigned char>(bytes[i+1]);decoded+=static_cast<wchar_t>(big?(x<<8)|y:x|(y<<8));}if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,decoded.data(),static_cast<int>(decoded.size()),nullptr,0,nullptr,nullptr))return {};}
        else {auto code=utf8?CP_UTF8:1252;auto flags=utf8?MB_ERR_INVALID_CHARS:0;int count=MultiByteToWideChar(code,flags,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);if(!count){if(utf8&&!bom)return loadReaderTextChunk(path,index,cancelled,1252);return {};}decoded.resize(count);MultiByteToWideChar(code,flags,bytes.data(),static_cast<int>(bytes.size()),decoded.data(),count);}
        TextChunk chunk;chunk.index=index;chunk.last=end==size;chunk.encoding=utf16?0:utf8?CP_UTF8:1252;chunk.text.reserve(decoded.size());
        for(size_t i=0;i<decoded.size();++i){auto c=decoded[i];if(!c)return {};if(c==L'\r'){if(i+1<decoded.size()&&decoded[i+1]==L'\n')++i;chunk.text+=L"\r\n";}else if(c==L'\n')chunk.text+=L"\r\n";else chunk.text+=c;}
        return chunk;
    }catch(...){return {};}
}
inline std::optional<std::wstring> loadReaderText(const std::filesystem::path& path) noexcept {
    auto chunk=loadReaderTextChunk(path);if(!chunk)return {};return std::move(chunk->text);
}
}

