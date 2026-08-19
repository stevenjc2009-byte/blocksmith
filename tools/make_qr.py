#!/usr/bin/env python3
"""Generates docs/install-qr.png: the QR code FBI scans to install this release.

FBI's "Scan QR Code" fetches whatever URL the code carries and installs it, so
the code has to encode the *direct asset download* URL, not the release page --
a release page is HTML and FBI will refuse it.

Error correction is level L on purpose. The URL is long, and a 3DS camera
reading a phone or monitor at arm's length does far better with fewer, larger
modules than with the redundancy a higher level would spend them on; VP and
Hotswap both ship level L for the same reason.

The generated code is decoded back with OpenCV and asserted equal to the input
before the file is written, because a QR that encodes the wrong string still
looks exactly like a QR.

Usage:  python tools/make_qr.py <version>
"""
import sys
from pathlib import Path

import cv2
import numpy as np
import qrcode

REPO = "stevenjc2009-byte/blocksmith"
ROOT = Path(__file__).resolve().parent.parent


def asset_url(version: str) -> str:
    flat = version.lstrip("v")
    return f"https://github.com/{REPO}/releases/download/v{flat}/blocksmith{flat}.cia"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    url = asset_url(sys.argv[1])

    qr = qrcode.QRCode(
        version=None,                                  # smallest that fits
        error_correction=qrcode.constants.ERROR_CORRECT_L,
        box_size=8,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white").convert("RGB")

    out = ROOT / "docs" / "install-qr.png"
    out.parent.mkdir(exist_ok=True)
    img.save(out)

    # Read it back off disk and decode it. A QR encoding the wrong URL is
    # indistinguishable from a correct one by eye, so this is the only check
    # that means anything.
    decoded, _, _ = cv2.QRCodeDetector().detectAndDecode(
        cv2.imread(str(out), cv2.IMREAD_COLOR)
    )
    if decoded != url:
        print(f"FAIL: decoded {decoded!r}, expected {url!r}")
        return 1

    print(f"wrote {out} ({out.stat().st_size} bytes), version {qr.version}")
    print(f"decoded back OK: {decoded}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
