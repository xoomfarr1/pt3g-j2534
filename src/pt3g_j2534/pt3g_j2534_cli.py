#!/usr/bin/env python3
"""pt3g_j2534_cli — a small demo CLI for the PT3G J2534 open-source driver.

Usage:
    python -m pt3g_j2534_pt3g_j2534_cli send-probe
    python -m pt3g_j2534_pt3g_j2534_cli send-probe --driver virtual
    python -m pt3g_j2534_pt3g_j2534_cli send-probe --driver serial --port /dev/ttyUSB0

Sends a probe CAN frame over the J2534 pass-thru API and reports the round
trip.  On a machine with no PT3G hardware attached, pass ``--driver virtual``
to use the headless fallback.

Prerequisites:
    pip install -e /root/dtm        # builds _pt3g_j2534.so/.dll + python package

Frames are raw CAN bytes: [id:4 big-endian][dlc:1][data...].
"""

from __future__ import annotations

import argparse
import logging
import sys

logger = logging.getLogger(__name__)

# The module lives in the pt3g_j2534 package.
import pt3g_j2534  # noqa: E402

# A probe CAN frame: id 0x7E0, dlc 8, data 8 bytes (0xBB..0xFF).
PROBE_FRAME = bytes(
    [
        0x00, 0x00, 0x07, 0xE0,
        0x08,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    ]
)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="PT3G VCI J2534 probe (open-source driver demo)"
    )
    p.add_argument(
        "command",
        choices=["send-probe"],
        help="what to do",
    )
    p.add_argument(
        "--driver",
        default="auto",
        help=(
            "J2534 driver: 'virtual' (headless fallback), 'serial' "
            "(Linux /dev/ttyUSB*), 'actia' (Windows Actia PDU API), 'auto' "
            "(platform default)"
        ),
    )
    p.add_argument(
        "--baud",
        type=int,
        default=500000,
        help="CAN baud rate (default 500000)",
    )
    p.add_argument(
        "--timeout",
        type=int,
        default=1000,
        help="read timeout in ms (default 1000)",
    )
    p.add_argument(
        "--debug",
        action="store_true",
        help="enable debug logging",
    )
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(levelname)-8s %(name)s: %(message)s",
    )

    # The CLI needs the compiled driver library.
    if not pt3g_j2534._HAVE_LIB:
        logger.error(
            "PT3G J2534 driver library not found. Run `pip install -e /root/dtm` "
            "which builds _pt3g_j2534.so/.dll."
        )
        return 1

    try:
        # Use the convenience wrapper PassThruOpen_ (accepts str/bytes driver,
        # returns raw handle).  Raises J2534Error on failure.
        handle = pt3g_j2534.PassThruOpen_(args.driver)
    except pt3g_j2534.J2534Error as exc:
        logger.error("PassThruOpen failed: %s", exc)
        return 1
    logger.info(
        "opened J2534 adapter (driver=%r, handle=%#x)", args.driver, handle
    )

    try:
        # Send a probe frame (raw CAN: id 4 bytes + dlc 1 byte + data).
        written = pt3g_j2534.PassThruWrite_(handle, PROBE_FRAME)
        logger.info("wrote %d bytes", written)

        # Read back the echoed probe frame.
        frame = pt3g_j2534.PassThruRead_(handle, args.timeout)
        if frame is None or len(frame) < 5:
            logger.warning("no frame read back (timeout=%dms)", args.timeout)
            return 0
        frame_id = int.from_bytes(frame[0:4], "big", signed=False)
        dlc = frame[4]
        payload = frame[5:5 + dlc]
        logger.info(
            "round-trip OK: id=0x%03X dlc=%d payload=%s",
            frame_id,
            dlc,
            payload,
        )
    finally:
        pt3g_j2534.PassThruClose(handle)
        logger.info("closed adapter")

    return 0


if __name__ == "__main__":
    sys.exit(main())
