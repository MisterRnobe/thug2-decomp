#!/usr/bin/env python3
"""Compare packed QB parsing with independent traversal and adapted PC x86.

The supplied files have symbol/array types at byte 2 and component type/flags
at byte 1. Original PC routines expect byte 1 and byte 0 respectively. ONLY
test copies are adapted; the reader and fixtures retain the original layout.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX
from verify_native_prx import EXE_SHA256


def reference(b):
    u = lambda p: struct.unpack_from('<I', b, p)[0]
    adapted = bytearray(b)
    refs, symbols, scripts = [], [], []
    align = lambda p: (p+3) & ~3

    def ref(field):
        target = u(field)
        refs.append((field, target))
        return target

    def value(t, field, end):
        if t in (3, 4):
            assert ref(field) == end
            return align(b.index(0, end)+1)
        if t in (5, 6):
            assert ref(field) == end
            return end+(12 if t == 5 else 16)
        if t == 7:
            assert ref(field) == end
            crc, decoded, stored = struct.unpack_from('<III', b, end)
            scripts.append((end, crc, decoded, stored))
            return end+12+align(stored)
        if t == 10:
            return structure(ref(field))
        if t == 12:
            return array(ref(field))
        return end

    def structure(p):
        end, component = p+8, u(p+4)
        if component:
            assert ref(p+4) == end
        while component:
            # Copy type/flag byte to the PC field. Do not reinterpret host structs.
            adapted[component] = b[component+1]
            t = b[component+1] >> 1
            assert t != 7, 'Raw script components are absent from this fixture corpus'
            end = value(t, component+8, component+16)
            field = component+12
            component = u(field)
            if component:
                assert ref(field) == end
        return end

    def array(p):
        adapted[p+1] = b[p+2]
        t, count = b[p+2], u(p+4)
        end = p+12
        table = p+8
        if count > 1:
            table = ref(p+8)
            assert table == end
            end += count*4
        for i in range(count):
            field = table+i*4
            if t in (3, 4):
                assert ref(field) == end
                end = b.index(0, end)+1
            else:
                end = value(t, field, end)
        return align(end)

    assert u(0) == 0xb075ef6f and u(4) == len(b)
    p = 28
    while p < len(b):
        adapted[p+1] = b[p+2]
        t = b[p+2]
        end = value(t, p+12, p+20)
        symbols.append((p, end, u(p+4), t))
        assert end > p and end <= len(b)
        p = end
    assert p == len(b)
    return adapted, refs, symbols, scripts


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--data', type=Path, default=Path('work/english-game/Game/Data'))
    ap.add_argument('--exe', type=Path, default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library', type=Path, default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report', type=Path, default=Path('work/native-validation/precompiled-oracle.json'))
    args = ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest() == EXE_SHA256, 'Unsupported EXE'
    lib = ctypes.CDLL(str(args.library.resolve()))
    for fn in (lib.image_refs, lib.image_symbols):
        fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t]
        fn.restype = ctypes.c_size_t
    lib.image_script_code.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    lib.image_script_code.restype = ctypes.c_size_t
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pe = pefile.PE(str(args.exe))
    uc.mem_map(0x400000, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    for section in pe.sections:
        uc.mem_write(0x400000+section.VirtualAddress, section.get_data())
    base, output, stack, stop = 0x1000000, 0x4000000, 0x10000000, 0x11000000
    uc.mem_map(base, 0x2000000); uc.mem_map(output, 0x2000000)
    uc.mem_map(stack, 0x2000); uc.mem_map(stop, 0x1000)

    def call(address, args, budget):
        uc.mem_write(stack+0x1000, struct.pack('<'+'I'*(1+len(args)), stop, *args))
        uc.reg_write(UC_X86_REG_ESP, stack+0x1000)
        uc.emu_start(address, stop, timeout=20_000_000, count=budget)
        assert uc.reg_read(UC_X86_REG_EIP) == stop, 'Original routine did not return'
        return uc.reg_read(UC_X86_REG_EAX)

    results = []
    total_refs = total_symbols = total_scripts = compressed = 0
    for path in sorted(args.data.rglob('*.qb.xbx')):
        try:
            b = path.read_bytes()
            adapted, refs, symbols, scripts = reference(b)
            data = (ctypes.c_uint64*max(1, len(refs)*2))()
            n = lib.image_refs(b, len(b), data, len(data))
            assert n == len(refs), 'Reference count mismatch'
            native = [(data[i*2], data[i*2+1]) for i in range(n)]
            assert sorted(native) == sorted(refs), 'Offset-reference mismatch'
            data = (ctypes.c_uint64*max(1, len(symbols)*4))()
            n = lib.image_symbols(b, len(b), data, len(data))
            assert n == len(symbols)
            assert [tuple(data[i*4+j] for j in range(4)) for i in range(n)] == symbols
            expected = bytearray(adapted)
            for field, target in refs:
                struct.pack_into('<I', expected, field, base+target)
            uc.mem_write(base, bytes(adapted))
            # Callback is null: no game symbol tables, allocators or file I/O.
            call(0x46ca50, [base, base, 0, 0, 0], len(b)*100+10000)
            relocated = bytes(uc.mem_read(base, len(b)))
            assert relocated == expected, 'Original PC relocation differs from predicted references'
            # Read every stored body through the native API and compare the
            # three compressed bodies against original Lzss::Decode as well.
            for i, (offset, crc, size, stored) in enumerate(scripts):
                native_code = ctypes.create_string_buffer(size)
                assert lib.image_script_code(b, len(b), i, native_code, size) == size
                payload = b[offset+12:offset+12+stored]
                if stored < size:
                    uc.mem_write(base, payload)
                    uc.mem_write(output, b'\xa5'*(size+16))
                    end = call(0x4014f0, [base, output, stored], size*100+10000)
                    assert end == output+size
                    assert bytes(uc.mem_read(output+size, 16)) == b'\xa5'*16
                    decoded = bytes(uc.mem_read(output, size)); compressed += 1
                else:
                    decoded = payload
                assert decoded == native_code.raw, 'Stored script decode mismatch'
                uc.mem_write(base, decoded)
                assert call(0x46f960, [base], size*100+10000) == crc, 'Original script CRC mismatch'
            total_refs += len(refs); total_symbols += len(symbols); total_scripts += len(scripts)
            results.append(dict(file=path.relative_to(args.data).as_posix(), sha256=hashlib.sha256(b).hexdigest(),
                                symbols=len(symbols), references=len(refs), scripts=len(scripts)))
        except Exception as e:
            raise RuntimeError(f'{path}: {e}') from e
    assert results, 'No precompiled fixtures'
    result = dict(exe_sha256=EXE_SHA256, files=len(results), symbols=total_symbols,
                  references=total_refs, script_checksums=total_scripts, compressed_scripts=compressed,
                  oracle='PC relocation after enum-byte adaptation of test copies only', entries=results)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS: {len(results)} adapted PC relocations, {total_symbols} symbols, {total_refs} offsets; '
          f'{total_scripts} original script CRCs, {compressed} original LZSS decodes', flush=True)


if __name__ == '__main__':
    main()
