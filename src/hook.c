/*
 * hook.c - macropad_hook.dll
 *
 * Windows loads this DLL into every (64-bit) GUI process because
 * macropad.exe installs it as a global WH_KEYBOARD hook. KeyboardProc then
 * runs inside whichever app is about to receive a keystroke, *after* the
 * keystroke has gone through Raw Input. That ordering is the whole trick:
 * by the time we get here, macropad.exe has already seen a WM_INPUT telling
 * it which physical device the key came from, so it can answer "block" or
 * "pass".
 *
 * Keep this file tiny: it lives in other people's processes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "config.h"
#include "shared.h"

/* Per-process cache of the exe's window. Each process that loads the DLL gets
 * its own copy of this variable. */
static HWND g_target;

__declspec(dllexport) LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    /* HC_ACTION = the app is removing a WM_KEYDOWN/WM_KEYUP from its queue.
     * Cheap check first: only the six pad keys ever cost a cross-process
     * round-trip; every other keystroke on the system falls straight through. */
    if (code == HC_ACTION && map_key((UINT)wParam)) {
        if (!g_target || !IsWindow(g_target))
            g_target = FindWindowExW(HWND_MESSAGE, NULL, MACROPAD_WINDOW_CLASS, NULL);

        DWORD_PTR block = 0;
        if (g_target &&
            SendMessageTimeoutW(g_target, WM_MACROPAD_QUERY, wParam, lParam,
                                SMTO_ABORTIFHUNG, 100, &block) &&
            block)
            return 1; /* non-zero = discard the keystroke */
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(inst); /* we don't care about thread attach/detach */
    return TRUE;
}
