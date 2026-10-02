#pragma once
#include "../features.hpp"
namespace cd {
inline uint32_t selectedPatches=0;
inline bool patchEnabled(size_t i) { return (selectedPatches&(1u<<i))!=0; }
inline uint32_t patchMask() { return selectedPatches; }
inline void loadPatchOptions(HMODULE dll) {
 wchar_t filename[32768]{};
 if(!GetModuleFileNameW(dll,filename,32768))throw std::runtime_error("Cannot locate patch configuration");
 std::wstring path(filename);path.resize(path.find_last_of(L"\\/")+1);path+=L"ChemDrawLatency.ini";
 if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Missing patch configuration; no hooks enabled");
 if(GetPrivateProfileIntW(L"Patches",L"Schema",0,path.c_str())!=1)throw std::runtime_error("Unsupported patch configuration");
 for(size_t i=0;i<features.size();++i)if(GetPrivateProfileIntW(L"Patches",features[i].key,0,path.c_str()))selectedPatches|=1u<<i;
 if(closeDependencies(selectedPatches)!=selectedPatches)throw std::runtime_error("Unsatisfied patch dependencies");
}
}
