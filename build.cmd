@echo off
rem Builds build\macropad.exe and build\macropad_hook.dll (64-bit).
rem Uses MSVC (cl) if it's on PATH (i.e. a "Developer Command Prompt"),
rem otherwise MinGW gcc/clang (e.g. winget install MartinStorsjo.LLVM-MinGW.UCRT).
setlocal
cd /d "%~dp0"
if not exist build mkdir build

rem The DLL is locked while the hook is loaded into running apps, so stop it first.
if exist build\macropad.exe (
    build\macropad.exe --quit
    ping -n 2 127.0.0.1 >nul
)

where cl >nul 2>nul && goto msvc
where clang >nul 2>nul && (set "CC=clang" & goto mingw)
where gcc >nul 2>nul && (set "CC=gcc" & goto mingw)
echo No C compiler found. Install LLVM-MinGW or Visual Studio C++ build tools.
exit /b 1

:mingw
echo Building with %CC%...
%CC% -O2 -Wall -Wextra -s -shared -o build\macropad_hook.dll src\hook.c -luser32 || exit /b 1
%CC% -O2 -Wall -Wextra -s -municode -mwindows -o build\macropad.exe src\macropad.c -luser32 -ladvapi32 || exit /b 1
goto done

:msvc
echo Building with MSVC...
cl /nologo /O2 /W4 /LD src\hook.c /Fe:build\macropad_hook.dll /Fo:build\ /link user32.lib || exit /b 1
cl /nologo /O2 /W4 /DUNICODE src\macropad.c /Fe:build\macropad.exe /Fo:build\ /link /SUBSYSTEM:WINDOWS user32.lib advapi32.lib || exit /b 1

:done
echo.
echo Built build\macropad.exe and build\macropad_hook.dll
