#pragma once
#include <cstdint>
#include <array>
struct Feature { const wchar_t* key; const wchar_t* name; const wchar_t* detail; uint32_t dependencies; };
// Shared state owners stay together; leaf changes can be deselected separately.
inline constexpr std::array<Feature,20> features{{
 {L"Canvas",L"GPU canvas, navigation and drag presentation",L"Shared rendering, hover, hit geometry, buffer and object-lifetime hooks",0},
 {L"ModalWait",L"Bounded modal tracking waits",L"Return promptly on input/deadlines without dispatching unrelated edits",1},
 {L"SpatialIndex",L"Spatial hit-test acceleration",L"Indexed candidate lookup; native lookup remains when disabled",1},
 {L"AllocationPool",L"Rendering allocation pool",L"Reuse temporary rendering nodes; native allocation remains when disabled",1},
 {L"BondPlacement",L"Freeze bond placement to the visible preview",L"Commit the exact direction shown at mouse-down",1},
 {L"Placement",L"Deferred placement chemistry",L"Batch placement analysis at native idle boundaries",1},
 {L"ChemistryWorker",L"Background chemistry workers",L"Use isolated copies of the installation's native chemistry engine",1|(1<<5)},
 {L"Arrows",L"Native arrow attachments and curve editing",L"Snapping, persistent links, curve handles and matching arrow ghosts",1|(1<<1)},
 {L"Alignment",L"Smart alignment and equal spacing",L"Prepared alignment index, snapping, guides and spacing arrows",1|(1<<1)},
 {L"Undo",L"Undo camera and empty-history fixes",L"Reject empty history records; publish edits and reveal offscreen targets",1},
 {L"Menus",L"GPU menus and refresh coalescing",L"Owner-drawn menu presentation and unchanged-state filtering",1},
 {L"Toolbars",L"Toolbar repaint fixes",L"Complete button frames and in-place toolbar refresh",1},
 {L"HistoryTrace",L"Passive history diagnostics",L"Bounded toolbar/history metadata log, without document contents",1},
 {L"RapidHistoryClicks",L"Accept every rapid Undo/Redo toolbar click",L"Route double-click presses through the native ordinary press path",1|(1<<11)},
 {L"NavigationUndo",L"Exclude zoom and pan from undo history",L"Use native recording suppression during camera commits",1},
 {L"StartupBanner",L"Dismiss startup banner when ready",L"Native splash lifecycle and timeout after bitmap publication",1},
 {L"ArrowInsertion",L"Insert molecules into reaction arrows",L"Split a supported shaft into two native arrows in the move transaction; Alt bypasses",1|(1<<7)|(1<<8)},
 {L"ReactionSuggestions",L"Paired-electron reaction suggestions",L"Independent product preview; Ctrl+Enter accepts and Esc dismisses",1|(1<<7)},
 {L"DrawingSnap",L"Align drawing and ghost placement",L"Arrow endpoints, neighboring rays, equal spacing and frozen click placement",1|(1<<8)},
 {L"ArrowInsertionTrace",L"Passive arrow insertion diagnostics",L"Record native drag stages and insertion rejection reasons",1|(1<<16)}
}};
inline constexpr uint32_t allFeatures=(1u<<features.size())-1;
inline uint32_t closeDependencies(uint32_t mask) { uint32_t last;do {last=mask;for(size_t i=0;i<features.size();++i)if(mask&(1u<<i))mask|=features[i].dependencies;}while(last!=mask);return mask; }
inline uint32_t removeDependents(uint32_t mask,size_t disabled) {
 mask&=~(1u<<disabled);uint32_t last;
 do {last=mask;for(size_t i=0;i<features.size();++i)if((mask&features[i].dependencies)!=features[i].dependencies)mask&=~(1u<<i);}while(last!=mask);return mask;
}
