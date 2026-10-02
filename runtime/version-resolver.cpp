#include "version-resolver.hpp"
#include "../files.hpp"
#include "../signatures.hpp"
#include <unordered_map>
namespace cd {
namespace { std::unordered_map<uint64_t,uintptr_t> addresses; }
void preflightDetours() {
 addresses.clear();
 const std::pair<const wchar_t*,const char*> modules[]={
 {L"ChemDrawBase.dll","0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a"},
 {L"ChemDrawUI.dll","b9af968d2e75822fc47a4e5a4d2f348ea636804c712763b73b3536a30268a4f2"},
 {L"CoreChemistryCommon.dll","154bb6d63073e12628cc2372b9c9180ed780c78833bc3a26d8880f2b0abd639c"}};
 for(size_t index=0;index<std::size(modules);++index){auto [name,expected]=modules[index];auto h=GetModuleHandleW(name);require(h,"Required ChemDraw module not loaded");wchar_t path[32768]{};winCheck(GetModuleFileNameW(h,path,32768)!=0,"Get module filename");auto bytes=readFile(path);require(hashBytes(bytes)==expected,"Runtime private ABI profile mismatch");PE image(std::move(bytes));
  for(auto& s:signatures)if(wcscmp(s.module,name)==0){auto hits=scan(image,s);require(hits.size()==1,"Runtime detour discovery missing or ambiguous");auto rva=hits.front();require(rva==s.original,"Discovered detour moved outside the validated private ABI profile");auto pattern=hexBytes(s.bytes),mask=hexBytes(s.mask);auto* live=reinterpret_cast<const unsigned char*>(h)+rva-s.anchor;
   for(size_t i=0;i<pattern.size();++i)require((live[i]&mask[i])==(pattern[i]&mask[i]),"In-memory detour bytes conflict with another modification");
   addresses[(uint64_t(index)<<32)|s.original]=reinterpret_cast<uintptr_t>(h)+rva;
  }
 }
}
uintptr_t resolveDetour(const wchar_t* name,uint32_t referenceRva){uint64_t index=_wcsicmp(name,L"ChemDrawUI.dll")==0?1:0;auto i=addresses.find((index<<32)|referenceRva);require(i!=addresses.end(),"A detour has no verified signature mapping");return i->second;}
}
