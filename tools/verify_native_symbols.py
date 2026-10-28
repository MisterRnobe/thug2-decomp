#!/usr/bin/env python3
"""Compare exported native symbol-routing topology with original PC lookup.

The synthetic PC table models names/types/aliases only. This checks lookup
routing and dangling-alias fallback, not expression evaluation or game loading.
Generate topology with symbols-corpus before running this optional test.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX
from verify_native_prx import EXE_SHA256


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--topology', type=Path, default=Path('work/native-validation/symbols-topology.json'))
    ap.add_argument('--exe', type=Path, default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--report', type=Path, default=Path('work/native-validation/symbols-oracle.json'))
    args = ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest() == EXE_SHA256, 'Unsupported EXE'
    nodes = json.loads(args.topology.read_text())['nodes']
    assert nodes and len({n['name'] for n in nodes}) == len(nodes)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pe = pefile.PE(str(args.exe))
    uc.mem_map(0x400000, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    for section in pe.sections:
        uc.mem_write(0x400000+section.VirtualAddress, section.get_data())
    buckets, entries, stack, stop = 0x1000000, 0x1010000, 0x10000000, 0x11000000
    uc.mem_map(buckets, (0x10000+len(nodes)*20+4095) & ~4095)
    uc.mem_map(stack, 0x2000); uc.mem_map(stop, 0x1000)
    addresses = {n['name']: entries+i*20 for i, n in enumerate(nodes)}
    heads = [0]*4096
    for n in nodes:
        slot = n['name'] & 4095
        kind = 13 if n['alias'] is not None else n['type']
        assert kind != 13 or n['alias'] is not None
        raw = n['alias'] if n['alias'] is not None else 0
        uc.mem_write(addresses[n['name']], struct.pack('<BBHIIII', 1, kind, 0, n['name'], 0, raw, heads[slot]))
        heads[slot] = addresses[n['name']]
    uc.mem_write(buckets, struct.pack('<4096I', *heads))
    uc.mem_write(0x6f3624, struct.pack('<I', buckets))

    def call(address, name, steps=1):
        uc.mem_write(stack+0x1000, struct.pack('<II', stop, name))
        uc.reg_write(UC_X86_REG_ESP, stack+0x1000)
        uc.emu_start(address, stop, timeout=1_000_000, count=1000+steps*500)
        assert uc.reg_read(UC_X86_REG_EIP) == stop, 'Lookup did not return within bound'
        return uc.reg_read(UC_X86_REG_EAX)

    resolved = dangling = skipped = 0
    for n in nodes:
        assert call(0x478cc0, n['name']) == addresses[n['name']], 'FindSymbol mismatch'
        if n['status'] in ('cycle', 'limit'):
            skipped += 1  # Original lookup can loop forever; native tests cover bounded diagnostics.
            continue
        if n['status'] == 'resolved':
            expected = addresses[n['target']]; resolved += 1
        else:
            assert n['status'] == 'missing' and n['last_alias'] is not None
            expected = addresses[n['last_alias']]; dangling += 1
        assert call(0x478cf0, n['name'], n['steps']) == expected, f'LookUpSymbol mismatch: {n["name"]:08x}'
    absent = 0
    while absent in addresses:
        absent += 1
    assert call(0x478cc0, absent) == 0 and call(0x478cf0, absent) == 0
    result = dict(exe_sha256=EXE_SHA256, definitions=len(nodes), find_checks=len(nodes)+1,
                  resolved_lookups=resolved, dangling_alias_fallbacks=dangling,
                  skipped_cycles_or_limits=skipped, missing_root_checks=2,
                  scope='Synthetic PC table from native topology; routing only')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS: {len(nodes)} original FindSymbol checks; {resolved} resolved lookups, '
          f'{dangling} dangling-alias fallbacks; absent root returns null', flush=True)


if __name__ == '__main__':
    main()
