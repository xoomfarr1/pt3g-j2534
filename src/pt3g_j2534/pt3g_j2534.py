"""pt3g_j2534 — a ctypes-compatible SAE J2534-1 v04.04 / v05.00 pass-thru driver
for the PT3G VCI.

This package exposes the standard J2534 PassThru* functions through a compiled
shared library (``_pt3g_j2534.so`` on Linux, ``_pt3g_j2534.pyd`` on Windows):

    import pt3g_j2534
    h = pt3g_j2534.PassThruOpen(b"virtual")        # or b"serial", b"actia", ...
    pt3g_j2534.PassThruWrite(h, frame, len(frame), ctypes.byref(n))
    pt3g_j2534.PassThruRead(h, 1000, buf, ctypes.byref(size), ctypes.byref(n))
    pt3g_j2534.PassThruClose(h)

Cross-platform design
---------------------
* Linux:  the ``serial`` driver talks to the PT3G VCI over USB-serial.
  The transport is GNU termios on ``/dev/ttyUSB*`` (a real backend).  When no
  PT3G hardware is attached the driver falls back to a virtual FIFO adapter so
  the whole stack (and this test suite) works headless.
* Windows: the ``actia`` driver transparently loads the official Actia PDU API
  DLL (``XS_D_PDU_API_64.dll`` / ``XS_D_PDU_API.dll``) installed by the official
  PT3G Device Manager.  That path is Windows-only and is guarded: on
  Linux/macOS the ``actia`` name is mapped to the virtual adapter so the
  library always loads and exports the PassThru* symbols.

The functions exposed at module level (``PassThruOpen``,
``PassThruClose``, ...) are the raw :mod:`ctypes` bindings that match the
SAE J2534-1 v04.04 / v05.00 signatures verbatim.  See :mod:`pt3g_j2534_cli` for
a ready-to-run demo.

A small ``CanHardwareAdapter``-shaped interface
(``VirtualJ2534Adapter``) and a headless ``VirtualAdapter`` (in-process CAN
channel) are provided so the rest of the DTS-Monaco toolchain can drive the
J2534 API exactly like it drives the USB-dongle path.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import logging
import os
from typing import Any, Callable, List, Optional, Sequence

__all__ = [
    # PassThru* functions (raw ctypes bindings, J2534-1 v04.04/v05.00 signatures)
    "PassThruOpen",
    "PassThruClose",
    "PassThruRead",
    "PassThruWrite",
    "PassThruAttach",
    "PassThruSendFeatureMessage",
    "PassThruGetVersion",
    "PassThruGetDriverDescription",
    "PassThruScanForDevices",
    "PassThruGetNextDevice",
    # discovery / info helpers
    "get_device_list",
    "get_driver_description",
    "get_version",
    # error helpers
    "J2534Error",
    # adapters
    "VirtualAdapter",
    "VirtualJ2534Adapter",
    # driver name constants
    "J2534_VIRTUAL_DRIVER",
    "J2534_SERIAL_DRIVER",
    "J2534_AUTO_DRIVER",
    "J2534_ACTIA_DRIVER",
    # constants
    "PASS_THROUGH_MODE_INTELLIGENT",
    "PASS_THROUGH_MODE_EXCLUSIVE",
    "PASS_THROUGH_MODE_BLOCKING",
    "PASS_THROUGH_FEATURE_READ",
    "PASS_THROUGH_FEATURE_WRITE",
    "PASS_THROUGH_FEATURE_TRANSACTION",
]

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Shared library loading
# ---------------------------------------------------------------------------


def _find_shared_library() -> Optional[str]:
    """Locate the compiled ``_pt3g_j2534`` shared library next to this file."""
    base = os.path.dirname(os.path.abspath(__file__))
    if os.path.isdir(base):
        for fn in os.listdir(base):
            if fn.startswith("_pt3g_j2534") and (
                fn.endswith(".so") or fn.endswith(".pyd") or fn.endswith(".dylib")
            ):
                return os.path.join(base, fn)
    for libname in ("libpt3g_j2534.so", "libpt3g_j2534.so.1"):
        path = ctypes.util.find_library(libname)
        if path:
            return path
    return None


def _load_library() -> ctypes.CDLL:
    """Load the shared library, or raise a helpful ImportError."""
    path = _find_shared_library()
    if path is None:
        raise ImportError(
            "Could not locate the pt3g_j2534 shared library "
            "(_pt3g_j2534.so|.dll|.dylib). Run `pip install -e .` from the "
            "DTS-Monaco root, or `pip install -e ./src/pt3g_j2534`."
        )
    try:
        lib = ctypes.CDLL(path)
    except OSError as exc:
        raise ImportError(
            f"Could not load shared library {path!r}: {exc}. "
            "Run `pip install -e .` from the DTS-Monaco root."
        ) from exc
    logger.info("Loaded PT3G J2534 driver library: %s", path)
    return lib


_lib: Optional[ctypes.CDLL] = None
_HAVE_LIB = False
try:
    _lib = _load_library()
    _HAVE_LIB = True
except ImportError:
    _lib = None
    _HAVE_LIB = False


# ---------------------------------------------------------------------------
# J2534 constants (SAE J2534-1 v04.04 / v05.00)
# ---------------------------------------------------------------------------
PASS_THROUGH_MODE_INTELLIGENT = 0x0001
PASS_THROUGH_MODE_EXCLUSIVE = 0x0002
PASS_THROUGH_MODE_BLOCKING = 0x0004

PASS_THROUGH_FEATURE_READ = 0x0000
PASS_THROUGH_FEATURE_WRITE = 0x0001
PASS_THROUGH_FEATURE_TRANSACTION = 0x0002
PASS_THROUGH_FEATURE_SILENT = 0x0003
PASS_THROUGH_FEATURE_SILENT_TRANSACTION = 0x0004
PASS_THROUGH_FEATURE_CONNECT = 0x0005
PASS_THROUGH_FEATURE_DISCONNECT = 0x0006
PASS_THROUGH_FEATURE_DIAGNOSTIC = 0x0007
PASS_THROUGH_FEATURE_DIAGNOSTIC_WITH_LEVEL = 0x0008
PASS_THROUGH_FEATURE_VIRTUAL_BOOT = 0x0009
PASS_THROUGH_FEATURE_INITIALIZE = 0x000A
PASS_THROUGH_FEATURE_INITIALIZE_SELECT = 0x000B
PASS_THROUGH_FEATURE_GET_CONFIG = 0x000C
PASS_THROUGH_FEATURE_RETRANSMIT = 0x000D

J2534_VIRTUAL_DRIVER = "virtual"
J2534_SERIAL_DRIVER = "serial"
J2534_AUTO_DRIVER = "auto"
J2534_ACTIA_DRIVER = "actia"

J2534_SUCCESS = 0x00000000
J2534_ERR_INVALID_HANDLE = 0xFFFFFFFF
J2534_ERR_NOT_INITIALIZED = 0xFFFFFFFE
J2534_ERR_ALREADY_INITIALIZED = 0xFFFFFFFD
J2534_ERR_DEVICE_IN_USE = 0xFFFFFFFB
J2534_ERR_BAD_ARGUMENT = 0xFFFFFFFA
J2534_ERR_NOT_ATTACHED = 0xFFFFFFF9
J2534_ERR_UNKNOWN_ERROR = 0xFFFFFFF8
J2534_ERR_DEVICE_NOT_FOUND = 0xFFFFFFF7
J2534_ERR_NO_DATA = 0xFFFFFFF6
J2534_ERR_BUFFER_OVERFLOW = 0xFFFFFFF5


class J2534Error(Exception):
    """Raised when a J2534 PassThru* call fails."""


# ---------------------------------------------------------------------------
# Raw ctypes bindings (the official J2534-1 v04.04 / v05.00 surface)
# ---------------------------------------------------------------------------


def _bind(lib: ctypes.CDLL) -> None:
    """Set restype/argtypes for the PassThru* functions (match the spec)."""
    lib.PassThruOpen.restype = ctypes.c_int
    lib.PassThruOpen.argtypes = [
        ctypes.c_char_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)
    ]
    lib.PassThruClose.restype = ctypes.c_int
    lib.PassThruClose.argtypes = [ctypes.c_void_p]
    lib.PassThruRead.restype = ctypes.c_int
    lib.PassThruRead.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ]
    lib.PassThruWrite.restype = ctypes.c_int
    lib.PassThruWrite.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong),
    ]
    lib.PassThruAttach.restype = ctypes.c_int
    lib.PassThruAttach.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.PassThruSendFeatureMessage.restype = ctypes.c_int
    lib.PassThruSendFeatureMessage.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
    ]
    lib.PassThruGetVersion.restype = ctypes.c_int
    lib.PassThruGetVersion.argtypes = [
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ]
    lib.PassThruGetDriverDescription.restype = ctypes.c_int
    lib.PassThruGetDriverDescription.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong),
    ]
    lib.PassThruScanForDevices.restype = ctypes.c_int
    lib.PassThruScanForDevices.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p,
    ]
    lib.PassThruGetNextDevice.restype = ctypes.c_int
    lib.PassThruGetNextDevice.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ]


if _lib is not None:
    _bind(_lib)
    # Expose at module level exactly as the J2534 spec names them.
    PassThruOpen = _lib.PassThruOpen
    PassThruClose = _lib.PassThruClose
    PassThruRead = _lib.PassThruRead
    PassThruWrite = _lib.PassThruWrite
    PassThruAttach = _lib.PassThruAttach
    PassThruSendFeatureMessage = _lib.PassThruSendFeatureMessage
    PassThruGetVersion = _lib.PassThruGetVersion
    PassThruGetDriverDescription = _lib.PassThruGetDriverDescription
    PassThruScanForDevices = _lib.PassThruScanForDevices
    PassThruGetNextDevice = _lib.PassThruGetNextDevice
else:  # pragma: no cover
    PassThruOpen = None
    PassThruClose = None
    PassThruRead = None
    PassThruWrite = None
    PassThruAttach = None
    PassThruSendFeatureMessage = None
    PassThruGetVersion = None
    PassThruGetDriverDescription = None
    PassThruScanForDevices = None
    PassThruGetNextDevice = None


# ---------------------------------------------------------------------------
# Frame helpers
# ---------------------------------------------------------------------------


def decode_frame(data: bytes) -> tuple:
    """Decode a raw CAN frame (id 4 bytes + dlc 1 byte + data) into
    ``(frame_id, dlc, payload)``."""
    if len(data) < 5:
        return (0, 0, b"")
    frame_id = int.from_bytes(data[0:4], "big", signed=False)
    dlc = data[4] & 0xFF
    payload = bytes(data[5:5 + dlc])
    return (frame_id, dlc, payload)


# ---------------------------------------------------------------------------
# Friendly wrapper helpers (Pythonic, raise on failure)
# ---------------------------------------------------------------------------


def _check(rc: int, operation: str) -> None:
    if rc != 0:
        raise J2534Error(f"J2534 {operation} failed with error code {rc:#010x}")


def PassThruOpen_(
    szDriver: Any, ulOpenMode: int = 0
) -> int:
    """Open a J2534 adapter.  Accepts str or bytes for the driver name.

    Returns the raw adapter handle (int).  Raises J2534Error on failure.
    """
    if isinstance(szDriver, str):
        szDriver = szDriver.encode("utf-8")
    h = ctypes.c_ulong(0)
    rc = PassThruOpen(szDriver, ulOpenMode, ctypes.byref(h))
    _check(rc, "PassThruOpen")
    return h.value


def PassThruRead_(
    handle: int,
    timeout_ms: int = 100,
    buffer_size: int = 256,
) -> bytes:
    """Read raw CAN frames from the adapter into a fresh buffer.

    Returns the raw frame bytes ``id(4) + dlc(1) + data`` actually read.
    """
    buf = ctypes.create_string_buffer(buffer_size)
    size = ctypes.c_ulong(buffer_size)
    transferred = ctypes.c_ulong(0)
    rc = PassThruRead(
        handle, timeout_ms, buf, ctypes.byref(size), ctypes.byref(transferred)
    )
    _check(rc, "PassThruRead")
    return bytes(buf.raw[: size.value])


def PassThruWrite_(handle: int, frame: bytes) -> int:
    """Write one raw CAN frame (id 4 + dlc 1 + data).  Returns bytes written."""
    if len(frame) < 5:
        raise ValueError(
            f"frame too short ({len(frame)} bytes); need id(4)+dlc(1)+data"
        )
    wr = ctypes.c_ulong(0)
    rc = PassThruWrite(handle, frame, len(frame), ctypes.byref(wr))
    _check(rc, "PassThruWrite")
    return wr.value


def get_driver_description(buffer_size: int = 512) -> str:
    """Return the J2534 driver description."""
    buf = ctypes.create_string_buffer(buffer_size)
    size = ctypes.c_ulong(buffer_size)
    rc = PassThruGetDriverDescription(buf, ctypes.byref(size))
    if rc == J2534_ERR_BUFFER_OVERFLOW:
        buf = ctypes.create_string_buffer(size.value)
        rc = PassThruGetDriverDescription(buf, ctypes.byref(size))
    _check(rc, "PassThruGetDriverDescription")
    return buf.value.decode("utf-8", "replace")


def get_version() -> tuple:
    """Return ``(major, minor)`` version of the J2534 driver."""
    major = ctypes.c_ulong(0)
    minor = ctypes.c_ulong(0)
    rc = PassThruGetVersion(ctypes.byref(major), ctypes.byref(minor))
    _check(rc, "PassThruGetVersion")
    return major.value, minor.value


# ---------------------------------------------------------------------------
# v05.00 device discovery
# ---------------------------------------------------------------------------


class _DeviceInfo(ctypes.Structure):
    """Mirrored from the C++ layer so the scan callback can read the struct.

    ``DWORD`` = uint32_t = 4 bytes (not ``c_ulong`` which is 8 bytes on
    64-bit).  Layout: [uint32 dwHandle][uint32 flags][uint32 reserved]
    [char szDevice[256]] = 268 bytes.
    """

    _fields_ = [
        ("dwDeviceHandle", ctypes.c_uint),
        ("dwFlags", ctypes.c_uint),
        ("dwReserved", ctypes.c_uint),
        ("szDevice", ctypes.c_char * 256),
    ]


def _make_scan_callback(device_list: List[dict]):
    """Build a Python callback for PassThruScanForDevices.

    The C++ layer passes a populated ``PassThruDeviceInfo`` as ``pvData``;
    the callback copies its fields into a Python dict, returning 0 to keep
    scanning.
    """

    def callback(dev_handle: int, pv_data: int, ul_size: int, lp_context: int) -> int:
        if not pv_data:
            return 0
        info = _DeviceInfo.from_address(pv_data)
        if not info.szDevice:
            return 0
        device_list.append(
            {
                "device_handle": info.dwDeviceHandle,
                "flags": info.dwFlags,
                "name": info.szDevice.decode("utf-8", "replace"),
            }
        )
        return 0  # continue scanning

    return ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p
    )(callback)


def get_device_list(use_virtual: bool = True) -> List[dict]:
    """Enumerate J2534-capable devices (SAE J2534-1 v05.00).

    Linux:     serial enumeration over /dev/ttyUSB* when hardware is present,
               otherwise the virtual adapter.
    Windows:   enumeration via the Actia registry hints (the official PDU API
               driver is still required on Windows).
    """
    if not _HAVE_LIB:
        raise RuntimeError("pt3g_j2534 shared library not loaded")
    driver = J2534_VIRTUAL_DRIVER if use_virtual else J2534_AUTO_DRIVER
    try:
        h = PassThruOpen_(driver)
    except J2534Error as exc:
        logger.warning("get_device_list: driver %r unavailable: %s", driver, exc)
        try:
            h = PassThruOpen_(J2534_VIRTUAL_DRIVER)
        except J2534Error as exc2:
            raise RuntimeError("no PT3G device available for discovery") from exc2
    devices: List[dict] = []
    cb = _make_scan_callback(devices)
    try:
        rc = PassThruScanForDevices(
            h,
            PASS_THROUGH_FEATURE_GET_CONFIG,
            ctypes.cast(cb, ctypes.c_void_p),
            0,
        )
        _check(rc, "PassThruScanForDevices")
    finally:
        PassThruClose(h)
    return devices


# ---------------------------------------------------------------------------
# Headless fallback adapters
# ---------------------------------------------------------------------------

class VirtualAdapter:
    """An in-process CAN channel with no hardware — the headless fallback.

    Stores raw CAN frames ``id(4) + dlc(1) + data`` in a FIFO.  This is what
    the test suite drives when no PT3G VCI is attached.
    """

    def __init__(self, name: str = "virtual") -> None:
        self.name = name
        self._queue: List[bytes] = []
        self._opened = False

    def open(self) -> None:
        self._queue.clear()
        self._opened = True

    def close(self) -> None:
        self._opened = False
        self._queue.clear()

    def read(self, timeout_ms: int = 100) -> Optional[bytes]:
        if not self._opened or not self._queue:
            return None
        return self._queue.pop(0)

    def write(self, frame: bytes) -> int:
        if not self._opened:
            return 0
        self._queue.append(frame)
        return len(frame)

    def get_info(self):
        from dts_monaco.hardware.base import HardwareInfo, HardwareType

        return HardwareInfo(kind=HardwareType.VIRTUAL, port=self.name, channels=1)

    def test(self) -> bool:
        return self._opened


class VirtualJ2534Adapter:
    """A CanHardwareAdapter that drives the J2534 virtual backend.

    This lets the rest of DTS-Monaco open a PT3G VCI adapter through the
    J2534 API even when no hardware is attached (headless development /
    CI), mirroring how ``UsbDongleAdapter`` is exercised with the python-can
    virtual bus.
    """

    def __init__(self, j2534_driver: str = J2534_VIRTUAL_DRIVER) -> None:
        self._driver = j2534_driver
        self._handle: int = 0
        self._opened = False
        self._lib_available = _HAVE_LIB
        self._name = f"virtual-j2534:{j2534_driver}"
        self._queue = []

    # -- CanHardwareAdapter contract -------------------------------------
    def open(self) -> None:
        import dts_monaco.hardware.base as base

        if self._opened:
            return
        if not self._lib_available:
            # Pure-python fallback: no shared library -> use the virtual bus.
            self._va = VirtualAdapter(self._name)
            self._va.open()
            self._opened = True
            logger.info("J2534 virtual adapter (python fallback): %s", self._name)
            return
        try:
            self._handle = PassThruOpen_(self._driver)
            self._opened = True
            logger.info(
                "J2534 virtual adapter opened: driver=%r", self._driver
            )
        except J2534Error as exc:
            logger.warning("J2534 virtual backend unavailable: %s", exc)
            self._va = VirtualAdapter(self._name)
            self._va.open()
            self._opened = True

    def close(self) -> None:
        if self._handle:
            try:
                PassThruClose(self._handle)
            except Exception:
                pass
            self._handle = 0
        self._opened = False
        self._queue.clear()

    def read(self, timeout_ms: int = 100) -> Optional[bytes]:
        if not self._opened:
            return None
        if not self._lib_available or hasattr(self, "_va"):
            return self._va.read(timeout_ms) if hasattr(self, "_va") else None
        try:
            raw = PassThruRead_(self._handle, timeout_ms)
            return raw
        except J2534Error as exc:
            logger.warning("J2534 read failed: %s", exc)
            return None

    def write(self, frame: bytes) -> int:
        if not self._opened:
            return 0
        if not self._lib_available or hasattr(self, "_va"):
            return self._va.write(frame) if hasattr(self, "_va") else 0
        try:
            return PassThruWrite_(self._handle, frame)
        except J2534Error:
            return 0

    def get_info(self):
        import dts_monaco.hardware.base as base

        return base.HardwareInfo(
            kind=base.HardwareType.PT3G_VCI,
            port="virtual (J2534)",
            vendor_id="PT3G / Porsche",
            product_id="PT3G-VCI",
            serial="(virtual)",
            channels=1,
        )

    def test(self) -> bool:
        try:
            self.write(b"\x00\x00\x00\x01\x01\x00")
            return self.read(200) is not None
        except Exception:
            return False
