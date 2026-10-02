#pragma once
#include "files.hpp"
inline constexpr const wchar_t* ownedNames[]={L"ChemDraw.exe",L"ChemDrawLatency.dll",L"ChemDrawLatency.ini",L"ChemDrawLatency-MinHook-LICENSE.txt",L"ChemDrawLatencyChemistry.dll",L"ChemDrawLatencyChemistry2.dll",L"ChemDrawLatencyChemistry3.dll",L"ChemDrawLatency.native-state"};
struct Change {std::wstring name;std::optional<Bytes> after;};
inline bool ownedName(const std::wstring& s){for(auto name:ownedNames)if(s==name)return true;return false;}
inline void validateFolder(const fs::path& folder){require(fs::is_directory(folder),"Choose the installation folder");require(!(GetFileAttributesW(folder.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Choose the real installation folder, not a directory link");for(auto name:ownedNames){auto p=folder/name;auto a=GetFileAttributesW(p.c_str());require(a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_REPARSE_POINT),"A target is a filesystem link; no files changed");}}
struct JournalEntry {std::wstring name;std::string before,after;};
inline std::vector<JournalEntry> journalEntries(const fs::path& txn){std::vector<JournalEntry> entries;auto all=lines(readFile(txn/L"journal.txt"));require(!all.empty()&&all[0]=="CDNativeTransaction1","Invalid transaction journal");for(size_t i=1;i<all.size();++i){std::istringstream in(all[i]);std::string name,before,after;require(bool(in>>name>>before>>after),"Invalid transaction entry");auto w=wide(name);require(ownedName(w),"Invalid transaction target");entries.push_back({w,before,after});}return entries;}
inline void recover(const fs::path& folder){validateFolder(folder);auto txn=folder/L".ChemDrawNativeTransaction";if(!fs::exists(txn))return;require(!(GetFileAttributesW(txn.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Transaction directory is a link");auto entries=journalEntries(txn);
 // Check every target and rollback snapshot before changing any file.
 for(auto& e:entries){auto p=folder/e.name;std::string now=fs::exists(p)?hashBytes(readFile(p)):"-";require(now==e.before||now==e.after,"Recovery conflict: a target changed outside this patcher");if(e.before!="-")require(hashBytes(readFile(txn/(e.name+L".before")))==e.before,"Recovery backup hash mismatch");}
 for(auto i=entries.rbegin();i!=entries.rend();++i){auto p=folder/i->name;if(i->before=="-"){if(fs::exists(p))fs::remove(p);}else atomicWrite(p,readFile(txn/(i->name+L".before")));}
 fs::remove_all(txn);
}
inline void transact(const fs::path& folder,const std::vector<Change>& changes){validateFolder(folder);auto txn=folder/L".ChemDrawNativeTransaction";require(!fs::exists(txn),"An interrupted transaction exists. Use Recover first");fs::create_directory(txn);bool prepared=false;
 try {std::string journal="CDNativeTransaction1\n";for(auto& c:changes){require(ownedName(c.name),"Unknown transaction target");auto p=folder/c.name;std::string before="-",after="-";if(fs::exists(p)){auto b=readFile(p);before=hashBytes(b);durableWrite(txn/(c.name+L".before"),b);}if(c.after){after=hashBytes(*c.after);durableWrite(txn/(c.name+L".after"),*c.after);}journal+=narrow(c.name)+" "+before+" "+after+"\n";}
  durableWrite(txn/L"journal.txt",textBytes(journal));prepared=true;
  // A deny-write handle blocks ordinary image loading while dependencies change.
  Handle gate(CreateFileW((folder/L"ChemDraw.exe").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));winCheck(gate.h!=INVALID_HANDLE_VALUE,"Lock executable; close ChemDraw first");
  for(auto& c:changes){if(c.name==L"ChemDraw.exe")gate.close();auto p=folder/c.name;if(c.after)replaceFile(txn/(c.name+L".after"),p);else if(fs::exists(p))fs::remove(p);}
  fs::remove_all(txn);
 }catch(...){auto error=std::current_exception();if(prepared){try{recover(folder);}catch(const std::exception& recovery){throw std::runtime_error(std::string("Transaction failed; automatic rollback needs recovery: ")+recovery.what());}}else fs::remove_all(txn);std::rethrow_exception(error);}
}
