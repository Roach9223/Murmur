# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for Murmur engine (Python dictation backend)
# Build with: pyinstaller murmur-engine.spec

import os
import sys
from PyInstaller.utils.hooks import collect_all, collect_submodules

block_cipher = None

# Collect all files for packages that have complex data/DLL dependencies.
# NOTE: torch is deliberately NOT bundled — it was only needed for the
# optional Silero VAD via torch.hub (~4.3GB), which is off by default and
# falls back to RMS silence detection. faster-whisper's own vad_filter uses
# onnxruntime, which IS bundled. This keeps the release zip under GitHub's
# 2GB asset limit.
onnxruntime_datas, onnxruntime_binaries, onnxruntime_hiddenimports = collect_all('onnxruntime')
ctranslate2_datas, ctranslate2_binaries, ctranslate2_hiddenimports = collect_all('ctranslate2')
faster_whisper_datas, faster_whisper_binaries, faster_whisper_hiddenimports = collect_all('faster_whisper')


def _no_libs(entries):
    """Drop MSVC link-time artifacts (*.lib) — dead weight at runtime."""
    return [(src, dest) for (src, dest) in entries
            if not str(src).lower().endswith('.lib')]


# ctranslate2's GPU path needs cuBLAS + cuDNN at runtime, which the pip wheel
# does not ship (only a cudnn stub). Grab exactly the DLLs it needs from the
# venv's torch install instead of bundling all of torch. cudnn_adv is skipped
# (RNN/legacy-attention ops ctranslate2 never calls, 230MB).
# Placed at the bundle root ('.') — the PyInstaller bootloader puts _internal
# on the DLL search path.
_CUDA_DLLS = [
    'cublas64_12.dll',
    'cublasLt64_12.dll',
    'cudart64_12.dll',
    'cudnn64_9.dll',
    'cudnn_ops64_9.dll',
    'cudnn_cnn64_9.dll',
    'cudnn_graph64_9.dll',
    'cudnn_heuristic64_9.dll',
    'cudnn_engines_precompiled64_9.dll',
    'cudnn_engines_runtime_compiled64_9.dll',
    'zlibwapi.dll',
]

_torch_lib = os.path.abspath(os.path.join(os.path.dirname(sys.executable),
                                          '..', 'Lib', 'site-packages', 'torch', 'lib'))
cuda_binaries = []
for _dll in _CUDA_DLLS:
    _p = os.path.join(_torch_lib, _dll)
    if os.path.exists(_p):
        cuda_binaries.append((_p, '.'))
    else:
        print(f'WARNING: {_dll} not found in {_torch_lib} — '
              f'GPU transcription may fall back to CPU in the bundle')


a = Analysis(
    ['app.py'],
    pathex=[],
    binaries=_no_libs(onnxruntime_binaries + ctranslate2_binaries + faster_whisper_binaries) + cuda_binaries,
    datas=_no_libs(onnxruntime_datas + ctranslate2_datas + faster_whisper_datas) + [
        ('services', 'services'),
    ],
    hiddenimports=[
        *onnxruntime_hiddenimports,
        *ctranslate2_hiddenimports,
        # faster-whisper
        'faster_whisper',
        # scipy submodules
        'scipy.signal',
        'scipy.signal._signaltools',
        # uvicorn / fastapi
        *collect_submodules('uvicorn'),
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
        # torch intentionally excluded — see note above collect_all calls
        'torch',
        'torchaudio',
        'torchvision',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

# PyInstaller's dependency walker re-discovers some CUDA DLLs via
# ctranslate2/onnxruntime imports and stages duplicates under torch\lib\.
# We already ship them at the bundle root — drop the ~800MB of duplicates.
a.binaries = [b for b in a.binaries
              if not b[0].lower().replace('/', '\\').startswith('torch\\')]
a.datas = [d for d in a.datas
           if not d[0].lower().replace('/', '\\').startswith('torch\\')]

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
