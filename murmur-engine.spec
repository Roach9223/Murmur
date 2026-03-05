# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for Murmur engine (Python dictation backend)
# Build with: pyinstaller murmur-engine.spec

import sys
from PyInstaller.utils.hooks import collect_all, collect_submodules

block_cipher = None

# Collect all files for packages that have complex data/DLL dependencies
torch_datas, torch_binaries, torch_hiddenimports = collect_all('torch')
onnxruntime_datas, onnxruntime_binaries, onnxruntime_hiddenimports = collect_all('onnxruntime')
ctranslate2_datas, ctranslate2_binaries, ctranslate2_hiddenimports = collect_all('ctranslate2')
faster_whisper_datas, faster_whisper_binaries, faster_whisper_hiddenimports = collect_all('faster_whisper')

a = Analysis(
    ['app.py'],
    pathex=[],
    binaries=torch_binaries + onnxruntime_binaries + ctranslate2_binaries + faster_whisper_binaries,
    datas=torch_datas + onnxruntime_datas + ctranslate2_datas + faster_whisper_datas + [
        ('services', 'services'),
    ],
    hiddenimports=[
        # torch / CUDA
        *torch_hiddenimports,
        *onnxruntime_hiddenimports,
        *ctranslate2_hiddenimports,
        # faster-whisper
        'faster_whisper',
        # scipy submodules
        'scipy.signal',
        'scipy.signal._signaltools',
        # uvicorn / fastapi
        'uvicorn',
        'uvicorn.logging',
        'uvicorn.loops',
        'uvicorn.loops.auto',
        'uvicorn.protocols',
        'uvicorn.protocols.http',
        'uvicorn.protocols.http.auto',
        'uvicorn.protocols.websockets',
        'uvicorn.protocols.websockets.auto',
        'uvicorn.lifespan',
        'uvicorn.lifespan.on',
        'fastapi',
        'starlette',
        # audio
        'sounddevice',
        '_sounddevice_data',
        # keyboard + mouse
        'keyboard',
        'mouse',
        # pystray
        'pystray',
        'pystray._win32',
        # Windows
        'ctypes.windll',
        'ctypes.wintypes',
        # numpy
        'numpy',
        # stdlib (scipy -> numpy.testing -> unittest)
        *collect_submodules('unittest'),
        *collect_submodules('numpy.testing'),
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'tkinter',
        'matplotlib',
        'xmlrpc',
        'lib2to3',
        'IPython',
        'jupyter',
        'notebook',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='murmur-engine',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,  # windowed mode (no console) — like pythonw.exe
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='assets/favicon.ico',
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='murmur-engine',
)
