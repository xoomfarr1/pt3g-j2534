# PT3G-J2534 — Open-source SAE J2534-1 pass-thru driver

Open-source, cross-platform implementation of the **SAE J2534-1 v04.04 and
v05.00 `PassThru*`** API for the **PT3G VCI** — the interface Porsche's PIWIS
III diagnostic software uses. This is a standalone, installable Python package
(GPL-3.0), independent of any other toolchain.

It exposes the standard J2534 `PassThruOpen` / `PassThruClose` /
`PassThruRead` / `PassThruWrite` / `PassThruAttach` /
`PassThruSendFeatureMessage` / `PassThruGetVersion` /
`PassThruGetDriverDescription` / `PassThruScanForDevices` /
`PassThruGetNextDevice` functions through a compiled C extension
(`_pt3g_j2534`), callable from Python via ctypes.

## Where things live

```text
src/pt3g_j2534/
├── __init__.py          # package re-export of the public API
├── pt3g_j2534.py        # public Python API (PassThruOpen etc.)
├── pt3g_j2534_cli.py    # CLI entrypoint (pt3g-j2534 send-probe)
├── _build.py            # build script
├── _j2534.cpp           # C++17 glue: exports the PassThru* symbols
```

## Install (standalone)

```bash
pip install -e .
```

This builds `_pt3g_j2534` and installs the package as editable. On Windows,
`pip install -e .` detects MSVC and uses `/O2` + `/std:c++17`; on Linux it uses
`-O2` + `-std=c++17`.

This package has **no Python runtime dependencies** — the J2534 surface is a
compiled C extension over ctypes.

## Quick start

```python
import pt3g_j2534

h = pt3g_j2534.PassThruOpen(b"virtual")   # headless fallback (no hardware)
pt3g_j2534.PassThruWrite(h, frame, len(frame), ctypes.byref(n))
pt3g_j2534.PassThruRead(h, 1000, buf, ctypes.byref(size), ctypes.byref(n))
pt3g_j2534.PassThruClose(h)
```

## Cross-platform design

### Linux (native)
The `serial` driver opens the PT3G VCI's USB-serial interface (`/dev/ttyUSB*`)
with GNU termios (`8N1, 115200 bps`). When no PT3G hardware is attached, the
driver falls back to a headless virtual FIFO adapter, so the whole stack runs
with zero hardware.

### Windows
The `actia` driver loads the official **Actia PDU API DLL**
(`XS_D_PDU_API_64.dll` / `XS_D_PDU_API.dll`) installed by the official **PT3G
Device Manager**. The Windows behaviour is preserved exactly (same product-ID /
serial-number lookup and CAN channel model). The DLL path is guarded and is
never loaded on Linux/macOS — the `actia` name is transparently remapped to the
virtual adapter there so the library still loads and exports the `PassThru*`
symbols.

## Building a Windows `.pyd` (standalone, no MSVC on the build box)

Because `pip install -e .` needs a compiler, and PyInstaller cannot
cross-compile, the cleanest approach when cross-compiling from Linux is to
build the `.pyd` directly with MinGW-w64:

```bash
# Debian/Ubuntu
sudo apt install g++-mingw-w64-x86-64

mkdir -p build/win64
x86_64-w64-mingw32-g++ -shared \
    -Os -std=c++17 -DWIN32 -DNDEBUG -DUNICODE -DPy_SSIZE_T_CLEAN=1 \
    -static-libgcc -static-libstdc++ \
    -o build/win64/_pt3g_j2534.pyd \
    src/pt3g_j2534/_j2534.cpp \
    -lkernel32 -luser32 -lwinpthread
```

The resulting `_pt3g_j2534.pyd` is a self-contained Windows DLL (it statically
links the C++ runtime) that the Python wrapper loads via `ctypes.CDLL`.

## Version support

* **J2534-1 v04.04** — `PassThruOpen`, `PassThruClose`, `PassThruRead`,
  `PassThruWrite`, `PassThruAttach`, `PassThruSendFeatureMessage`,
  `PassThruGetVersion`, `PassThruGetDriverDescription`
* **J2534-1 v05.00** — the above, plus `PassThruScanForDevices` /
  `PassThruGetNextDevice`

## Tests (headless, no hardware)

```bash
pip install -e ".[dev]"
pytest
```

## License

GPL-3.0 — see [LICENSE](LICENSE).
