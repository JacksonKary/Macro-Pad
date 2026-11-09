# MacroPad

A tiny, windowless Windows background program that turns a cheap 3-key + knob USB macro pad into media controls, **without affecting any other keyboard**.

| Pad input   | Pad sends | MacroPad turns it into |
|-------------|-----------|------------------------|
| Key 1       | `A`       | Previous track         |
| Key 2       | `B`       | Play / Pause           |
| Key 3       | `C`       | Next track             |
| Knob ⟲ (CCW)| `1`       | Volume down            |
| Knob press  | `2`       | Mute                   |
| Knob ⟳ (CW) | `3`       | Volume up              |

An `A` typed on a regular keyboard still types `A`. Only keys coming from the pad are remapped.

It's written in plain C against the Win32 API, with no frameworks or runtimes and about 350 lines in total. The two binaries come to roughly 27 KB. While idle it uses no CPU: the program sleeps inside the kernel until a key event wakes it.

---

## Quick start

```powershell
# 1. Build (needs a C compiler, see "Building" below)
.\build.cmd

# 2. Check that the pad is detected (shows [PAD] next to it)
.\build\macropad.exe --list | Out-Host

# 3. Run it now and start it automatically at every logon
.\build\macropad.exe --install
```

| Command | What it does |
|---|---|
| `macropad.exe` | Run in the background (no window, no tray icon). |
| `macropad.exe --list` | Print every keyboard device, then for 20 s print which device each key press comes from. |
| `macropad.exe --install` | Add to `HKCU\...\CurrentVersion\Run` so it starts at logon, then start. |
| `macropad.exe --uninstall` | Remove from startup and stop the running copy. |
| `macropad.exe --quit` | Stop the running copy. |

> Keep `macropad_hook.dll` in the same folder as `macropad.exe`. If the folder is moved, run `--install` again so the startup entry points at the new location.

To change the mappings or the device, edit **`src/config.h`** and rebuild.

---

## The problem, and why it's harder than it looks

The pad is a **USB HID keyboard**. Windows doesn't care that it's small: it merges it with every other keyboard into one shared input stream. By the time an app gets a `WM_KEYDOWN` for `A`, the information about *which* keyboard sent it is gone.

Remapping only the pad requires two things:

1. **Identify** which physical device each keystroke came from.
2. **Block** the pad's original keystroke (so `a` doesn't get typed into whatever is focused) and send a media key instead.

Windows has no single user-mode API that does both:

| API | Knows the device? | Can block the key? |
|---|---|---|
| **Raw Input** (`WM_INPUT`) | ✅ yes, gives a device handle | ❌ no, read-only |
| **Low-level hook** (`WH_KEYBOARD_LL`) | ❌ no | ✅ yes, but it runs *before* Raw Input, so it can't know the device yet |
| **Keyboard hook** (`WH_KEYBOARD`) | ❌ no | ✅ yes, and it runs *after* Raw Input |

The trick, which is also what LuaMacros did under the hood, is to **combine Raw Input with a `WH_KEYBOARD` hook**, relying on the order Windows processes things in.

---

## How it works

### The life of one key press

```
 Pad: key 1 is pressed (sends 'A')
   │
   ▼
 Windows "Raw Input Thread" (inside the kernel/win32k)
   │
   ├──► (1) WM_INPUT posted to macropad.exe ─────────────┐
   │        "VK 'A' down, from device \\?\HID#VID_514C…" │
   │                                                     ▼
   │                                   macropad.exe: is_pad()? yes, mapped? yes
   │                                     • pending list += { 'A', down }
   │                                     • SendInput(VK_MEDIA_PREV_TRACK)
   │
   └──► (2) WM_KEYDOWN 'A' posted to the focused app (e.g. Notepad)
              │
              ▼
          Notepad calls GetMessage(); before it gets the message, Windows calls
          our WH_KEYBOARD hook, which runs INSIDE Notepad (macropad_hook.dll)
              │
              │  SendMessageTimeout(macropad.exe, WM_MACROPAD_QUERY, 'A', down)
              ▼
          macropad.exe: is { 'A', down } in pending list? yes, remove it, return 1
              │
              ▼
          hook returns 1, so Windows discards the WM_KEYDOWN. Notepad never sees 'A'.
```

A normal `A` from a regular keyboard follows the same path, except that at step (1) `is_pad()` says no, nothing goes on the pending list, the hook's query returns 0, and Notepad gets its `A` as usual.

### Piece by piece

#### 1. A message-only window (`create_window`)

Win32 delivers almost everything (raw input, hook queries, "please quit") as **window messages**, so even an invisible program needs a window to receive them. We create it with `HWND_MESSAGE` as the parent. That makes it a *message-only window*: it can never be shown, has no taskbar button, and is skipped by `EnumWindows`. It's basically a mailbox.

The exe is linked with `-mwindows` (`/SUBSYSTEM:WINDOWS` on MSVC). That means it's a GUI-subsystem program, so Windows never creates a console for it. That's why nothing pops up at startup.

#### 2. Raw Input (`register_raw_input`, `on_raw_input`)

```c
rid.usUsagePage = 0x01;   // HID Generic Desktop
rid.usUsage     = 0x06;   // Keyboard
rid.dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
RegisterRawInputDevices(&rid, 1, sizeof rid);
```

* **Usage page / usage** are HID-spec numbers. 1/6 means "keyboards". (1/2 would be mice.)
* **`RIDEV_INPUTSINK`** asks for input even when our window isn't in the foreground, which it never is.
* **`RIDEV_DEVNOTIFY`** sends `WM_INPUT_DEVICE_CHANGE` when a keyboard is plugged in or removed.

Every keystroke from every keyboard now arrives as `WM_INPUT`. `GetRawInputData` fills a `RAWINPUT` struct whose `header.hDevice` is a handle to the physical device, and `data.keyboard.VKey` / `Flags` give the key and whether it's going down or up.

#### 3. Identifying the pad (`is_pad`)

`GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, …)` turns the handle into a device path, for example:

```
\\?\HID#VID_514C&PID_8851&MI_01&Col01#9&28656cf4&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}
        └──┬───┘ └──┬───┘ └─┬─┘ └─┬─┘ └──────┬───────┘ └───────────────┬────────────────────┘
        vendor   product  USB   HID      instance ID           device-interface class
          ID       ID    iface  collection (made up by Windows)   GUID (= "keyboard")
```

**This is why the original Lua script broke when moved to a new PC.** It matched the *whole* path, and the instance part (`8&3CBA26A&0&0000` on one PC, `9&28656cf4&0&0000` on another) is generated by Windows from the USB port and machine. The **VID/PID** (`514C` / `8851`) is burned into the pad's firmware and never changes, so MacroPad matches only `VID_514C&PID_8851`. That's also why `--list` shows the pad twice (`Col01` and `Col03`): it exposes several HID collections, and matching on VID/PID covers all of them.

Looking up a name costs a kernel call plus a string search, so `is_pad` keeps the yes/no answer per handle in a small array (`g_cache`). That array is cleared on `WM_INPUT_DEVICE_CHANGE`, because handles can be reused after an unplug.

#### 4. Doing the action (`press_vk`)

```c
in[0].ki.wVk     = VK_MEDIA_PLAY_PAUSE;
in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;           // down
in[1]            = in[0];
in[1].ki.dwFlags |= KEYEVENTF_KEYUP;                // up
SendInput(2, in, sizeof(INPUT));
```

`SendInput` injects keystrokes as if they came from a keyboard. Media and volume keys are ordinary virtual keys (`VK_MEDIA_*`, `VK_VOLUME_*`), handled system-wide by Windows' shell. They need `KEYEVENTF_EXTENDEDKEY` because on real keyboards they send an `E0`-prefixed scan code. The volume overlay and media controls work exactly as they do for a real media key.

Our own injected keys also come back to us as `WM_INPUT`, but with `hDevice == NULL`. `is_pad(NULL)` returns false, so we can't trigger ourselves in a loop.

The action fires on **key-down**. Holding a key makes the pad auto-repeat, and the action repeats with it, which is the desired behavior for volume.

#### 5. The hook DLL (`hook.c`)

```c
g_hook = SetWindowsHookExW(WH_KEYBOARD, KeyboardProc, dllHandle, 0);
```

With thread id `0` this is a **global hook**. Windows *loads `macropad_hook.dll` into every GUI process* on the desktop (of the same bitness) and calls `KeyboardProc` inside that process each time it pulls a keyboard message off its queue. That's why the hook code has to live in a DLL rather than the exe.

Because this code runs inside *every* app, it is written to be as cheap as possible:

* For any key that isn't one of the six pad keys (`map_key()` is a 6-entry loop), it calls `CallNextHookEx` straight away and does nothing else.
* Only for `A/B/C/1/2/3` does it ask the exe, using `SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, 100 ms)`. The timeout ensures that a hung or dead MacroPad can never freeze typing in other apps.
* It finds the exe's window with `FindWindowExW(HWND_MESSAGE, NULL, "MacroPad.MessageWindow", NULL)` and caches the handle (`IsWindow` re-checks it in case the exe restarted).

32-bit apps can't load a 64-bit DLL. For those, Windows instead switches back to our exe's thread to run the hook, so they're covered too, just a little slower.

#### 6. The pending list (`pending_add`, `pending_take`)

This is the glue between steps 2 and 5. When the pad sends `A`-down, the exe records `{ 'A', down, timestamp }`. When the hook asks "block `A`-down?", the exe looks for a matching entry, removes it, and says yes. Key-up events are tracked separately (bit 31 of the hook's `lParam` is the up/down flag).

Two edge cases are handled:

* **The hook asks before we've read the `WM_INPUT`.** The raw input message is already *in* our queue (Windows posts it first), but we may not have processed it yet. The query handler drains pending `WM_INPUT` messages with `PeekMessageW(..., WM_INPUT, WM_INPUT, PM_REMOVE)` and checks again.
* **The hook never asks**, for example when the focused window is an admin window (see Limitations). The entries expire after 1 second so they can't later swallow a real `A` from a regular keyboard.

#### 7. The message loop

```c
while (GetMessageW(&msg, NULL, 0, 0) > 0)
    DispatchMessageW(&msg);
```

`GetMessage` blocks inside the kernel until a message arrives. There's no polling, no `sleep()`, and no timer, so the process uses **0% CPU** until a key is pressed. Each key press costs a few microseconds of work.

#### 8. Running at startup (`set_autostart`)

`--install` writes one string value:

```
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
    MacroPad = "C:\path\to\macropad.exe"
```

Windows runs everything in that key at logon. It's per-user and needs no admin rights. The entry appears, and can be toggled, in **Task Manager → Startup apps**.

#### 9. Single instance

`CreateMutexW(NULL, TRUE, L"Local\\MacroPad.SingleInstance")` creates a named kernel object. If it already exists (`ERROR_ALREADY_EXISTS`), another copy is running and this one exits. Windows releases the mutex automatically when the process dies.

---

## Project layout

```
MacroPad/
├── build.cmd          build script (MSVC or MinGW clang/gcc)
├── src/
│   ├── config.h       ← device ID + key mappings (the file to edit)
│   ├── shared.h       window class name + message ID shared by exe and DLL
│   ├── macropad.c     the exe: raw input, pending list, actions, CLI
│   └── hook.c         the DLL: WH_KEYBOARD hook that asks the exe "block?"
├── legacy/
│   └── MacroPad.lua   the original LuaMacros script this replaces
└── build/             output (git-ignored)
```

## Building

Either compiler works, and `build.cmd` picks whichever it finds:

* **LLVM-MinGW** (small, no Visual Studio needed):
  `winget install MartinStorsjo.LLVM-MinGW.UCRT`, then open a new terminal and run `.\build.cmd`
* **MSVC**: install the *Desktop development with C++* workload in Visual Studio, open a **Developer PowerShell**, and run `.\build.cmd`

`build.cmd` runs `macropad.exe --quit` first. While MacroPad is running, the DLL is loaded into other apps and Windows locks the file. If the build still can't overwrite the DLL, some app is still holding it; waiting a moment, or logging out and back in, releases it.

## Customizing

Everything is in `src/config.h`:

```c
#define MACROPAD_DEVICE_ID L"VID_514C&PID_8851"

static const KeyMapping KEY_MAP[] = {
    { 'A', VK_MEDIA_PREV_TRACK },
    ...
};
```

* **A different pad:** run `macropad.exe --list | Out-Host`, press the pad's keys, find the line tagged with the right device, and copy its `VID_xxxx&PID_xxxx` into `MACROPAD_DEVICE_ID`.
* **Different actions:** any [virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) works as a target, e.g. `VK_F13`–`VK_F24` (keys no app uses by default, handy for binding in other software), or `VK_MEDIA_STOP`.
* **More pad keys:** add rows. Both the exe and the DLL read the same table, so they stay in sync.

## Limitations (and why)

* **Admin (elevated) windows**: Windows' *UIPI* security stops a normal process from injecting a hook DLL into an elevated one (Task Manager, an admin terminal, some game launchers). When one of those is focused, MacroPad still sends the media key, but the original `a` also gets through. Workaround: run MacroPad elevated from Task Scheduler ("Run with highest privileges", trigger "At log on") instead of `--install`.
* **Apps that read Raw Input directly** (some games) see the pad's `a` regardless, because Raw Input can't be blocked from user mode.
* **Windows Store / sandboxed (AppContainer) apps** may not load global hook DLLs. Most desktop apps, including Spotify, browsers, Discord and editors, are fine.
* The only way to remove *all* of these limits is a kernel filter driver (e.g. the *Interception* driver), which is a much bigger and riskier project.

### Alternative: no software at all

Many of these VID `514C` pads can be **reprogrammed** so the firmware itself sends media keys. Most report media keys as HID "Consumer Control" usages. The vendor's configuration tool or open-source tools like [`ch57x-keyboard-tool`](https://github.com/kriomant/ch57x-keyboard-tool) do this, if they support the specific model. Then no program is needed on any PC. This project is the software approach, and a good way to learn how Windows input works.

## Further reading

* [Raw Input overview](https://learn.microsoft.com/windows/win32/inputdev/raw-input)
* [`SetWindowsHookEx`](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowshookexw) and [KeyboardProc](https://learn.microsoft.com/windows/win32/winmsg/keyboardproc)
* [Message-only windows](https://learn.microsoft.com/windows/win32/winmsg/window-features#message-only-windows)
* [`SendInput`](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendinput)
* [Combining Raw Input and keyboard hook to selectively block input from multiple keyboards](https://www.codeproject.com/Articles/716591/Combining-Raw-Input-and-keyboard-Hook-to-selective): the article this technique is usually credited to
