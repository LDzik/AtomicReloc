@echo off
setlocal enabledelayedexpansion

echo ====================================================
echo AtomicReloc Build System
echo ====================================================

:: Check if MSVC is already in path
where cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo MSVC compiler already in environment path.
    goto COMPILE
)

:: Try user-specified path
set "VS_PATH=D:\Visual Studio\2026\VC\Auxiliary\Build\vcvars64.bat"
if exist "!VS_PATH!" (
    echo Loading Visual Studio 2026 x64 Developer Environment...
    call "!VS_PATH!"
    goto COMPILE
)

:: Try other common paths
for %%p in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
) do (
    if exist %%p (
        echo Loading Visual Studio Environment from %%p...
        call %%p
        goto COMPILE
    )
)

echo ERROR: Visual Studio x64 developer environment (vcvars64.bat) could not be found.
echo Please run this batch script inside an active Visual Studio Developer Command Prompt.
exit /b 1

:COMPILE
echo.
echo Compiling Resources...
rc.exe /fo resources\resources.res resources\resources.rc
if %ERRORLEVEL% neq 0 (
    echo ERROR: Resource compilation failed.
    exit /b %ERRORLEVEL%
)

echo.
echo Compiling AtomicReloc C++ Sources...
cl.exe /std:c++20 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE src\FileSystemEngine.cpp src\JournalManager.cpp src\RestartManager.cpp src\TaskCoordinator.cpp src\WinUI.cpp src\main.cpp resources\resources.res /link /OUT:AtomicReloc.exe /SUBSYSTEM:WINDOWS ole32.lib shell32.lib rstrtmgr.lib comctl32.lib user32.lib gdi32.lib Advapi32.lib

if %ERRORLEVEL% equ 0 (
    echo.
    echo ====================================================
    echo SUCCESS: AtomicReloc.exe successfully compiled!
    echo ====================================================
) else (
    echo.
    echo ====================================================
    echo ERROR: Compilation failed!
    echo ====================================================
)
