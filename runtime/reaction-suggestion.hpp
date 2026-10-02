#pragma once
#include "gpu-preview.hpp"
#include <string>

namespace cd {
inline constexpr UINT_PTR reactionSuggestionTimer=0xcd98f1;
class ReactionCameraScope {
public:
    ReactionCameraScope() noexcept;
    ~ReactionCameraScope();
};
struct ReactionLabel {
    Point position{}; // Baseline in document coordinates.
    std::wstring text,font{L"Arial"};
    double size{};
};
struct ReactionPreview {
    GhostArtwork artwork;
    std::vector<ReactionLabel> labels;
    RectD bounds{};
    uint64_t identity{};
};
void installReactionSuggestions();
void reactionObjectChanged(Obj) noexcept;
void reactionPageChanged(Obj) noexcept;
void forgetReactionPage(Obj) noexcept;
void idleReactionSuggestions(Obj) noexcept;
bool reactionSuggestionShortcut(Obj,UINT,WPARAM,LPARAM) noexcept;
bool reactionSuggestionQueuedInput(MSG&) noexcept;
std::shared_ptr<const ReactionPreview> reactionSuggestionPreview(Obj) noexcept;
}
