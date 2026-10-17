@echo off
rem Build fm07mod (x86, VS 18 toolchain).
rem   bin\   fm07mod.dll + fm07launch.exe + fm07mod.ini for local testing (launcher mode)
rem   dist\  ready-to-copy package: dsound.dll + fm07mod.ini (+ optional launcher)
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
rem a running game keeps fm07mod.dll locked, but a loaded DLL can still be renamed
if exist bin\fm07mod.dll.old del bin\fm07mod.dll.old 2>nul
if exist bin\fm07mod.dll move /y bin\fm07mod.dll bin\fm07mod.dll.old >nul
set CFLAGS=/nologo /O2 /W4 /MT /GS- /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Foobj\
cl %CFLAGS% /LD src\fm07mod.c src\hook.c src\hires.c src\perf.c src\proxy.c /Fe:bin\fm07mod.dll /link /MAP:obj\fm07mod.map /DEF:src\fm07mod.def user32.lib gdi32.lib || exit /b 1
cl %CFLAGS% src\launcher.c /Fe:bin\fm07launch.exe /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
if not exist bin\fm07mod.ini copy fm07mod.ini bin\ >nul

rem dist: the same DLL serves as the dsound.dll drop-in
if not exist dist mkdir dist
if not exist dist\launcher mkdir dist\launcher
copy /y bin\fm07mod.dll dist\dsound.dll >nul
copy /y fm07mod.ini dist\ >nul
copy /y bin\fm07mod.dll dist\launcher\ >nul
copy /y bin\fm07launch.exe dist\launcher\ >nul
copy /y fm07mod.ini dist\launcher\ >nul
echo Built bin\ and dist\
