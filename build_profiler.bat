@echo off
rem Build the external sampling profiler (x86) into mod\bin\fm07prof.exe.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /W4 /MT /D_CRT_SECURE_NO_WARNINGS src\profiler.c /Foobj\ /Fe:bin\fm07prof.exe /link winmm.lib || exit /b 1
echo Built bin\fm07prof.exe
