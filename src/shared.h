/*
 * shared.h - the tiny "protocol" between macropad.exe and macropad_hook.dll.
 */
#ifndef MACROPAD_SHARED_H
#define MACROPAD_SHARED_H

/* Class name of the exe's hidden message-only window. The hook DLL finds the
 * exe by looking this up with FindWindowExW(HWND_MESSAGE, ...). */
#define MACROPAD_WINDOW_CLASS L"MacroPad.MessageWindow"

/* Hook DLL -> exe: "should I swallow this keystroke?"
 *   wParam = virtual-key code
 *   lParam = the keystroke lParam the hook received (bit 31 = key up)
 *   return 1 = block it, 0 = let it through */
#define WM_MACROPAD_QUERY (WM_APP + 1)

#define MACROPAD_HOOK_DLL L"macropad_hook.dll"

#endif
