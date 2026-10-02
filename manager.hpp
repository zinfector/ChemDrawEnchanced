#pragma once
#include "transaction.hpp"
#include "bootstrap.hpp"
#include "signatures.hpp"
#include "features.hpp"
#include <tlhelp32.h>
#include <functional>
inline constexpr std::pair<const wchar_t*,const char*> originals[]={
 {L"ChemDraw.exe","7f178d50fd6b6243393173207be6b70899bdce12b02c8b7c6b94c5410c29fa0a"},
 {L"ChemDrawBase.dll","0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a"},
 {L"ChemDrawUI.dll","b9af968d2e75822fc47a4e5a4d2f348ea636804c712763b73b3536a30268a4f2"},
 {L"CoreChemistryCommon.dll","154bb6d63073e12628cc2372b9c9180ed780c78833bc3a26d8880f2b0abd639c"}
};
inline fs::path originalExe(const fs::path& folder){auto backup=folder/L"ChemDraw.exe.before-latency-fix";return fs::exists(backup)?backup:folder/L"ChemDraw.exe";}
struct Inspection {bool compatible{};uint32_t mask{};std::wstring text;};
inline uint32_t installedMask(const fs::path& folder){auto path=folder/L"ChemDrawLatency.ini";if(!fs::exists(folder/L"ChemDrawLatency.native-state"))return 0;uint32_t mask=0;for(size_t i=0;i<features.size();++i)if(GetPrivateProfileIntW(L"Patches",features[i].key,0,path.c_str()))mask|=1u<<i;return mask;}
inline Inspection inspect(const fs::path& folder){validateFolder(folder);Inspection result;result.mask=installedMask(folder);bool hashes=true;std::wostringstream report;report<<L"Automatic discovery: executable sections only; relative addresses masked.\r\n";
 for(auto [name,expected]:originals){auto p=std::wstring(name)==L"ChemDraw.exe"?originalExe(folder):folder/name;auto bytes=readFile(p);PE check(bytes);bool match=hashBytes(bytes)==expected;hashes&=match;report<<name<<L": "<<(match?L"validated ABI profile":L"unrecognized build")<<L"\r\n";}
 size_t found=0;for(const wchar_t* module:{L"ChemDrawBase.dll",L"ChemDrawUI.dll"}){PE image(readFile(folder/module));for(auto& s:signatures)if(wcscmp(module,s.module)==0){auto hits=scan(image,s);report<<s.module<<L"!"<<wide(s.name)<<L": ";if(hits.size()==1){++found;report<<L"RVA 0x"<<std::hex<<hits[0]<<std::dec;if(hashes&&hits[0]!=s.original)hashes=false;}else report<<(hits.empty()?L"missing":L"ambiguous");report<<L"\r\n";}}
 try{PE exe(readFile(originalExe(folder)));auto thunks=idleThunks(exe);for(auto [rva,iat]:thunks)report<<L"Idle bootstrap RVA 0x"<<std::hex<<rva<<L", IAT 0x"<<iat<<std::dec<<L"\r\n";}catch(const std::exception& e){hashes=false;report<<wide(e.what())<<L"\r\n";}
 result.compatible=hashes&&found==std::size(signatures);report<<found<<L"/"<<std::size(signatures)<<L" unique detour signatures.\r\n";
 report<<(result.compatible?L"Ready: ChemDraw 26.0.0.6141 x64 ABI verified.\r\n":L"Inspection only: private fields, vtables, globals and call ABIs need a verified profile for this build.\r\n");
 result.text=report.str();return result;
}
inline void noChemDraw(const fs::path& folder){Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));winCheck(snap.h!=INVALID_HANDLE_VALUE,"List running processes");PROCESSENTRY32W p{};p.dwSize=sizeof(p);if(Process32FirstW(snap.h,&p))do{if(_wcsicmp(p.szExeFile,L"ChemDraw.exe")!=0)continue;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,p.th32ProcessID));require(process.h!=nullptr,"A running ChemDraw process cannot be identified; close it first");wchar_t path[32768]{};DWORD size=32768;winCheck(QueryFullProcessImageNameW(process.h,0,path,&size),"Identify ChemDraw process");auto executable=fs::path(path);require(_wcsicmp(fs::weakly_canonical(executable.parent_path()).c_str(),folder.c_str())!=0,"Save documents and close ChemDraw from the selected installation first");}while(Process32NextW(snap.h,&p));}
struct FolderLock {Handle mutex;explicit FolderLock(const fs::path& folder):mutex(CreateMutexW(nullptr,FALSE,(L"Global\\ChemDrawNative-"+wide(hashBytes(textBytes(narrow(folder.wstring()))))).c_str())){winCheck(mutex.h!=nullptr,"Create patcher mutex");auto r=WaitForSingleObject(mutex.h,0);require(r==WAIT_OBJECT_0||r==WAIT_ABANDONED,"Another patcher is using this folder");}~FolderLock(){ReleaseMutex(mutex.h);}};
inline std::vector<std::pair<std::wstring,std::string>> records(const fs::path& path,const char* header){auto ls=lines(readFile(path));require(!ls.empty()&&ls[0]==header,"Invalid patch metadata");std::vector<std::pair<std::wstring,std::string>> out;for(size_t i=1;i<ls.size();++i){std::istringstream in(ls[i]);std::string name,hash;require(bool(in>>name>>hash),"Invalid metadata entry");auto w=wide(name);require(ownedName(w),"Invalid metadata filename");require(hash=="-"||hash.size()==64,"Invalid metadata hash");out.emplace_back(w,hash);}return out;}
inline void verifyState(const fs::path& folder){auto state=folder/L"ChemDrawLatency.native-state";if(!fs::exists(state))return;auto entries=records(state,"CDNativeState1");for(auto [name,expected]:entries){auto p=folder/name;auto actual=fs::exists(p)?hashBytes(readFile(p)):"-";require(actual==expected,"Installed patch files were modified externally; refusing to overwrite them");}}
inline void saveBaseline(const fs::path& folder){auto backup=folder/L".ChemDrawNativeBackup";if(fs::exists(backup)){require(!(GetFileAttributesW(backup.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Baseline directory is a link");records(backup/L"baseline.txt","CDNativeBaseline1");return;}auto pending=folder/L".ChemDrawNativeBackup.pending";require(!fs::exists(pending),"An incomplete baseline backup exists; inspect it before continuing");fs::create_directory(pending);
 try{std::string metadata="CDNativeBaseline1\n";for(size_t i=1;i<std::size(ownedNames)-1;++i){auto name=ownedNames[i];std::string hash="-";if(fs::exists(folder/name)){auto bytes=readFile(folder/name);hash=hashBytes(bytes);durableWrite(pending/name,bytes);}metadata+=narrow(name)+" "+hash+"\n";}durableWrite(pending/L"baseline.txt",textBytes(metadata));winCheck(MoveFileExW(pending.c_str(),backup.c_str(),MOVEFILE_WRITE_THROUGH),"Publish baseline backup");}catch(...){fs::remove_all(pending);throw;}
}
inline Bytes configuration(uint32_t mask){std::string s="[Patches]\r\nSchema=1\r\n";for(size_t i=0;i<features.size();++i)s+=narrow(features[i].key)+"="+((mask&(1u<<i))?"1":"0")+"\r\n";return textBytes(s);}
inline void apply(const fs::path& raw,uint32_t mask,bool uninstall=false){fs::path folder=fs::canonical(raw);validateFolder(folder);FolderLock lock(folder);noChemDraw(folder);require(!fs::exists(folder/L".ChemDrawNativeTransaction"),"An interrupted transaction exists. Recover it first");require(!(mask&~allFeatures)&&closeDependencies(mask)==mask,"Invalid feature selection or missing dependency");if(!mask)uninstall=true;
 auto original=readFile(originalExe(folder));require(hashBytes(original)==originals[0].second,"Original executable/backup is not the verified ChemDraw build");auto patched=bootstrap(original);auto current=readFile(folder/L"ChemDraw.exe");require(current==original||current==patched,"The executable has unrelated changes or was updated; refusing to overwrite it");verifyState(folder);
 std::vector<Change> changes;
 if(uninstall){require(fs::exists(folder/L"ChemDraw.exe.before-latency-fix"),"No original executable backup found");auto backup=folder/L".ChemDrawNativeBackup";
  if(fs::exists(folder/L"ChemDrawLatency.native-state")){require(fs::exists(backup),"Native baseline backup missing");for(auto [name,hash]:records(backup/L"baseline.txt","CDNativeBaseline1")){if(hash=="-")changes.push_back({name,std::nullopt});else{auto bytes=readFile(backup/name);require(hashBytes(bytes)==hash,"Baseline backup hash mismatch");changes.push_back({name,bytes});}}changes.push_back({L"ChemDrawLatency.native-state",std::nullopt});}
  changes.push_back({L"ChemDraw.exe",original});transact(folder,changes);return;
 }
 auto evidence=inspect(folder);require(evidence.compatible,"Build discovery could not verify the private ABI. See the inspection report");saveBaseline(folder);
 if(!fs::exists(folder/L"ChemDraw.exe.before-latency-fix"))durableWrite(folder/L"ChemDraw.exe.before-latency-fix",original);
 changes.push_back({L"ChemDrawLatency.dll",resource(101)});changes.push_back({L"ChemDrawLatency.ini",configuration(mask)});changes.push_back({L"ChemDrawLatency-MinHook-LICENSE.txt",resource(102)});
 auto engine=readFile(folder/L"CoreChemistryCommon.dll");for(const wchar_t* name:{L"ChemDrawLatencyChemistry.dll",L"ChemDrawLatencyChemistry2.dll",L"ChemDrawLatencyChemistry3.dll"}) { if(mask&(1u<<6))changes.push_back({name,engine});else {auto baseline=records(folder/L".ChemDrawNativeBackup"/L"baseline.txt","CDNativeBaseline1");for(auto [saved,hash]:baseline)if(saved==name){if(hash=="-")changes.push_back({saved,std::nullopt});else{auto bytes=readFile(folder/L".ChemDrawNativeBackup"/saved);require(hashBytes(bytes)==hash,"Worker baseline hash mismatch");changes.push_back({saved,bytes});}}}}
 std::string state="CDNativeState1\n";for(auto& c:changes)state+=narrow(c.name)+" "+(c.after?hashBytes(*c.after):"-")+"\n";state+="ChemDraw.exe "+hashBytes(patched)+"\n";
 // Executable is committed last, after every supporting file and state record.
 changes.push_back({L"ChemDrawLatency.native-state",textBytes(state)});changes.push_back({L"ChemDraw.exe",patched});transact(folder,changes);
}
