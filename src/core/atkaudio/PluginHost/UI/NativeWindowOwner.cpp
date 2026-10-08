#include "NativeWindowOwner.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void atk::setNativeWindowOwner(void* window, void* owner)
{
#ifdef _WIN32
    auto hwnd = static_cast<HWND>(window);
    auto ownerHwnd = static_cast<HWND>(owner);

    if (hwnd == nullptr || ownerHwnd == nullptr || !IsWindow(hwnd) || !IsWindow(ownerHwnd))
        return;

    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
#else
    (void)window;
    (void)owner;
#endif
}
