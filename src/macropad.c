/*
 * macropad.c - macropad.exe
 *
 * Turns the keys of one specific USB keyboard (the macro pad) into media
 * keys, without touching any other keyboard. Runs with no visible window.
 *
 *   macropad.exe              run in the background (normal mode)
 *   macropad.exe --list       show keyboards + which device each key comes from
 *   macropad.exe --install    run at logon (HKCU Run key), then start
 *   macropad.exe --uninstall  remove from logon and stop the running copy
 *   macropad.exe --quit       stop the running copy
 *
 * How it works (see README.md for the long version):
 *   1. Raw Input (WM_INPUT) tells us *which device* produced each key.
 *      If it's the pad and the key is mapped, we inject the media key and
 *      remember "swallow the next 'A' down" in a small pending list.
 *   2. A WH_KEYBOARD hook (macropad_hook.dll) runs inside the app that is
 *      about to receive the key, and asks us (WM_MACROPAD_QUERY) whether to
 *      swallow it. We answer from the pending list.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include "config.h"
#include "shared.h"

#define LIST_WINDOW_CLASS L"MacroPad.ListWindow"
#define RUN_KEY           L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE         L"MacroPad"
#define PENDING_TTL_MS    1000   /* forget a pad key the hook never asked about */
#define LIST_SECONDS      20

static BOOL g_listMode;
static HHOOK g_hook;

/* ------------------------------------------------------------------ output */
/* A GUI-subsystem exe has no console. For --list we borrow the console of
 * whoever launched us (or write to a pipe/file if output is redirected). */

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void out(const wchar_t *fmt, ...)
{
    if (g_out == INVALID_HANDLE_VALUE) {
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!g_out || g_out == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        }
        if (!g_out || g_out == INVALID_HANDLE_VALUE) { g_out = NULL; }
    }
    if (!g_out) return;

    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfW(buf, fmt, ap); /* user32's printf: %s %d %x, no floats */
    va_end(ap);

    DWORD written, mode;
    if (GetConsoleMode(g_out, &mode)) {
        WriteConsoleW(g_out, buf, (DWORD)n, &written, NULL);
    } else { /* pipe or file: write UTF-8 */
        char utf8[4096];
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8, sizeof utf8, NULL, NULL);
        WriteFile(g_out, utf8, (DWORD)len, &written, NULL);
    }
}

/* --------------------------------------------------------- device matching */

/* Case-insensitive "does haystack contain needle". */
static BOOL contains_i(const wchar_t *hay, const wchar_t *needle)
{
    int n = lstrlenW(needle), h = lstrlenW(hay);
    for (int i = 0; i + n <= h; i++)
        if (CompareStringOrdinal(hay + i, n, needle, n, TRUE) == CSTR_EQUAL)
            return TRUE;
    return FALSE;
}

static BOOL device_name(HANDLE dev, wchar_t *name, UINT cch)
{
    return dev && GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name, &cch) != (UINT)-1;
}

/* Asking Windows for a device's name on every keystroke would be wasteful, so
 * we remember the answer per device handle. Cleared on any plug/unplug. */
typedef struct { HANDLE dev; BOOL isPad; } DevCache;
static DevCache g_cache[16];
static UINT g_cacheCount;

static BOOL is_pad(HANDLE dev)
{
    if (!dev) return FALSE; /* NULL = injected input (e.g. our own SendInput) */

    for (UINT i = 0; i < g_cacheCount; i++)
        if (g_cache[i].dev == dev) return g_cache[i].isPad;

    wchar_t name[512];
    BOOL pad = device_name(dev, name, ARRAYSIZE(name)) && contains_i(name, MACROPAD_DEVICE_ID);

    if (g_cacheCount == ARRAYSIZE(g_cache)) g_cacheCount = 0;
    g_cache[g_cacheCount].dev = dev;
    g_cache[g_cacheCount].isPad = pad;
    g_cacheCount++;
    return pad;
}

/* ------------------------------------------------------------ pending keys */
/* Keystrokes we've decided to swallow but the hook hasn't asked about yet. */

typedef struct { UINT vk; BOOL up; ULONGLONG time; } Pending;
static Pending g_pending[32];
static UINT g_pendingCount;

static void pending_add(UINT vk, BOOL up)
{
    if (g_pendingCount == ARRAYSIZE(g_pending)) /* full: drop the oldest */
        g_pending[0] = g_pending[--g_pendingCount];
    Pending p = { vk, up, GetTickCount64() };
    g_pending[g_pendingCount++] = p;
}

/* Find, remove and report a matching entry. Also expires stale ones (keys
 * that went to a window our hook can't reach, e.g. an admin window). */
static BOOL pending_take(UINT vk, BOOL up)
{
    ULONGLONG now = GetTickCount64();
    BOOL found = FALSE;
    for (UINT i = 0; i < g_pendingCount; ) {
        Pending *p = &g_pending[i];
        BOOL stale = now - p->time > PENDING_TTL_MS;
        BOOL match = !found && p->vk == vk && p->up == up;
        if (stale || match) {
            if (match && !stale) found = TRUE;
            *p = g_pending[--g_pendingCount]; /* swap-remove; order doesn't matter */
        } else {
            i++;
        }
    }
    return found;
}

/* ------------------------------------------------------------- the action */

static void press_vk(BYTE vk)
{
    INPUT in[2] = { 0 };
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY; /* media keys are "extended" (E0-prefixed) */
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void on_raw_input(HRAWINPUT hri)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(hri, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return;
    if (ri.header.dwType != RIM_TYPEKEYBOARD)
        return;

    UINT vk = ri.data.keyboard.VKey;
    BOOL up = (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0;

    if (g_listMode) {
        if (up) return;
        wchar_t name[512];
        if (!device_name(ri.header.hDevice, name, ARRAYSIZE(name)))
            lstrcpyW(name, L"(injected / unknown)");
        out(L"  vk 0x%02X  %s%s\r\n", vk,
            contains_i(name, MACROPAD_DEVICE_ID) ? L"[PAD] " : L"", name);
        return;
    }

    BYTE action = map_key(vk);
    if (!action || !is_pad(ri.header.hDevice))
        return;

    pending_add(vk, up);   /* tell the hook to swallow the original key */
    if (!up)
        press_vk(action);  /* fire on key-down; held key = auto-repeat = keeps going */
}

/* --------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        on_raw_input((HRAWINPUT)lParam);
        break; /* fall through to DefWindowProc, which frees the input */

    case WM_INPUT_DEVICE_CHANGE:
        g_cacheCount = 0; /* handles may be reused after unplug/replug */
        return 0;

    case WM_MACROPAD_QUERY: {
        UINT vk = (UINT)wParam;
        BOOL up = ((DWORD)lParam >> 31) & 1; /* bit 31 of keystroke lParam = transition state */
        if (pending_take(vk, up))
            return 1;
        /* The WM_INPUT for this key is normally already processed, but it may
         * still be sitting in our queue. Drain WM_INPUT and look again. */
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_INPUT, WM_INPUT, PM_REMOVE))
            DispatchMessageW(&m);
        return pending_take(vk, up);
    }

    case WM_TIMER: /* --list timeout */
        DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hook) UnhookWindowsHookEx(g_hook);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND create_window(HINSTANCE inst, const wchar_t *cls)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc)) return NULL;
    /* HWND_MESSAGE parent = message-only window: never visible, no taskbar
     * entry, not enumerated by EnumWindows. Just a mailbox. */
    return CreateWindowExW(0, cls, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
}

static BOOL register_raw_input(HWND hwnd)
{
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; /* HID "Generic Desktop" page */
    rid.usUsage = 0x06;     /* Keyboard */
    /* INPUTSINK: deliver even though we never have focus.
     * DEVNOTIFY: send WM_INPUT_DEVICE_CHANGE on plug/unplug. */
    rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget = hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof rid);
}

/* --------------------------------------------------------------- commands */

static void list_keyboards(void)
{
    UINT count = 0;
    GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST));
    RAWINPUTDEVICELIST devs[64];
    if (count > ARRAYSIZE(devs)) count = ARRAYSIZE(devs);
    count = GetRawInputDeviceList(devs, &count, sizeof(RAWINPUTDEVICELIST));

    out(L"\r\nKeyboards (pad match = \"%s\"):\r\n", MACROPAD_DEVICE_ID);
    for (UINT i = 0; i < count && count != (UINT)-1; i++) {
        if (devs[i].dwType != RIM_TYPEKEYBOARD) continue;
        wchar_t name[512];
        if (!device_name(devs[i].hDevice, name, ARRAYSIZE(name))) continue;
        out(L"  %s%s\r\n", contains_i(name, MACROPAD_DEVICE_ID) ? L"[PAD] " : L"      ", name);
    }
    out(L"\r\nPress keys for %d seconds to see which device sends them...\r\n", LIST_SECONDS);
}

static void quit_running_instance(void)
{
    HWND h = FindWindowExW(HWND_MESSAGE, NULL, MACROPAD_WINDOW_CLASS, NULL);
    if (h) PostMessageW(h, WM_CLOSE, 0, 0);
}

static BOOL set_autostart(BOOL enable)
{
    if (!enable)
        return RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE) == ERROR_SUCCESS;

    wchar_t path[MAX_PATH], cmd[MAX_PATH + 2];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wsprintfW(cmd, L"\"%s\"", path);
    return RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                           (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static BOOL install_hook(void)
{
    /* The DLL must sit next to the exe. */
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    lstrcpyW(path + n, MACROPAD_HOOK_DLL);

    HMODULE dll = LoadLibraryW(path);
    if (!dll) return FALSE;
    HOOKPROC proc = (HOOKPROC)(void *)GetProcAddress(dll, "KeyboardProc");
    if (!proc) return FALSE;
    /* threadId 0 = global: Windows maps the DLL into every desktop process
     * of the same bitness. 32-bit apps get marshalled back to this thread. */
    g_hook = SetWindowsHookExW(WH_KEYBOARD, proc, dll, 0);
    return g_hook != NULL;
}

/* ------------------------------------------------------------------ main */

static BOOL has_arg(const wchar_t *cmdline, const wchar_t *flag)
{
    return contains_i(cmdline, flag);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)show;

    if (has_arg(cmdline, L"--quit")) {
        quit_running_instance();
        return 0;
    }
    if (has_arg(cmdline, L"--uninstall")) {
        set_autostart(FALSE);
        quit_running_instance();
        out(L"MacroPad removed from startup and stopped.\r\n");
        return 0;
    }

    HWND hwnd;
    if (has_arg(cmdline, L"--list")) {
        g_listMode = TRUE;
        list_keyboards();
        hwnd = create_window(inst, LIST_WINDOW_CLASS);
        if (!hwnd || !register_raw_input(hwnd)) return 1;
        SetTimer(hwnd, 1, LIST_SECONDS * 1000, NULL);
    } else {
        if (has_arg(cmdline, L"--install")) {
            BOOL ok = set_autostart(TRUE);
            out(ok ? L"MacroPad will now start at logon. Starting it now.\r\n"
                   : L"Failed to write the Run registry key.\r\n");
        }

        /* One instance only. The named mutex lives as long as this process. */
        CreateMutexW(NULL, TRUE, L"Local\\MacroPad.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

        hwnd = create_window(inst, MACROPAD_WINDOW_CLASS);
        if (!hwnd) return 1;
        /* If we're ever run elevated, still accept queries from normal apps. */
        ChangeWindowMessageFilterEx(hwnd, WM_MACROPAD_QUERY, MSGFLT_ALLOW, NULL);
        if (!register_raw_input(hwnd) || !install_hook()) {
            MessageBoxW(NULL, L"MacroPad failed to start: could not register raw input "
                              L"or load " MACROPAD_HOOK_DLL L".",
                        L"MacroPad", MB_ICONERROR);
            return 1;
        }
    }

    /* The message loop. GetMessage sleeps in the kernel until something
     * arrives, so the program uses 0% CPU while idle. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}
