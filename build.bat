@echo off
setlocal

set PROJECT_DIR=%~dp0
set DIST_DIR=F:\tmp\murmur-dist
set BUILD_DIR=F:\tmp\murmur-build
set OUTPUT_DIR=%PROJECT_DIR%Murmur

echo === Building Murmur ===
echo.

:: Step 1: Build Python engine with PyInstaller
echo [1/3] Building Python engine (PyInstaller)...
cd /d "%PROJECT_DIR%"
call venv\Scripts\activate
pyinstaller murmur-engine.spec --distpath "%DIST_DIR%" --workpath "%BUILD_DIR%" --noconfirm
if errorlevel 1 (
    echo ERROR: PyInstaller build failed
    exit /b 1
)
echo Python engine built.
echo.

:: Step 2: Build C++ UI with CMake (Release)
echo [2/3] Building C++ UI (CMake Release)...
set VCPKG_ROOT=F:\vcpkg
cd /d "%PROJECT_DIR%dictation-ui"
cmake --preset release
cmake --build build/release --config Release
if errorlevel 1 (
    echo ERROR: CMake build failed
    exit /b 1
)
echo C++ UI built.
echo.

:: Step 3: Assemble Murmur folder
echo [3/3] Assembling Murmur folder...
if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%\models"
mkdir "%OUTPUT_DIR%\logs"

:: C++ UI exe + DLLs
copy "%PROJECT_DIR%dictation-ui\build\release\Release\Murmur.exe" "%OUTPUT_DIR%\Murmur.exe"
copy "%PROJECT_DIR%dictation-ui\build\release\Release\brotlicommon.dll" "%OUTPUT_DIR%\"
copy "%PROJECT_DIR%dictation-ui\build\release\Release\brotlidec.dll" "%OUTPUT_DIR%\"

:: Python engine bundle
xcopy "%DIST_DIR%\murmur-engine" "%OUTPUT_DIR%\engine\" /E /I /Q

:: Config + prompts + docs
copy "%PROJECT_DIR%config.json" "%OUTPUT_DIR%\config.json"
copy "%PROJECT_DIR%Murmur\README.txt" "%OUTPUT_DIR%\README.txt" 2>nul
xcopy "%PROJECT_DIR%prompts" "%OUTPUT_DIR%\prompts\" /E /I /Q

echo.
echo === Build complete ===
echo Output: %OUTPUT_DIR%
dir "%OUTPUT_DIR%" /b
echo.
echo Run: %OUTPUT_DIR%\Murmur.exe
