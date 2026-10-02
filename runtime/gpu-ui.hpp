#pragma once
#include "runtime.hpp"

namespace cd {
void installGpuUi();
void markGpuDocument(HWND);
void invalidateCanvas(HWND);
void redrawCanvas(HWND);
}
