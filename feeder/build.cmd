@echo off
rem Run from anywhere. Builds SteelBattalionFeeder.exe next to this script.
setlocal
set "HERE=%~dp0"
for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "OUT=%TEMP%\sbfeeder-build"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl /nologo /W4 /O2 /MT /EHsc /I"%HERE%vigem\include" "%HERE%sbfeeder.c" "%HERE%vigem\src\ViGEmClient.cpp" /Fe:SteelBattalionFeeder.exe /link /SUBSYSTEM:WINDOWS setupapi.lib
if errorlevel 1 (popd & exit /b 1)
copy /y SteelBattalionFeeder.exe "%HERE%" >nul
popd
echo Built %HERE%SteelBattalionFeeder.exe