#!/usr/bin/env python3
"""Check native token QB parsing against an independent slicer and original x86.

All loose .qb files and every .qb entry in PRX are checked, including shadowed
versions. Precompiled .qb.xbx symbol images are deliberately excluded.
"""
import argparse
import ctypes
import hashlib
import json
import random
import struct
import zlib
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX
from verify_native_prx import EXE_SHA256, entries


def reference(data):
    """Only frame widths and script CRCs; independent of the native reader."""
    one = set(bytes.fromhex('01 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 11 12 13 14 15 '
                            '20 21 22 23 24 25 26 27 28 29 2c 2d 30 32 33 34 35 36 38 39 '
                            '3a 3b 3c 3d 3e 3f 42 45'))
    four = {2, 0x16, 0x17, 0x18, 0x1a, 0x2e, 0x43, 0x44, 0x46}
    tokens, scripts = [], []
    pos, body, crc = 0, None, 0
    while pos < len(data):
        start = pos
        t = data[pos]
        pos += 1
        if t == 0:
            assert pos == len(data)
        elif t in one:
            pass
        elif t in four:
            pos += 4
        elif t in (0x1b, 0x1c):
            pos += 4+struct.unpack_from('<I', data, pos)[0]
        elif t in (0x1e, 0x1f):
            pos += 12 if t == 0x1e else 8
        elif t == 0x2b:
            pos = data.index(0, pos+4)+1
        elif t in (0x2f, 0x37, 0x40, 0x41):
            pos += 4+6*struct.unpack_from('<I', data, pos)[0]
        elif t in (0x47, 0x48, 0x49):
            pos += 2
        else:
            raise ValueError(f'Unknown token {t:x} at {start:x}')
        assert pos <= len(data)
        tokens.append((start, pos))
        if t == 0x23:
            assert body is None
            body, crc = start+6, 0
        elif t == 0x24:
            assert body is not None
            scripts.append((body, pos, crc ^ 0xffffffff))
            body = None
        elif body is not None and start >= body and t not in (1, 2):
            crc = zlib.crc32(data[start:pos], crc)
    assert tokens and data[tokens[-1][0]] == 0 and body is None
    return tokens, scripts


def fixtures(root, lib):
    for path in sorted(root.rglob('*.qb')):
        yield 'loose:'+path.relative_to(root).as_posix(), path.read_bytes()
    for path in sorted((root/'pre').glob('*.prx')):
        for index, name, size, compressed, payload in entries(path):
            if not name.lower().endswith(b'.qb'):
                continue
            if compressed:
                context = lib.decoder_create()
                try:
                    output = ctypes.create_string_buffer(size)
                    assert lib.decoder_run(context, payload, compressed, output, size) == 0
                    payload = output.raw
                finally:
                    lib.decoder_destroy(context)
            yield f'{path.name}:{index}:'+name.decode('latin1'), payload


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--data', type=Path, default=Path('work/english-game/Game/Data'))
    ap.add_argument('--exe', type=Path, default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library', type=Path, default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report', type=Path, default=Path('work/native-validation/qb-oracle.json'))
    ap.add_argument('--limit', type=int)
    args = ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest() == EXE_SHA256, 'Unsupported EXE'
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.decoder_create.restype = ctypes.c_void_p
    lib.decoder_destroy.argtypes = [ctypes.c_void_p]
    lib.decoder_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    for fn in (lib.qb_offsets, lib.qb_script_info):
        fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t]
        fn.restype = ctypes.c_size_t
    pe = pefile.PE(str(args.exe))
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    base = pe.OPTIONAL_HEADER.ImageBase
    uc.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    for section in pe.sections:
        uc.mem_write(base+section.VirtualAddress, section.get_data())
    src, stack, stop = 0x1000000, 0x10000000, 0x11000000
    uc.mem_map(src, 0x2000000)
    uc.mem_map(stack, 0x2000)
    uc.mem_map(stop, 0x1000)

    def original(address, pointer, budget):
        uc.mem_write(stack+0x1000, struct.pack('<II', stop, pointer))
        uc.reg_write(UC_X86_REG_ESP, stack+0x1000)
        uc.emu_start(address, stop, timeout=20_000_000, count=budget)
        assert uc.reg_read(UC_X86_REG_EIP) == stop, 'Original helper did not return'
        return uc.reg_read(UC_X86_REG_EAX)

    results = []
    rng = random.Random(0x51425448554732)
    token_count = script_count = x86_tokens = x86_scripts = 0
    for name, data in fixtures(args.data, lib):
        try:
            tokens, scripts = reference(data)
            offsets = (ctypes.c_uint64*len(tokens))()
            assert lib.qb_offsets(data, len(data), offsets, len(offsets)) == len(tokens)
            assert list(offsets) == [start for start, end in tokens], 'Token boundary mismatch'
            info = (ctypes.c_uint64*(3*len(scripts)))()
            assert lib.qb_script_info(data, len(data), info, len(info)) == len(scripts)
            assert list(info) == [n for s in scripts for n in s], 'Script range/CRC mismatch'
            assert len(data) < 0x2000000
            uc.mem_write(src, data)
            # Sample each observed token type plus 48 random non-EOF tokens.
            choices, types = set(), set()
            for i, (start, end) in enumerate(tokens[:-1]):
                if data[start] not in types:
                    types.add(data[start]); choices.add(i)
            choices.update(rng.sample(range(len(tokens)-1), min(48, len(tokens)-1)))
            for i in sorted(choices):
                start, end = tokens[i]
                actual = original(0x46f690, src+start, (end-start)*30+1000)
                assert actual == src+end, ('SkipToken mismatch', start)
                x86_tokens += 1
            for body, end, crc in rng.sample(scripts, min(2, len(scripts))):
                actual = original(0x46f960, src+body, (end-body)*100+10000)
                assert actual == crc, ('Script CRC mismatch', body)
                x86_scripts += 1
            token_count += len(tokens); script_count += len(scripts)
            results.append(dict(fixture=name, sha256=hashlib.sha256(data).hexdigest(),
                                tokens=len(tokens), scripts=len(scripts)))
        except Exception as e:
            raise RuntimeError(f'{name}: {e}') from e
        if len(results)%50 == 0:
            print(f'{len(results)} fixtures: {token_count} token boundaries and {script_count} script CRCs match', flush=True)
        if args.limit and len(results) >= args.limit:
            break
    assert results, 'No token QB fixtures found'
    result = dict(exe_sha256=EXE_SHA256, fixtures=len(results), token_boundaries=token_count,
                  script_checksums=script_count, x86_skip_checks=x86_tokens, x86_script_checks=x86_scripts,
                  limited=bool(args.limit), entries=results)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS: {len(results)} fixtures; {token_count} boundaries, {script_count} script CRCs; '
          f'x86: {x86_tokens} skip checks, {x86_scripts} script checks', flush=True)


if __name__ == '__main__':
    main()
