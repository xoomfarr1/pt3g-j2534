"""pt3g_j2534 — open-source SAE J2534-1 v04.04 / v05.00 pass-thru driver for the
PT3G VCI (Porsche PIWIS III diagnostic interface).

Exposes the standard J2534 PassThru* functions via a compiled shared library
plus a CanHardwareAdapter-shaped virtual adapter for headless use.

See ``pt3g_j2534.py`` for the full public API.
"""

from __future__ import annotations

from .pt3g_j2534 import (  # noqa: F401
    # driver name constants
    J2534_ACTIA_DRIVER,
    J2534_AUTO_DRIVER,
    J2534_SERIAL_DRIVER,
    J2534_VIRTUAL_DRIVER,
    # mode / feature constants
    PASS_THROUGH_FEATURE_CONNECT,
    PASS_THROUGH_FEATURE_DISCONNECT,
    PASS_THROUGH_FEATURE_DIAGNOSTIC,
    PASS_THROUGH_FEATURE_DIAGNOSTIC_WITH_LEVEL,
    PASS_THROUGH_FEATURE_GET_CONFIG,
    PASS_THROUGH_FEATURE_INITIALIZE,
    PASS_THROUGH_FEATURE_INITIALIZE_SELECT,
    PASS_THROUGH_FEATURE_READ,
    PASS_THROUGH_FEATURE_RETRANSMIT,
    PASS_THROUGH_FEATURE_SILENT,
    PASS_THROUGH_FEATURE_SILENT_TRANSACTION,
    PASS_THROUGH_FEATURE_TRANSACTION,
    PASS_THROUGH_FEATURE_VIRTUAL_BOOT,
    PASS_THROUGH_MODE_BLOCKING,
    PASS_THROUGH_MODE_EXCLUSIVE,
    PASS_THROUGH_MODE_INTELLIGENT,
    # error / result helpers
    J2534Error,
    _HAVE_LIB,
    _lib,
    get_device_list,
    get_driver_description,
    get_version,
    PassThruOpen_,
    PassThruRead_,
    PassThruWrite_,
    # PassThru* functions (SAE J2534-1 v04.04 / v05.00)
    PassThruAttach,
    PassThruClose,
    PassThruGetDriverDescription,
    PassThruGetNextDevice,
    PassThruGetVersion,
    PassThruOpen,
    PassThruRead,
    PassThruScanForDevices,
    PassThruSendFeatureMessage,
    PassThruWrite,
    # headless adapters
    VirtualAdapter,
    VirtualJ2534Adapter,
)
