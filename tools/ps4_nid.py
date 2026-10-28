"""Compute PS4 NIDs (the hashed function IDs used in import tables).

Usage: python ps4_nid.py scePadReadState sceKernelUsleep ...
"""
import hashlib
import struct
import sys

ENC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(name):
    v, = struct.unpack("<Q", hashlib.sha1(name.encode() + SUFFIX).digest()[:8])
    # 64 bits -> 10 six-bit chars plus a final char holding the low 4 bits
    return "".join(ENC[(v >> (58 - 6 * i)) & 63] for i in range(10)) + ENC[(v & 0xF) << 2]


if __name__ == "__main__":
    for n in sys.argv[1:]:
        print(nid(n), n)
