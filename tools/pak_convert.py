#!/usr/bin/env python3
"""pak_convert.py - Controller Pak saves between kig and emulators.

kig (ultramodern) keeps a pak as a folder, saves/controllerpak_port<N>/:
    controllerpak_header.bin   16 x 32-byte entries, little-endian: u32 file_size, u32 game_code,
                               u16 company_code, char ext_name[4], char game_name[16], u16 padding
    controllerpak_file_<n>.bin note n's data, N64 byte order
Emulators (and tools/refemu.py's core) use a raw 32 KB pak image (.mpk): 128 pages of 256 bytes with
the libultra file system: page 0 ID area, page 1 inode table, page 2 its backup, pages 3-4 the
16-entry directory, pages 5-127 data (one bank).

Usage:
    python tools/pak_convert.py to-image <folder> <out.mpk>
    python tools/pak_convert.py to-folder <in.mpk> <folder>
"""
import struct
import sys
from pathlib import Path

PAGE = 256
PAGES = 128
FIRST_DATA_PAGE = 5      # 1 (ID) + 2 (inode + backup) + 2 (directory), one bank
MAX_NOTES = 16
INODE_END, INODE_FREE = 0x0001, 0x0003
HEADER_FMT = "<IIH4s16sH"  # kig's controllerpak_header.bin entry


def _id_block():
    """A valid pack ID: libultra checks the checksums (sum of the first 14 big-endian halfwords, and
    0xFFF2 minus it)."""
    body = struct.pack(">IIQQHBB", 0xFFFFFFFF, 0, 0x4B4947_00000001, 0x0000_0000_0000_0001, 0x0001, 1, 0)
    s = sum(struct.unpack(">14H", body)) & 0xFFFF
    return body + struct.pack(">HH", s, (0xFFF2 - s) & 0xFFFF)


def _inode_checksum(table):
    return sum(table[FIRST_DATA_PAGE * 2:]) & 0xFF


def folder_to_image(folder):
    """32 KB pak image from a kig pak folder (missing folder or header = an empty formatted pak)."""
    folder = Path(folder)
    img = bytearray(PAGES * PAGE)
    page0 = bytearray(PAGE)
    for off in (0x20, 0x60, 0x80, 0xC0):
        page0[off:off + 32] = _id_block()
    img[0:PAGE] = page0
    inodes = [0] * PAGES
    for p in range(FIRST_DATA_PAGE, PAGES):
        inodes[p] = INODE_FREE
    directory = bytearray(2 * PAGE)
    header = (folder / "controllerpak_header.bin").read_bytes() if (folder / "controllerpak_header.bin").exists() else b""
    next_page = FIRST_DATA_PAGE
    for n in range(min(MAX_NOTES, len(header) // 32)):
        size, game_code, company, ext, name, _ = struct.unpack(HEADER_FMT, header[n * 32:(n + 1) * 32])
        if not (size and game_code and company):
            continue
        data = (folder / f"controllerpak_file_{n}.bin").read_bytes()[:size]
        pages = (size + PAGE - 1) // PAGE
        if next_page + pages > PAGES:
            raise ValueError(f"note {n} does not fit in the pak")
        chain = list(range(next_page, next_page + pages))
        for i, p in enumerate(chain):
            inodes[p] = chain[i + 1] if i + 1 < len(chain) else INODE_END
            chunk = data[i * PAGE:(i + 1) * PAGE]
            img[p * PAGE:p * PAGE + len(chunk)] = chunk
        directory[n * 32:(n + 1) * 32] = struct.pack(">IHHBbH4s16s", game_code, company, chain[0], 0x02, 0, 0, ext, name)
        next_page += pages
    table = bytearray(struct.pack(">128H", *inodes))
    table[1] = _inode_checksum(table)
    img[1 * PAGE:2 * PAGE] = table
    img[2 * PAGE:3 * PAGE] = table
    img[3 * PAGE:5 * PAGE] = directory
    return bytes(img)


def image_to_folder(img, folder):
    """Writes a 32 KB pak image's notes as a kig pak folder."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    inodes = struct.unpack(">128H", img[PAGE:2 * PAGE])
    header = bytearray(MAX_NOTES * 32)
    for n in range(MAX_NOTES):
        game_code, company, start, status, _, _, ext, name = struct.unpack(">IHHBbH4s16s", img[3 * PAGE + n * 32:3 * PAGE + (n + 1) * 32])
        if not (game_code and company):
            (folder / f"controllerpak_file_{n}.bin").unlink(missing_ok=True)
            continue
        data, p, seen = bytearray(), start & 0xFF, set()
        while FIRST_DATA_PAGE <= p < PAGES and p not in seen:
            seen.add(p)
            data += img[p * PAGE:(p + 1) * PAGE]
            nxt = inodes[p]
            if nxt == INODE_END:
                break
            p = nxt & 0xFF
        (folder / f"controllerpak_file_{n}.bin").write_bytes(bytes(data))
        header[n * 32:(n + 1) * 32] = struct.pack(HEADER_FMT, len(data), game_code, company, ext, name, 0)
    (folder / "controllerpak_header.bin").write_bytes(bytes(header))


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("to-image", "to-folder"):
        print(__doc__)
        return 2
    if sys.argv[1] == "to-image":
        Path(sys.argv[3]).write_bytes(folder_to_image(sys.argv[2]))
    else:
        image_to_folder(Path(sys.argv[2]).read_bytes(), sys.argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main())
