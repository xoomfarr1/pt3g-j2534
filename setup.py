"""Setuptools configuration for the PT3G J2534 open-source driver.

Standalone, installable package for the SAE J2534-1 v04.04 / v05.00
pass-thru driver for the PT3G VCI.

The compiled extension is built with MSVC on Windows (`/O2` + `/std:c++17`).
This package ships **no Linux build** — it targets a Windows machine.
The extension source path is deliberately relative: setuptools rejects
absolute paths in ``ext_modules`` sources, and it resolves relative
paths against the directory that contains ``setup.py``, so this works
no matter where the repository lives on disk.
"""

import sys
from setuptools import Extension, find_packages, setup

# Relative source path.  setuptools resolves it against the directory that
# contains this file, so we deliberately avoid an absolute path here.
_SRC = "src/pt3g_j2534/_j2534.cpp"

# Windows-only build. The package raises instead of silently building on
# non-Windows (there is no Linux artifact shipped).
if sys.platform != "win32":
    raise RuntimeError(
        "PT3G J2534 driver is Windows-only: this package targets MSVC "
        "(/O2 + /std:c++17) on Windows. It ships no Linux build."
    )
# MSVC (Windows) uses /O2 and /std:c++17.
_EXTRA_COMPILE_ARGS = ["/O2", "/std:c++17"]

_EXT = Extension(
    name="_pt3g_j2534",
    sources=[_SRC],
    extra_compile_args=_EXTRA_COMPILE_ARGS,
    define_macros=[("Py_SSIZE_T_CLEAN", "1")],
)

setup(
    name="pt3g_j2534",
    version="1.0.0",
    description=(
        "Open-source SAE J2534-1 v04.04/v05.00 pass-thru driver for the PT3G VCI"
    ),
    author="DTS-Monaco Contributors",
    license="GPL-3.0",
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    ext_package="pt3g_j2534",
    ext_modules=[_EXT],
    python_requires=">=3.10",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Environment :: Win32 (MS Windows)",
        "Intended Audience :: Developers",
        "Intended Audience :: End Users/Desktop",
        "License :: OSI Approved :: GNU General Public License v3 (GPLv3)",
        "Operating System :: Microsoft :: Windows",
        "Programming Language :: Python :: 3",
    ],
)
