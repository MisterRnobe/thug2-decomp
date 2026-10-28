#!/usr/bin/env python3
"""Compare native RandomRange with the supported original x86 helper.

ParseRandomRange, its ftol helper and both RNG streams execute unchanged.
Compare type/bits, stop PC and subsequent 15-bit probes on both streams.
Finite inputs only; no engine allocator or full interpreter runs.
"""
import argparse
import ctypes
import hashlib
import json
import random
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_FPCW
from verify_native_prx import EXE_SHA256


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/range-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()));lib.random_selector_create.argtypes=[ctypes.c_uint32];lib.random_selector_create.restype=ctypes.c_void_p;lib.random_selector_destroy.argtypes=[ctypes.c_void_p]
    lib.random_selector_range.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_int,ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint32)];lib.random_selector_range.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe));uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for section in pe.sections:uc.mem_write(0x400000+section.VirtualAddress,section.get_data())
    code,component,stack,stop=0x2000000,0x3000000,0x10000000,0x11000000
    uc.mem_map(code,0x1000);uc.mem_map(component,0x1000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000);uc.reg_write(UC_X86_REG_FPCW,0x037f)
    def put(p,n):uc.mem_write(p,struct.pack('<I',n&0xffffffff))
    def get(p):return struct.unpack('<I',uc.mem_read(p,4))[0]
    def call(address,argument):
        put(stack+0x1000,stop);put(stack+0x1004,argument);uc.reg_write(UC_X86_REG_ESP,stack+0x1000)
        uc.emu_start(address,stop,count=10000);assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original range helper did not return';return uc.reg_read(UC_X86_REG_EAX)
    bits=lambda f:struct.unpack('<I',struct.pack('<f',f))[0]
    special=[bits(f) for f in (0.0,-0.0,1.0,-1.0,2.0,0.5,-0.5,0.9,-0.9,1e-5,-1e-5,32767.0,32768.0,65536.0,-2147483648.0,2147483520.0,2147483648.0,4294967296.0)]
    special += [1,0x80000001,0x7f7fffff,0xff7fffff,0x00800000,0x80800000]
    for f in (0.0,1.0,-1.0,2.0,-2.0):
        center=bits(f)
        for delta in (-2,-1,1,2):
            candidate=(center+delta)&0xffffffff
            if candidate&0x7f800000!=0x7f800000:special.append(candidate)
    for f in (1e-5,-1e-5,0.99999,1.00001):
        center=bits(f);special.extend([(center-1)&0xffffffff,center,(center+1)&0xffffffff])
    pairs=[(bits(-0.5),bits(0.5))]+[(a,b) for a in special for b in special]
    rng=random.Random(0x72616e6765)
    for _ in range(2500):
        a,b=rng.getrandbits(32),rng.getrandbits(32)
        if a&0x7f800000!=0x7f800000 and b&0x7f800000!=0x7f800000:pairs.append((a,b))
    checks=probes=0;counts={1:0,2:0}
    for seed in (0,1,123,3264,0x7fffffff,0x80000000,0xffffffff):
        call(0x403da0,seed);native=lib.random_selector_create(seed)
        try:
            for i,(a,b) in enumerate(pairs):
                second=i%2;uc.mem_write(code,b'\x1f'+struct.pack('<II',a,b));uc.mem_write(component,b'\0'*16)
                uc.reg_write(UC_X86_REG_EAX,code);uc.reg_write(UC_X86_REG_EBX,second);destination=call(0x46fbd0,component)
                original_kind=uc.mem_read(component,1)[0]>>1;original_bits=get(component+8)
                kind,value=ctypes.c_uint32(),ctypes.c_uint32();error=lib.random_selector_range(native,a,b,second,ctypes.byref(kind),ctypes.byref(value))
                assert not error and (kind.value,value.value)==(original_kind,original_bits),(seed,i,hex(a),hex(b),original_kind,hex(original_bits),kind.value,hex(value.value),error)
                assert destination==code+9;counts[original_kind]+=1;checks+=1
                for stream in (0,1):
                    original=call(0x403e30 if stream else 0x403de0,32768)
                    assert lib.random_selector_range(native,bits(0.0),bits(32767.0),stream,ctypes.byref(kind),ctypes.byref(value))==0
                    assert kind.value==1 and value.value==original;probes+=1
        finally:lib.random_selector_destroy(native)
    report={'range_checks':checks,'subsequent_stream_probes':probes,'integer_results':counts[1],'float_results':counts[2],'mismatches':0,'scope':__doc__.strip()}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='scope'}))

if __name__=='__main__':main()
