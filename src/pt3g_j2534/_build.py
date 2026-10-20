"""setuptools build script for the pt3g_j2534 shared library.

Produces the ctypes-compatible extension ``_pt3g_j2534`` (Linux .so,
Windows .pyd) exposing the SAE J2534-1 v04.04 / v05.00 PassThru* functions.

Can be invoked directly (from this directory):
    python _build.py build_ext --inplace
or, from the DTS-Monaco root, the extension is built automatically as part of
``pip install -e .`` via the top-level ``setup.py``/``pyproject.toml``.

Build requirements (Linux):  g++ (any modern GCC/Clang), Python 3.10+.
Build requirements (Windows):  MSVC (Windows SDK), Python 3.10+.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import setuptools
from setuptools import Extension

_HERE = Path(__file__).resolve().parent
_SRC = _HERE / "_j2534.cpp"

# MSVC (Windows) uses /O2 and /std:c++17; GCC/Clang (Linux) uses -O2 and -std=c++17.
# The -O2/-std flags are NOT valid MSVC command-line arguments and would abort a
# Windows build, so we switch toolchain flags per platform.
if sys.platform == "win32":
    _EXTRA_COMPILE_ARGS = ["/O2", "/std:c++17"]
else:
    _EXTRA_COMPILE_ARGS = ["-O2", "-std=c++17"]

_EXT = Extension(
    name="_pt3g_j2534",
    sources=[str(_SRC)],
    extra_compile_args=_EXTRA_COMPILE_ARGS,
    define_macros=[("Py_SSIZE_T_CLEAN", "1")],
)

__version__ = "1.0.0"


def main(argv=None) -> None:
    """Build the extension in place (pybind-style entry point)."""
    setuptools.setup(
        name="pt3g_j2534",
        version=__version__,
        description=(
            "Open-source SAE J2534-1 v04.04/v05.00 pass-thru driver for the "
            "PT3G VCI"
        ),
        ext_package="pt3g_j2534",
        ext_modules=[_EXT],
        packages=["pt3g_j2534"],
        package_dir={"pt3g_j2534": str(_HERE)},
        python_requires=">=3.10",
    )


if __name__ == "__main__":
    main()
