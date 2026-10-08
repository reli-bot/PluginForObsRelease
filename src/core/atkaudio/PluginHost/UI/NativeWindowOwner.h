#pragma once

namespace atk
{
// Makes the top-level native window `window` owned by `owner` (Windows: GWLP_HWNDPARENT), so it
// stays above its owner and minimises with it. No-op on other platforms or for null handles.
void setNativeWindowOwner(void* window, void* owner);
} // namespace atk
