@echo off
setlocal

set PROJECT_DIR=%~dp0
set DIST_DIR=%TEMP%\murmur-dist
set BUILD_DIR=%TEMP%\murmur-build
set OUTPUT_DIR=%PROJECT_DIR%Murmur

:: Parse flags
set UI_ONLY=0
if "%1"=="--ui-only" set UI_ONLY=1

if %UI_ONLY%==1 (
    echo === Building Murmur [UI only] ===
) else (
    echo === Building Murmur [full] ===
)
echo.

:: Kill running Murmur processes before building
tasklist /FI "IMAGENAME eq Murmur.exe" 2>NUL | %SystemRoot%\System32\find.exe /I "Murmur.exe" >NUL && (
    echo Stopping running Murmur.exe...
    taskkill /F /IM Murmur.exe >NUL 2>&1
    timeout /t 2 /nobreak >NUL
)
tasklist /FI "IMAGENAME eq murmur-engine.exe" 2>NUL | %SystemRoot%\System32\find.exe /I "murmur-engine.exe" >NUL && (
    echo Stopping running murmur-engine.exe...
    taskkill /F /IM murmur-engine.exe >NUL 2>&1
    timeout /t 2 /nobreak >NUL
)

:: Step 1: Build Python engine with PyInstaller (skip with --ui-only)
if %UI_ONLY%==1 (
    echo [1/3] Skipping Python engine --ui-only
    echo.
) else (
    echo [1/3] Building Python engine with PyInstaller...
    cd /d "%PROJECT_DIR%"
    call venv\Scripts\activate
    pyinstaller murmur-engine.spec --distpath "%DIST_DIR%" --workpath "%BUILD_DIR%" --noconfirm
    if errorlevel 1 (
        echo ERROR: PyInstaller build failed
        exit /b 1
    )
    echo Python engine built.
    echo.
)

:: Step 2: Build C++ UI with CMake (Release)
echo [2/3] Building C++ UI (CMake Release)...
if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    echo Set it to your vcpkg installation directory, e.g. set VCPKG_ROOT=C:\vcpkg
    exit /b 1
)
cd /d "%PROJECT_DIR%dictation-ui"
:: Only configure if not already configured
if not exist "build\release\CMakeCache.txt" (
    echo Configuring CMake...
    cmake --preset release
) else (
    echo CMake already configured, skipping configure.
)
cmake --build build/release --config Release -- /maxcpucount
if errorlevel 1 (
    echo ERROR: CMake build failed
    exit /b 1
)
echo C++ UI built.
echo.

:: Step 3: Assemble Murmur folder (incremental)
echo [3/3] Assembling Murmur folder...
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if not exist "%OUTPUT_DIR%\models" mkdir "%OUTPUT_DIR%\models"
if not exist "%OUTPUT_DIR%\logs" mkdir "%OUTPUT_DIR%\logs"
if not exist "%OUTPUT_DIR%\Recordings" mkdir "%OUTPUT_DIR%\Recordings"
if not exist "%OUTPUT_DIR%\Transcriptions" mkdir "%OUTPUT_DIR%\Transcriptions"

:: C++ UI exe + DLLs (always copy)
copy /Y "%PROJECT_DIR%dictation-ui\build\release\Release\Murmur.exe" "%OUTPUT_DIR%\Murmur.exe"
copy /Y "%PROJECT_DIR%dictation-ui\build\release\Release\brotlicommon.dll" "%OUTPUT_DIR%\"
copy /Y "%PROJECT_DIR%dictation-ui\build\release\Release\brotlidec.dll" "%OUTPUT_DIR%\"

:: Python engine bundle (skip with --ui-only if engine dir exists)
if %UI_ONLY%==1 (
    if exist "%OUTPUT_DIR%\engine\" (
        echo Engine bundle unchanged, skipping.
    ) else (
        echo WARNING: No engine bundle found, copying anyway...
        xcopy "%DIST_DIR%\murmur-engine" "%OUTPUT_DIR%\engine\" /E /I /Q /Y
    )
) else (
    :: Full build: always refresh engine
    if exist "%OUTPUT_DIR%\engine\" rmdir /s /q "%OUTPUT_DIR%\engine"
    xcopy "%DIST_DIR%\murmur-engine" "%OUTPUT_DIR%\engine\" /E /I /Q /Y
)

:: Config + prompts
copy /Y "%PROJECT_DIR%config.json" "%OUTPUT_DIR%\config.json"
xcopy "%PROJECT_DIR%prompts\*" "%OUTPUT_DIR%\prompts\" /I /Q /Y

echo.
echo === Build complete ===
echo Output: %OUTPUT_DIR%
dir "%OUTPUT_DIR%" /b
echo.
echo Run: %OUTPUT_DIR%\Murmur.exe
