#!/usr/bin/env python3
"""
patch_verneed.py — patch ELF VERNEED version requirements.

Replaces "GLIBC_2.28" and "GLIBC_2.29" version entries with "GLIBC_2.4",
allowing binaries built against Debian Bullseye (glibc 2.31) to load on
devices running glibc 2.23.

Usage: patch_verneed.py <binary>
"""

import sys
import struct


def elf_hash(name):
    h = 0
    for c in name:
        h = (h << 4) + ord(c)
        g = h & 0xf0000000
        if g:
            h ^= g >> 24
        h &= ~g
    return h


# Map old version strings to replacement (same length, null-padded).
# All entries must map to a version available on the target device (glibc 2.17+).
# Lengths must match exactly (null-pad shorter replacements).
PATCH_MAP = {
    b"GLIBC_2.25\x00": b"GLIBC_2.4\x00\x00",  # getrandom/getentropy (OpenSSL entropy)
    b"GLIBC_2.27\x00": b"GLIBC_2.4\x00\x00",  # memfd_create etc.
    b"GLIBC_2.28\x00": b"GLIBC_2.4\x00\x00",
    b"GLIBC_2.29\x00": b"GLIBC_2.4\x00\x00",
}

NEW_HASH = elf_hash("GLIBC_2.4")


def main():
    if len(sys.argv) != 2:
        print("Usage: patch_verneed.py <binary>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    # Verify ELF magic
    if data[:4] != b"\x7fELF":
        print("Not an ELF file", file=sys.stderr)
        sys.exit(1)

    ei_class = data[4]  # 1=32-bit, 2=64-bit
    ei_data = data[5]   # 1=LE, 2=BE
    if ei_class != 1:
        print("Only 32-bit ELF supported", file=sys.stderr)
        sys.exit(1)

    endian = "<" if ei_data == 1 else ">"

    # ELF32 header
    e_shoff    = struct.unpack_from(endian + "I", data, 32)[0]
    e_shentsize = struct.unpack_from(endian + "H", data, 46)[0]
    e_shnum    = struct.unpack_from(endian + "H", data, 48)[0]

    SHT_GNU_verneed = 0x6ffffffe

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link, sh_info, _align, _entsize = \
            struct.unpack_from(endian + "IIIIIIIIII", data, off)
        sections.append(dict(
            name_idx=sh_name, type=sh_type,
            offset=sh_offset, size=sh_size,
            link=sh_link, info=sh_info,
        ))

    # Locate .gnu.version_r (VERNEED) and its linked .dynstr
    verneed_sec = next((s for s in sections if s["type"] == SHT_GNU_verneed), None)
    if not verneed_sec:
        print("No .gnu.version_r section found — nothing to patch")
        sys.exit(0)

    dynstr_sec = sections[verneed_sec["link"]]
    dynstr_start = dynstr_sec["offset"]
    dynstr_end   = dynstr_start + dynstr_sec["size"]

    # ── Step 1: patch strings in .dynstr ──────────────────────────────────────
    patched_strings = {}   # old_abs_offset -> new_string (for reporting)
    dynstr_view = data[dynstr_start:dynstr_end]
    for old, new in PATCH_MAP.items():
        pos = 0
        while True:
            idx = dynstr_view.find(old, pos)
            if idx == -1:
                break
            abs_pos = dynstr_start + idx
            data[abs_pos : abs_pos + len(old)] = new
            dynstr_view = data[dynstr_start:dynstr_end]   # refresh view
            old_str = old.rstrip(b"\x00").decode()
            new_str = new.rstrip(b"\x00").decode()
            print(f"  [dynstr +0x{idx:04x}]  {old_str!r} → {new_str!r}")
            patched_strings[abs_pos] = new_str
            pos = idx + len(new)

    if not patched_strings:
        print("No GLIBC_2.28/2.29 strings found — binary is already compatible")
        sys.exit(0)

    # ── Step 2: patch vna_hash in VERNEED entries ─────────────────────────────
    # Elf32_Verneed  { u16 vn_version; u16 vn_cnt; u32 vn_file;
    #                  u32 vn_aux;    u32 vn_next; }
    # Elf32_Vernaux  { u32 vna_hash; u16 vna_flags; u16 vna_other;
    #                  u32 vna_name; u32 vna_next; }
    vn_off = verneed_sec["offset"]
    num_entries = verneed_sec["info"]

    for _ in range(num_entries):
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = \
            struct.unpack_from(endian + "HHIII", data, vn_off)

        vna_off = vn_off + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = \
                struct.unpack_from(endian + "IHHII", data, vna_off)

            # Read current name from (already-patched) dynstr
            name_start = dynstr_start + vna_name
            name_end   = data.index(0, name_start)
            name       = bytes(data[name_start:name_end]).decode()

            if name == "GLIBC_2.4" and vna_hash != NEW_HASH:
                print(f"  [vernaux +0x{vna_off - verneed_sec['offset']:04x}]  "
                      f"vna_hash 0x{vna_hash:08x} → 0x{NEW_HASH:08x}  ({name})")
                struct.pack_into(endian + "I", data, vna_off, NEW_HASH)

            if vna_next == 0:
                break
            vna_off += vna_next

        if vn_next == 0:
            break
        vn_off += vn_next

    # ── Write patched binary ──────────────────────────────────────────────────
    with open(path, "wb") as f:
        f.write(data)
    print(f"Patched {path}")


if __name__ == "__main__":
    main()
