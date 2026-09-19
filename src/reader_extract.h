#pragma once
#include "text_reader.h"
#include <vector>
#include <memory>

namespace books {
struct ReaderTextFile {
    std::filesystem::path path;
    ~ReaderTextFile(){if(!path.empty())DeleteFileW(path.c_str());}
};
std::shared_ptr<ReaderTextFile> extractDocxText(const std::filesystem::path& source,const std::function<bool()>& cancelled);
// Text-only fallback is deliberately separate from formatted rendering. It never
// creates a DOM/Rich Edit document, activates objects or decodes embedded images.
inline std::shared_ptr<ReaderTextFile> extractReaderText(const std::filesystem::path& source,bool rtf,const std::function<bool()>& cancelled) {
    auto extension=source.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
    if(extension==L".docx")return extractDocxText(source,cancelled);
    try {
        wchar_t temp[32768]{};if(!GetTempPathW(32768,temp))return {};
        wchar_t name[32768]{};if(!GetTempFileNameW(temp,L"vbr",0,name))return {};
        auto result=std::make_shared<ReaderTextFile>();result->path=name;
        std::ofstream output(result->path,std::ios::binary|std::ios::trunc);if(!output)return {};
        const wchar_t bom=0xfeff;output.write(reinterpret_cast<const char*>(&bom),sizeof(bom));
        bool wrote=false;size_t steps=0;bool cancelledWork=false;
        auto put=[&](wchar_t c){if(c){output.write(reinterpret_cast<const char*>(&c),sizeof(c));wrote=true;}};
        std::ifstream raw;
        std::optional<TextChunk> chunk;size_t position=0;uint64_t index=0;
        if(rtf){raw.open(source,std::ios::binary);char magic[5]{};raw.read(magic,5);if(raw.gcount()!=5||memcmp(magic,"{\\rtf",5))return {};raw.seekg(0);}
        int pending=-1;
        auto get=[&]() -> int {
            if((++steps%4096)==0&&(!output||cancelled())){cancelledWork=true;return -1;}
            if(pending>=0){auto c=pending;pending=-1;return c;}
            if(rtf)return raw.get();
            if(!chunk||position==chunk->text.size()){
                if(chunk&&chunk->last)return -1;
                chunk=loadReaderTextChunk(source,index++,cancelled);position=0;
                if(!chunk){cancelledWork=true;return -1;}
            }
            return chunk->text[position++];
        };
        if(rtf) {
            struct Group{bool skip=false;int uc=1;};Group group;std::vector<Group> stack;int fallback=0,codePage=1252;
            auto literal=[&](int c){if(fallback){--fallback;return;}if(!group.skip){char byte=static_cast<char>(c);wchar_t wide=0;if(MultiByteToWideChar(codePage,0,&byte,1,&wide,1))put(wide);}};
            for(int c=get();c>=0;c=get()) {
                if(c=='{'){if(stack.size()>=512)return {};stack.push_back(group);}
                else if(c=='}'){if(stack.empty())return {};group=stack.back();stack.pop_back();}
                else if(c=='\\'){
                    int next=get();if(next<0)return {};
                    if(next=='*'){group.skip=true;continue;}
                    if(next=='\''){auto hex=[](int v){if(v>='0'&&v<='9')return v-'0';if(v>='a'&&v<='f')return v-'a'+10;if(v>='A'&&v<='F')return v-'A'+10;return -1;};int a=hex(get()),b=hex(get());if(a<0||b<0)return {};literal(a*16+b);continue;}
                    if(!isalpha(static_cast<unsigned char>(next))){if(next=='\\'||next=='{'||next=='}')literal(next);else if(next=='~')literal(' ');continue;}
                    std::string word;do{if(word.size()<64)word+=static_cast<char>(next);next=get();}while(next>=0&&isalpha(static_cast<unsigned char>(next)));
                    bool negative=next=='-';if(negative)next=get();int64_t value=0;
                    while(next>='0'&&next<='9'){value=value*10+next-'0';if(value>INT_MAX)return {};next=get();}
                    if(negative)value=-value;if(next!=' ')pending=next;
                    if(word=="bin"){if(value<0)return {};for(int64_t i=0;i<value;++i)if(get()<0)return {};continue;}
                    if(word=="fonttbl"||word=="colortbl"||word=="stylesheet"||word=="info"||word=="pict"||word=="object"||word=="fldinst"||word=="header"||word=="footer"||word=="listtable"||word=="listoverridetable"||word=="datastore"||word=="themedata")group.skip=true;
                    else if(word=="uc"){if(value<0||value>32)return {};group.uc=static_cast<int>(value);}
                    else if(word=="ansicpg"){if(value>0&&IsValidCodePage(static_cast<UINT>(value)))codePage=static_cast<int>(value);}
                    else if(word=="u"){if(!group.skip)put(static_cast<wchar_t>(value));fallback=group.uc;}
                    else if(!group.skip){if(word=="par"||word=="line"||word=="row")put(L'\n');else if(word=="tab"||word=="cell")put(L'\t');else if(word=="emdash")put(0x2014);else if(word=="endash")put(0x2013);else if(word=="bullet")put(0x2022);}
                }else if(c!='\r'&&c!='\n')literal(c);
            }
            if(!stack.empty())return {};
        } else {
            std::wstring hidden;bool space=false;
            auto emit=[&](wchar_t c){if(!hidden.empty())return;if(iswspace(c)){if(!space)put(L' ');space=true;}else{put(c);space=false;}};
            for(int c=get();c>=0;c=get()) {
                if(c=='<'){
                    std::wstring tag;int quote=0;bool comment=false;int previous=0,before=0;
                    for(int v=get();v>=0;v=get()){
                        if(tag.size()<256)tag+=static_cast<wchar_t>(v);
                        if(tag==L"!--")comment=true;
                        if(comment){if(v=='>'&&previous=='-'&&before=='-')break;}
                        else {if(quote){if(v==quote)quote=0;}else if(v=='\''||v=='"')quote=v;else if(v=='>')break;}
                        before=previous;previous=v;
                    }
                    if(comment)continue;
                    std::transform(tag.begin(),tag.end(),tag.begin(),towlower);
                    bool closing=tag.starts_with(L"/");size_t start=closing?1:0,end=start;while(end<tag.size()&&iswalnum(tag[end]))++end;
                    auto word=tag.substr(start,end-start);
                    if(closing&&word==hidden)hidden.clear();
                    else if(!closing&&hidden.empty()&&(word==L"script"||word==L"style"||word==L"head"||word==L"template"))hidden=word;
                    if(hidden.empty()&&(word==L"p"||word==L"div"||word==L"br"||word==L"li"||word==L"tr"||word==L"h1"||word==L"h2"||word==L"h3"||word==L"section")){put(L'\n');space=true;}
                }else if(c=='&'&&hidden.empty()){
                    std::wstring entity;int v=get();while(v>=0&&v!=';'&&entity.size()<32&&!iswspace(static_cast<wint_t>(v))&&v!='<'){entity+=static_cast<wchar_t>(v);v=get();}
                    if(v==';'){
                        if(entity==L"amp")emit('&');else if(entity==L"lt")emit('<');else if(entity==L"gt")emit('>');else if(entity==L"quot")emit('"');else if(entity==L"apos"||entity==L"#39")emit('\'');else if(entity==L"nbsp")emit(' ');
                        else if(entity.starts_with(L"#")){wchar_t* tail=nullptr;bool hex=entity.size()>1&&(entity[1]=='x'||entity[1]=='X');auto value=wcstoul(entity.c_str()+(hex?2:1),&tail,hex?16:10);if(tail&&!*tail&&value&&value<=0x10ffff&&!(value>=0xd800&&value<=0xdfff)){if(value>0xffff){value-=0x10000;emit(static_cast<wchar_t>(0xd800+(value>>10)));emit(static_cast<wchar_t>(0xdc00+(value&1023)));}else emit(static_cast<wchar_t>(value));}}
                        else {emit('&');for(auto ch:entity)emit(ch);emit(';');}
                    }else {emit('&');for(auto ch:entity)emit(ch);pending=v;}
                }else emit(static_cast<wchar_t>(c));
            }
        }
        output.flush();if(!output||!wrote||cancelledWork||cancelled())return {};output.close();return result;
    }catch(...){return {};}
}
}
