#pragma once
#include <windows.h>
#include <cstdint>
namespace cd {
void preflightDetours();
uintptr_t resolveDetour(const wchar_t* module,uint32_t referenceRva);
}
