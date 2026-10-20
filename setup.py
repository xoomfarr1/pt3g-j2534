"""Setuptools configuration for the PT3G J2534 open-source driver.

Standalone, installable package for the SAE J2534-1 v04.04 / v05.00
pass-thru driver for the PT3G VCI.

The extension flags are chosen per-platform because MSVC and GCC/Clang use
different command-line syntax:

Windows (MSVC)   : /O2  and  /std:c++17
Linux (GCC/Clang): -O2  and  -std=c++17

Note: ``setup.py`` deliberately uses relative paths for the extension source.
setuptools rejects absolute paths in ``ext_modules`` sources, and it resolves
relative paths against the directory that contains ``setup.py``, so this
works no matter where the repository lives on disk.  The Windows path is the
target configuration; the Linux branch only exists as a fallback so the same
file also builds plainly on a native Linux box (kept for completeness).
"""

import sys
from setuptools import Extension, find_packages, setup

# Relative source path.  setuptools resolves it against the directory that
# contains this file, so we deliberately avoid an absolute path here.
_SRC = "src/pt3g_j2534/_j2534.cpp"

# MSVC (Windows) uses /O2 and /std:c++17; GCC/Clang (Linux) uses -O2 and
# -std=c++17.  Only one of these two lists is ever passed to the compiler.
if sys.platform == "win32":
    _EXTRA_COMPILE_ARGS = ["/O2", "/std:c++17"]
else:
    _EXTRA_COMPILE_ARGS = ["-O2", "-std=c++17"]

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
        "Operating System :: POSIX :: Linux",
        "Programming Language :: Python :: 3",
    ],
)
