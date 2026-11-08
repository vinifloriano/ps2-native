#!/usr/bin/env python3
"""Harvest function names from a Neversoft-engine PS2 ELF (GH3, THPS, THUG, THAW...).

The Q-script engine registers its C functions in tables of {const char *name, CFunc fn} pairs in .data/.sdata. Each
pair is a string pointer into .rodata (a C identifier) followed by a code pointer into .text. This scans every 4-byte
aligned word of the data sections for runs of such pairs and writes them out as a function map.

    python3 -I tools/harvest_cfuncs.py disc/SLUS_216.72 analysis/cfuncs.tsv

Output: TSV with address, name, table address. Prints a summary (tables, functions, duplicates) to stdout.
Standard library only. The output is derived from the game's code: keep it out of any public repository.
"""
import re
import struct
import sys

IDENT = re.compile(rb'^[A-Za-z_][A-Za-z0-9_]{1,127}$')
MIN_RUN = 3  # pairs in a row before a run counts as a table


def sections(data):
    shoff = struct.unpack_from('<I', data, 0x20)[0]
    shnum, shstrndx = struct.unpack_from('<HH', data, 0x30)
    raw = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * 40) for i in range(shnum)]
    stroff = raw[shstrndx][4]
    out = {}
    for name_off, kind, _flags, addr, off, size, *_ in raw:
        end = data.index(b'\0', stroff + name_off)
        name = data[stroff + name_off:end].decode()
        if kind == 1 and addr and size:  # PROGBITS with an address
            out[name] = (addr, off, size)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    elf, out_path = sys.argv[1], sys.argv[2]
    data = open(elf, 'rb').read()
    secs = sections(data)
    text = secs['.text']
    rodata = secs['.rodata']

    def in_sec(sec, va):
        return sec[0] <= va < sec[0] + sec[2]

    def cstr(va):
        off = rodata[1] + (va - rodata[0])
        end = data.find(b'\0', off, off + 130)
        if end < 0:
            return None
        s = data[off:end]
        return s.decode() if IDENT.match(s) else None

    def pair_at(off):
        name_va, fn_va = struct.unpack_from('<II', data, off)
        if not in_sec(rodata, name_va) or not in_sec(text, fn_va) or fn_va & 3:
            return None
        name = cstr(name_va)
        return (name, fn_va) if name else None

    rows, tables = [], []
    for sec_name in ('.data', '.sdata'):
        if sec_name not in secs:
            continue
        addr, off, size = secs[sec_name]
        i = 0
        while i + 8 <= size:
            run = []
            j = i
            while j + 8 <= size:
                p = pair_at(off + j)
                if not p:
                    break
                run.append((addr + j, p))
                j += 8
            if len(run) >= MIN_RUN:
                tables.append((addr + i, len(run)))
                rows += [(fn, name, addr + i) for _at, (name, fn) in run]
                i = j
            else:
                i += 4

    by_addr = {}
    for fn, name, table in rows:
        by_addr.setdefault(fn, []).append(name)
    with open(out_path, 'w') as f:
        f.write('address\tname\ttable\n')
        for fn, name, table in sorted(rows):
            f.write('%08x\t%s\t%08x\n' % (fn, name, table))

    shared = {fn: names for fn, names in by_addr.items() if len(set(names)) > 1}
    print('tables: %d, entries: %d, distinct functions: %d, addresses with several names: %d'
          % (len(tables), len(rows), len(by_addr), len(shared)))
    for t, n in sorted(tables, key=lambda x: -x[1])[:10]:
        print('  table %08x: %d entries' % (t, n))


if __name__ == '__main__':
    main()
