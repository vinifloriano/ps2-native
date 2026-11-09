#!/usr/bin/env python3
"""Code windows that hold overlays (Grand Theft Auto: San Andreas: eight .NM files, all at 0x88D880). No game data.

1. scripts/windows.py list: empty executable segments that begin at the same address are one window (alternatives), while a
   segment at another address in the same 16 KiB unit still moves up a unit.
2. eerecomp --live-image: an image with an "MWo3" overlay header checks only the overlay's own words. The code before the header
   (the program's) and after the overlay's size and .bss (left over from a bigger overlay loaded before) is not checked, and
   neither is a zero word.

usage: window_overlays.py --recompiler <eerecomp>
"""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def elf_with_segments(segments):
    """A little-endian MIPS ELF with only program headers: (vaddr, filesz, memsz, flags) each."""
    header = bytearray(52)
    header[0:4] = b'\x7fELF'
    header[4], header[5], header[6] = 1, 1, 1
    struct.pack_into('<HHIIIIIHHHHHH', header, 16, 2, 8, 1, 0x100000, 52, 0, 0, 52, 32, len(segments), 40, 0, 0)
    table = bytearray()
    for vaddr, filesz, memsz, flags in segments:
        table += struct.pack('<8I', 1, 0x1000 if filesz else 0, vaddr, vaddr, filesz, memsz, flags, 16)
    body = bytearray(0x1000 - 52 - len(table))
    return bytes(header + table + body + bytes(0x100))


def check_windows():
    with tempfile.TemporaryDirectory() as temporary:
        elf = Path(temporary) / 'overlays.elf'
        elf.write_bytes(elf_with_segments([
            (0x100000, 0x100, 0x100, 5),   # the program
            (0x200880, 0, 0x7000, 7),      # overlay A
            (0x200880, 0, 0x9000, 7),      # overlay B, same place: the same window
            (0x201000, 0, 0x1000, 7),      # another window in the same unit: moves up
        ]))
        out = subprocess.run([sys.executable, '-I', str(ROOT / 'scripts' / 'windows.py'), 'list', str(elf)], text=True,
                             capture_output=True, check=True).stdout.split('\n')
        bases = [line.split()[0] for line in out if line.strip()]
        if bases != ['00200000', '00204000']:
            sys.exit('windows.py list: expected 00200000 and 00204000, got %s' % bases)


def check_mask(eerecomp):
    base, header = 0x00200000, 0x880
    image = bytearray(0x1000)
    # before the overlay: a function of the program (addiu v0, zero, 7; jr ra; nop)
    struct.pack_into('<III', image, 0x100, 0x24020007, 0x03E00008, 0)
    # the overlay: header (magic, number, own address, size 0x40, .bss 0x20), then a function at +0x20
    struct.pack_into('<5I', image, header, 0x336F574D, 1, base + header, 0x40, 0x20)
    struct.pack_into('<IIII', image, header + 0x20, 0x24020001, 0x24420002, 0x03E00008, 0)
    # past size + .bss: left over from a bigger overlay (a function the program's calls would reach)
    struct.pack_into('<III', image, 0xA00, 0x24020005, 0x03E00008, 0)
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / 'window.bin'
        path.write_bytes(image)
        out = Path(temporary) / 'generated'
        result = subprocess.run([str(eerecomp), '--live-image', str(path), '--base', '%08X' % base, '--stamp', '7',
                                 '--entry', '%08X' % (base + header + 0x20), '--out', str(out), '--dense-checks'],
                                text=True, capture_output=True)
        if result.returncode != 0 or 'ok=1' not in result.stdout:
            sys.exit('eerecomp --live-image failed:\n%s\n%s' % (result.stdout, result.stderr))
        table = (out / 'table.cpp').read_text()
        found = re.search(r'_checks\[\] = \{([^}]*)\}', table)
        if not found:
            sys.exit('no check words in table.cpp')
        values = [int(v.strip().rstrip('u'), 16) for v in found.group(1).split(',') if v.strip()]
        checks = list(zip(values[0::2], values[1::2]))
        if not checks:
            sys.exit('the overlay has no check words')
        for offset, word in checks:
            if not header <= offset < header + 0x40 + 0x20:
                sys.exit('check word at +0x%X is outside the overlay (0x%X to 0x%X)' % (offset, header, header + 0x60))
            if word == 0:
                sys.exit('a zero word is checked at +0x%X' % offset)
        if (header + 0x20, 0x24020001) not in checks:
            sys.exit('the overlay function is not checked: %s' % checks)


def main():
    if len(sys.argv) != 3 or sys.argv[1] != '--recompiler':
        sys.exit(__doc__)
    check_windows()
    check_mask(Path(sys.argv[2]).resolve())
    print('window overlay checks passed')


if __name__ == '__main__':
    main()
