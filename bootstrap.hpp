#pragma once
#include "pe.hpp"
#include "bootstrap-names.hpp"
inline std::vector<std::pair<uint32_t,uint32_t>> idleThunks(const PE& image) {
 std::vector<std::pair<uint32_t,uint32_t>> out;
 auto imports=image.imports();
 for(auto name:idleImportNames){std::vector<uint32_t> iats;
  for(const auto& imp:imports)if(_stricmp(imp.dll.c_str(),"ChemDrawUI.dll")==0&&imp.name==name)iats.push_back(imp.iat);
  require(iats.size()==1,"Idle import is missing or ambiguous");std::vector<uint32_t> matches;
  for(auto s:image.sections)if(s.flags&IMAGE_SCN_MEM_EXECUTE)for(uint32_t i=0;i+6<=s.bytes;++i){size_t p=s.raw+i;if(image.b[p]==0xff&&image.b[p+1]==0x25){auto target=int64_t(s.rva)+i+6+get<int32_t>(image.b,p+2);if(target==iats.front())matches.push_back(s.rva+i);}}
  require(matches.size()==1,"Idle import thunk is missing or ambiguous");out.emplace_back(matches.front(),iats.front());
 }return out;
}
inline Bytes bootstrap(Bytes original) {
 PE image(std::move(original));auto targets=idleThunks(image);auto& b=image.b;
 uint32_t fa=get<uint32_t>(b,image.opt+36),sa=get<uint32_t>(b,image.opt+32),firstRaw=UINT32_MAX,end=0;
 for(auto s:image.sections){if(s.bytes)firstRaw=std::min(firstRaw,s.raw);end=std::max(end,s.rva+std::max(s.size,s.bytes));}
 auto count=uint16_t(image.sections.size());uint32_t headerEnd=uint32_t(image.table)+(count+2)*40;
 // Preserve descriptor bytes and original IAT RVAs; append a new initializer import.
 Bytes imports;uint32_t importRva=get<uint32_t>(b,image.opt+120);
 for(uint32_t i=0;i<4096;++i){auto p=image.offset(importRva+i*20,20);bool nonzero=false;for(size_t j=0;j<20;++j)nonzero|=b[p+j]!=0;if(!nonzero)break;imports.insert(imports.end(),b.begin()+p,b.begin()+p+20);require(i<4095,"Invalid import table");}
 if(headerEnd>firstRaw){uint32_t growth=alignTo(headerEnd-firstRaw,fa);
  auto debugRva=get<uint32_t>(b,image.opt+160),debugSize=get<uint32_t>(b,image.opt+164);require(debugSize%28==0,"Invalid debug directory");
  if(debugRva)for(uint32_t i=0;i<debugSize;i+=28){auto p=image.offset(debugRva+i,28)+24;auto raw=get<uint32_t>(b,p);if(raw>=firstRaw){require(raw<=UINT32_MAX-growth,"Debug offset overflow");put(b,p,raw+growth);}}
  for(auto s:image.sections)for(size_t field:{20,24,28}){auto raw=get<uint32_t>(b,s.header+field);if(raw>=firstRaw){require(raw<=UINT32_MAX-growth,"Section offset overflow");put(b,s.header+field,raw+growth);}}
  auto symbols=get<uint32_t>(b,image.pe+12);if(symbols>=firstRaw)put(b,image.pe+12,symbols+growth);
  b.insert(b.begin()+firstRaw,growth,0);put(b,image.opt+60,alignTo(headerEnd,fa));
 }
 uint32_t dataRva=alignTo(end,sa);Bytes data=imports;data.resize(data.size()+40);
 auto add=[&](Bytes bytes,uint32_t boundary){data.resize(alignTo(uint32_t(data.size()),boundary));uint32_t rva=dataRva+uint32_t(data.size());data.insert(data.end(),bytes.begin(),bytes.end());return rva;};
 auto ascii=[](const char* text){return Bytes(text,text+strlen(text)+1);};
 auto dll=add(ascii("ChemDrawLatency.dll"),1);Bytes func{0,0};auto name=ascii("Initialize");func.insert(func.end(),name.begin(),name.end());auto function=add(func,2);
 Bytes entries(16);put(entries,0,uint64_t(function));auto ilt=add(entries,8),iat=add(entries,8);
 put(data,imports.size(),ilt);put(data,imports.size()+12,dll);put(data,imports.size()+16,iat);
 uint32_t codeRva=alignTo(dataRva+uint32_t(data.size()),sa);Bytes code;
 auto rel32=[&](uint32_t target){int64_t delta=int64_t(target)-(int64_t(codeRva)+int64_t(code.size())+4);require(delta>=INT32_MIN&&delta<=INT32_MAX,"Bootstrap displacement overflow");size_t at=code.size();code.resize(at+4);put(code,at,int32_t(delta));};
 for(auto [thunk,nativeIat]:targets){code.resize(alignTo(uint32_t(code.size()),16));uint32_t stub=codeRva+uint32_t(code.size());
  for(int c:{0x51,0x52,0x41,0x50,0x41,0x51,0x48,0x83,0xec,0x28,0xff,0x15})code.push_back(static_cast<unsigned char>(c));rel32(iat);
  for(int c:{0x48,0x83,0xc4,0x28,0x41,0x59,0x41,0x58,0x5a,0x59,0xff,0x25})code.push_back(static_cast<unsigned char>(c));rel32(nativeIat);
  bool wrote=false;for(auto s:image.sections)if(thunk>=s.rva&&thunk+6<=s.rva+s.bytes){size_t at=get<uint32_t>(b,s.header+20)+thunk-s.rva;b[at]=0xe9;put(b,at+1,int32_t(int64_t(stub)-thunk-5));b[at+5]=0x90;wrote=true;break;}require(wrote,"Bootstrap thunk not in file");
 }
 auto section=[&](uint16_t index,const char* name,uint32_t rva,const Bytes& payload,uint32_t flags){auto raw=alignTo(uint32_t(b.size()),fa),size=alignTo(uint32_t(payload.size()),fa);b.resize(size_t(raw)+size);std::copy(payload.begin(),payload.end(),b.begin()+raw);size_t h=image.table+index*40;std::fill(b.begin()+h,b.begin()+h+40,static_cast<unsigned char>(0));memcpy(b.data()+h,name,strlen(name));put(b,h+8,uint32_t(payload.size()));put(b,h+12,rva);put(b,h+16,size);put(b,h+20,raw);put(b,h+36,flags);return size;};
 auto dataSize=section(count,".cdlati",dataRva,data,0xc0000040),codeSize=section(count+1,".cdlatx",codeRva,code,0x60000020);
 put(b,image.pe+6,uint16_t(count+2));put(b,image.opt+4,get<uint32_t>(b,image.opt+4)+codeSize);put(b,image.opt+8,get<uint32_t>(b,image.opt+8)+dataSize);put(b,image.opt+56,alignTo(codeRva+uint32_t(code.size()),sa));
 put(b,image.opt+64,uint32_t(0));put(b,image.opt+120,dataRva);put(b,image.opt+124,uint32_t(imports.size()+40));
 for(size_t directory:{4,11}){put(b,image.opt+112+directory*8,uint32_t(0));put(b,image.opt+116+directory*8,uint32_t(0));}
 return b;
}
