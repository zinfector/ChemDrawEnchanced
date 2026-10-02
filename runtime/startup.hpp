#pragma once
#include "runtime.hpp"

namespace cd {
void beginStartupPreparation();
void finishStartupPreparation() noexcept;
void cancelStartupPreparation() noexcept;
bool supportedStartupHash(HMODULE,const char*) noexcept;
}
