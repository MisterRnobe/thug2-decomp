#!/usr/bin/env python3
"""Compare native PRE decoding with the supported original x86 machine code.

Only this optional test loads an EXE. The product library and CLI do not.
Uses existing project .venv dependencies (pefile and unicorn).
"""
import argparse
import ctypes
import hashlib
import json
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX

EXE_SHA256 = '7eb982acfa06ee0cb6884c4334d4c049258903ceaa287fd1d2e700a731b91632'


def entries(path):
    """Independent fixture traversal, not the native parser under test."""
    data = path.read_bytes()
    length, version, count = struct.unpack_from('<III', data)
    assert length == len(data) and version == 0xabcd0003
    pos = 12
    for index in range(count):
        size, compressed, names, uses, checksum = struct.unpack_from('<ii h H I', data, pos)
        assert size >= 0 and compressed >= 0 and names > 0
        name = data[pos+16:pos+16+names].split(b'\0', 1)[0]
        pos += 16+names
        stored = compressed or size
        payload = data[pos:pos+stored]
        assert len(payload) == stored
        yield index, name, size, compressed, payload
        pos += (stored+3) & ~3
    assert pos == len(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', type=Path, default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--archives', type=Path, default=Path('work/english-game/Game/Data/pre'))
    ap.add_argument('--library', type=Path, default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--cli', type=Path, default=Path('work/native-build/thug2-prx'))
    ap.add_argument('--report', type=Path, default=Path('work/native-validation/oracle.json'))
    ap.add_argument('--limit', type=int, help='Optional maximum compressed entries for a quick smoke check')
    args = ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest() == EXE_SHA256, 'Unsupported EXE'
    pe = pefile.PE(str(args.exe))
    assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    base = pe.OPTIONAL_HEADER.ImageBase
    uc.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    for section in pe.sections:
        uc.mem_write(base+section.VirtualAddress, section.get_data())
    src, dst, stack, stop = 0x1000000, 0x4000000, 0x10000000, 0x11000000
    uc.mem_map(src, 0x2000000)
    uc.mem_map(dst, 0x4000000)
    uc.mem_map(stack, 0x2000)
    uc.mem_map(stop, 0x1000)
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.decoder_create.restype = ctypes.c_void_p
    lib.decoder_destroy.argtypes = [ctypes.c_void_p]
    lib.decoder_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_void_p, ctypes.c_size_t]
    lib.checksum_name.argtypes = [ctypes.c_char_p]
    lib.checksum_name.restype = ctypes.c_uint32
    paths = sorted(args.archives.glob('*.prx'))
    assert paths, 'No PRX fixtures found'
    names = {name for path in paths for _, name, _, _, _ in entries(path)}
    names.update([b'', b'A/B.XBX', b'a\\b.xbx', bytes(range(1, 256))])
    for name in sorted(names):
        uc.mem_write(src, name+b'\0')
        uc.mem_write(stack+0x1000, struct.pack('<II', stop, src))
        uc.reg_write(UC_X86_REG_ESP, stack+0x1000)
        uc.emu_start(0x401b90, stop, count=len(name)*30+100)
        assert uc.reg_read(UC_X86_REG_EIP) == stop
        assert lib.checksum_name(name) == uc.reg_read(UC_X86_REG_EAX), ('CRC mismatch', name)
    print(f'{len(names)} name CRCs match original x86 code', flush=True)

    # Exercise real filesystem extraction in addition to the library ABI.
    # Keep only the first entry for each game name hash, as PreFile does.
    args.report.parent.mkdir(parents=True, exist_ok=True)
    sample_context = lib.decoder_create()
    try:
        with tempfile.TemporaryDirectory(prefix='extract-', dir=args.report.parent) as tmp:
            out = Path(tmp)/'files'
            subprocess.run([str(args.cli), 'extract', str(paths[0]), str(out)], check=True)
            seen = set()
            extracted = 0
            for _, name, size, compressed, payload in entries(paths[0]):
                crc = lib.checksum_name(name)
                if crc in seen:
                    continue
                seen.add(crc)
                if compressed:
                    output = ctypes.create_string_buffer(size)
                    assert lib.decoder_run(sample_context, payload, compressed, output, size) == 0
                    payload = output.raw
                actual = out.joinpath(*name.decode('ascii').replace('\\', '/').split('/')).read_bytes()
                assert actual == payload, ('Extraction mismatch', name)
                extracted += 1
            assert sum(p.is_file() for p in out.rglob('*')) == extracted
            refusal = subprocess.run([str(args.cli), 'extract', str(paths[0]), str(out)], capture_output=True)
            assert refusal.returncode != 0, 'Existing destination was not rejected'
            print(f'CLI extraction: {extracted} files checked; overwrite refused', flush=True)
    finally:
        lib.decoder_destroy(sample_context)
    context = lib.decoder_create()
    report = []
    count = total = 0
    started = time.monotonic()
    try:
        for path in paths:
            for index, name, size, compressed, payload in entries(path):
                if not compressed:
                    continue
                assert compressed < 0x2000000 and size+16 < 0x4000000
                output = ctypes.create_string_buffer(size)
                assert lib.decoder_run(context, payload, compressed, output, size) == 0, (path, name)
                uc.mem_write(src, payload)
                uc.mem_write(dst, b'\xa5'*(size+16))
                uc.mem_write(stack+0x1000, struct.pack('<IIII', stop, src, dst, compressed))
                uc.reg_write(UC_X86_REG_ESP, stack+0x1000)
                uc.emu_start(0x5b7df0, stop, timeout=20_000_000, count=size*100+10000)
                assert uc.reg_read(UC_X86_REG_EIP) == stop, ('Decoder did not return', path, name)
                original = bytes(uc.mem_read(dst, size))
                assert bytes(uc.mem_read(dst+size, 16)) == b'\xa5'*16, ('Output overrun', path, name)
                assert original == output.raw, ('Decode mismatch', path, name)
                report.append(dict(archive=path.name, index=index, name=name.decode('latin1'),
                                   size=size, sha256=hashlib.sha256(original).hexdigest()))
                count += 1
                total += size
                if args.limit and count >= args.limit:
                    break
            print(f'{path.name}: {count} compressed entries, {total} bytes matched', flush=True)
            if args.limit and count >= args.limit:
                break
    finally:
        lib.decoder_destroy(context)
    result = dict(exe_sha256=EXE_SHA256, compressed_entries=count, decoded_bytes=total,
                  name_crcs=len(names), extraction_archive=paths[0].name, extracted_files=extracted,
                  seconds=round(time.monotonic()-started, 2), limited=bool(args.limit), entries=report)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS: {count} original/native matches; report: {args.report}', flush=True)


if __name__ == '__main__':
    main()
