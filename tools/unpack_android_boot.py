#!/usr/bin/env python3
"""Unpacks Android boot.img (v4), init_boot.img and vendor_boot.img (v4) for inspection.

Usage: tools/unpack_android_boot.py <dir with images> <out dir>
Writes kernel, ramdisks, vendor bootconfig and the kernel command lines.
"""
import os
import struct
import sys


def align(n, a):
    return (n + a - 1) // a * a


def unpack_boot(path, out, prefix):
    d = open(path, "rb").read()
    assert d[:8] == b"ANDROID!", path
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from("<4I", d, 8)
    version = struct.unpack_from("<I", d, 40)[0]
    cmdline = d[44:44 + 1536].split(b"\0")[0].decode()
    page = 4096
    off = page
    kernel = d[off:off + kernel_size]
    off += align(kernel_size, page)
    ramdisk = d[off:off + ramdisk_size]
    if kernel:
        open(os.path.join(out, prefix + "kernel"), "wb").write(kernel)
    if ramdisk:
        open(os.path.join(out, prefix + "ramdisk"), "wb").write(ramdisk)
    print(f"{path}: v{version} kernel={kernel_size} ramdisk={ramdisk_size} cmdline={cmdline!r}")


def unpack_vendor_boot(path, out):
    d = open(path, "rb").read()
    assert d[:8] == b"VNDRBOOT", path
    version, page = struct.unpack_from("<2I", d, 8)
    vendor_ramdisk_size = struct.unpack_from("<I", d, 24)[0]
    cmdline = d[28:28 + 2048].split(b"\0")[0].decode()
    header_size, dtb_size = struct.unpack_from("<2I", d, 2096)
    table_size, entries, entry_size, bootconfig_size = struct.unpack_from("<4I", d, 2112)
    off = align(header_size, page)
    ramdisk = d[off:off + vendor_ramdisk_size]
    off += align(vendor_ramdisk_size, page)
    dtb = d[off:off + dtb_size]
    off += align(dtb_size, page)
    off += align(table_size, page)
    bootconfig = d[off:off + bootconfig_size]
    open(os.path.join(out, "vendor_ramdisk"), "wb").write(ramdisk)
    if dtb:
        open(os.path.join(out, "vendor_dtb"), "wb").write(dtb)
    open(os.path.join(out, "vendor_bootconfig"), "wb").write(bootconfig)
    print(f"{path}: v{version} page={page} vendor_ramdisk={vendor_ramdisk_size} dtb={dtb_size} "
          f"ramdisk_entries={entries} bootconfig={bootconfig_size}")
    print(f"vendor cmdline: {cmdline!r}")


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    unpack_boot(os.path.join(src, "boot.img"), out, "")
    unpack_boot(os.path.join(src, "init_boot.img"), out, "init_")
    unpack_vendor_boot(os.path.join(src, "vendor_boot.img"), out)


if __name__ == "__main__":
    main()
