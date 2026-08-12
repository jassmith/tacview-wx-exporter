@echo off
setlocal

:: Builds tacview.dll (the proxy) into build\. Run from a Developer Command
:: Prompt, or let this script find the Build Tools install itself.

:: Set up the toolchain unless we are already inside a Developer Command
:: Prompt. Note: no parenthesised block around this — %VCVARS% would not
:: expand in the same block that sets it.
if not "%VSINSTALLDIR%"=="" goto :toolchain_ready

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :no_toolchain
call "%VCVARS%" >nul
goto :toolchain_ready

:no_toolchain
echo ERROR: vcvars64.bat not found. Install VS 2022 Build Tools with the
echo        "Desktop development with C++" workload, or run this from a
echo        Developer Command Prompt.
exit /b 1

:toolchain_ready

set "HERE=%~dp0"
if not exist "%HERE%build" mkdir "%HERE%build"

:: Version metadata. Without it, server tooling that checks the Tacview
:: version either errors out or "repairs" the install by replacing this DLL.
rc /nologo /fo "%HERE%build\version.res" "%HERE%src\version.rc"
if errorlevel 1 (
    echo BUILD FAILED - could not compile version.rc
    exit /b 1
)

cl /nologo /LD /O2 /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN ^
   /I"%HERE%src" ^
   "%HERE%src\proxy.cpp" "%HERE%src\acmi_inject.cpp" "%HERE%src\zipw.cpp" ^
   /Fo:"%HERE%build\\" ^
   /Fe:"%HERE%build\tacview.dll" ^
   /link /DEF:"%HERE%src\proxy.def" "%HERE%build\version.res"

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo Built %HERE%build\tacview.dll
endlocal
