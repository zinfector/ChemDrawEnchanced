#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <limits>
using Bytes=std::vector<unsigned char>;
inline void require(bool good,const char* message) { if(!good)throw std::runtime_error(message); }
template<class T> inline T get(const Bytes& b,size_t p) { require(p<=b.size()&&sizeof(T)<=b.size()-p,"Truncated PE structure");T v;memcpy(&v,b.data()+p,sizeof(v));return v; }
template<class T> inline void put(Bytes& b,size_t p,T v) { require(p<=b.size()&&sizeof(T)<=b.size()-p,"PE write exceeds file bounds");memcpy(b.data()+p,&v,sizeof(v)); }
inline uint32_t alignTo(uint32_t n,uint32_t a) { require(a&&!(a&(a-1)),"Invalid PE alignment");require(n<=UINT32_MAX-a+1,"PE alignment overflow");return (n+a-1)&~(a-1); }
struct Section {uint32_t rva,size,raw,bytes,flags;size_t header;};
class PE {
public:
 Bytes b;size_t pe,opt,table;std::vector<Section> sections;
 explicit PE(Bytes bytes):b(std::move(bytes)) {
  require(get<uint16_t>(b,0)==0x5a4d,"Missing MZ header");pe=get<uint32_t>(b,0x3c);
  require(get<uint32_t>(b,pe)==0x4550&&get<uint16_t>(b,pe+4)==0x8664,"Requires an x64 PE image");
  opt=pe+24;require(get<uint16_t>(b,pe+20)>=240&&get<uint16_t>(b,opt)==0x20b,"Requires PE32+");
  require(get<uint32_t>(b,opt+108)>=16,"Missing PE directories");
  table=opt+get<uint16_t>(b,pe+20);auto count=get<uint16_t>(b,pe+6);require(count&&count<=94,"Invalid section count");
  for(size_t i=0;i<count;++i){auto h=table+i*40;Section s{get<uint32_t>(b,h+12),get<uint32_t>(b,h+8),get<uint32_t>(b,h+20),get<uint32_t>(b,h+16),get<uint32_t>(b,h+36),h};
   require(s.raw<=b.size()&&s.bytes<=b.size()-s.raw,"Section exceeds file bounds");require(uint64_t(s.rva)+std::max(s.size,s.bytes)<=UINT32_MAX,"Section RVA overflow");sections.push_back(s);}
 }
 size_t offset(uint32_t rva,size_t bytes=1)const {
  for(auto s:sections)if(rva>=s.rva&&uint64_t(rva)+bytes<=uint64_t(s.rva)+s.bytes)return size_t(s.raw)+rva-s.rva;
  require(uint64_t(rva)+bytes<=get<uint32_t>(b,opt+60)&&uint64_t(rva)+bytes<=b.size(),"RVA has no file backing");return rva;
 }
 std::string stringAt(uint32_t rva)const {std::string result;for(size_t i=0;i<4096;++i){auto c=get<char>(b,offset(rva+uint32_t(i)));if(!c)return result;result+=c;}throw std::runtime_error("Unterminated PE string");}
 struct Import {std::string dll,name;uint32_t iat;};
 std::vector<Import> imports()const {
  std::vector<Import> out;auto root=get<uint32_t>(b,opt+120);require(root,"Missing import table");
  for(uint32_t j=0;j<4096;++j){auto p=offset(root+j*20,20);auto ilt=get<uint32_t>(b,p),name=get<uint32_t>(b,p+12),iat=get<uint32_t>(b,p+16);
   if(!ilt&&!name&&!iat)return out;require(name&&iat,"Invalid import descriptor");auto dll=stringAt(name);if(!ilt)ilt=iat;
   for(uint32_t i=0;i<100000;++i){auto v=get<uint64_t>(b,offset(ilt+i*8,8));if(!v)break;if(!(v>>63)){require(v<=UINT32_MAX-2,"Invalid import name RVA");out.push_back({dll,stringAt(uint32_t(v)+2),iat+i*8});}if(i==99999)throw std::runtime_error("Import list too long");}
  }throw std::runtime_error("Unterminated import descriptors");
 }
};
struct Signature {const wchar_t* module;uint32_t original;const char* name;const char* prototype;const char* source;int anchor;const char* bytes;const char* mask;};
inline Bytes hexBytes(const char* value){Bytes b;for(size_t i=0;value[i];i+=2){require(value[i+1],"Odd hex string");auto nib=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid hex byte");};b.push_back(static_cast<unsigned char>(nib(value[i])*16+nib(value[i+1])));}return b;}
inline std::vector<uint32_t> scan(const PE& pe,const Signature& signature) {
 auto bytes=hexBytes(signature.bytes),mask=hexBytes(signature.mask);require(bytes.size()==mask.size()&&!bytes.empty(),"Invalid signature");
 size_t first=0;while(first<mask.size()&&mask[first]!=255)++first;require(first<mask.size(),"Signature has no fixed byte");
 std::vector<uint32_t> matches;
 for(auto s:pe.sections)if(s.flags&IMAGE_SCN_MEM_EXECUTE){if(s.bytes<bytes.size())continue;
  const auto* begin=pe.b.data()+s.raw;size_t cursor=0;
  while(cursor<=s.bytes-bytes.size()) {auto* hit=static_cast<const unsigned char*>(memchr(begin+cursor+first,bytes[first],s.bytes-bytes.size()-cursor+1));if(!hit)break;size_t start=size_t(hit-begin)-first;
   bool good=true;for(size_t i=0;i<bytes.size();++i)if((begin[start+i]&mask[i])!=(bytes[i]&mask[i])){good=false;break;}
   if(good){int64_t rva=int64_t(s.rva)+int64_t(start)+signature.anchor;if(rva>=0&&rva<=UINT32_MAX)matches.push_back(uint32_t(rva));if(matches.size()>=32)return matches;}
   cursor=start+1;
  }
 }return matches;
}
