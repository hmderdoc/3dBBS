#!/usr/bin/env python3
"""Generate the QR code for FBI's Remote Install (scan -> installs the .cia
over the air). Point it at a release asset URL:

    python3 tools/make_qr.py \
        https://github.com/hmderdoc/3dBBS/releases/download/v0.4.0/3dBBS.cia \
        qr.png

FBI fetches the URL over plain HTTP(S) on the console; GitHub release asset
URLs work directly. Requires: pip install segno
"""
import sys

import segno


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    url, out = sys.argv[1], sys.argv[2]
    qr = segno.make(url, error="m")
    qr.save(out, scale=6, border=4)
    print(f"{out}: QR for {url}")


if __name__ == "__main__":
    main()
