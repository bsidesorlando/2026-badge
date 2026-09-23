#!/usr/bin/env python3
"""Obfuscate a CTF flag for src/ctf.c.

    scripts/ctf_flag.py <challenge id> <flag>

Prints the C array to paste into ctf.c. The keystream must match
ctf_keystream() in ctf.c: an 8-bit LCG seeded from the challenge id.
This only keeps flags out of `strings`; it is not encryption.
"""

import sys


def keystream(cid: int, n: int):
    x = (0xA5 ^ (cid * 0x3B)) & 0xFF
    for _ in range(n):
        x = (x * 5 + 0x3B) & 0xFF
        yield x


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cid = int(sys.argv[1], 0)
    flag = sys.argv[2].encode()
    enc = [b ^ k for b, k in zip(flag, keystream(cid, len(flag)))]
    body = ", ".join("0x%02X" % b for b in enc)
    print("static const uint8_t flag%d[%d] = { %s };" % (cid, len(enc), body))


if __name__ == "__main__":
    main()
