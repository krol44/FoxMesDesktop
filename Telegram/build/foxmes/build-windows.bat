@echo off
setlocal
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (echo No Visual Studio installation was found.& exit /b 1)
set "VCTOOLSETFILE=%VSPATH%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt"
if not exist "%VCTOOLSETFILE%" set "VCTOOLSETFILE=%VSPATH%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
echo Toolset version file: %VCTOOLSETFILE%
if not exist "%VCTOOLSETFILE%" (echo No toolset version file was found.& exit /b 1)
set "VCTOOLSET="
for /f "usebackq tokens=1" %%v in ("%VCTOOLSETFILE%") do set "VCTOOLSET=%%v"
if not defined VCTOOLSET (echo The default toolset file is empty.& exit /b 1)
echo Pinning both build systems to MSVC toolset %VCTOOLSET%.
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -vcvars_ver=%VCTOOLSET% || exit /b 1
if not "%VCToolsVersion%"=="%VCTOOLSET%" (echo Toolset mismatch: VsDevCmd selected "%VCToolsVersion%" but MSBuild will use "%VCTOOLSET%".& exit /b 1)
set "Platform=x64"
echo Platform=%Platform% VSCMD_ARG_TGT_ARCH=%VSCMD_ARG_TGT_ARCH%
where cl || exit /b 1
set "FOXMES_EXPECTED_TOOLSET=%VCTOOLSET%"
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*
