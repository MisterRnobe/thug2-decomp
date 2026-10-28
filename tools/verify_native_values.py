#!/usr/bin/env python3
"""Compare portable scalar QB operations with original CExpressionEvaluator.

Uses synthetic scalar components and an empty symbol table; no game allocators,
callbacks or expression-side engine calls. This is not interpreter execution.
"""
import argparse
import ctypes
import hashlib
import json
import math
import random
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX, UC_X86_REG_FPCW
from verify_native_prx import EXE_SHA256


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/values-oracle.json'))
    ap.add_argument('--samples',type=int,default=1000)
    args=ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()))
    lib.value_binary.argtypes=[ctypes.c_uint,ctypes.c_uint,ctypes.c_uint32,ctypes.c_uint,ctypes.c_uint32,
                               ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint32)]
    uc=Uc(UC_ARCH_X86,UC_MODE_32)
    pe=pefile.PE(str(args.exe));uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for section in pe.sections:uc.mem_write(0x400000+section.VirtualAddress,section.get_data())
    uc.mem_map(0,0x1000) # MSVC exception-chain slot at FS:[0].
    evaluator,stack,stop,buckets=0x1000000,0x10000000,0x11000000,0x2000000
    uc.mem_map(evaluator,0x2000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000);uc.mem_map(buckets,0x4000)
    uc.mem_write(0x6f3624,struct.pack('<I',buckets))
    uc.reg_write(UC_X86_REG_FPCW,0x037f)
    rng=random.Random(0x56414c554553)
    ints=[0,1,2,3,0xffffffff,0x80000000,0x7fffffff,0x1000001]
    floatbits=lambda f:struct.unpack('<I',struct.pack('<f',f))[0]
    floats=[floatbits(f) for f in (0.0,-0.0,1.0,-1.0,1e-7,1e-6,1e-5,16777216.0,16777218.0,3.402823466e38,1.17549435e-38)]
    values=[(1,n) for n in ints]+[(2,n) for n in floats]+[(13,n) for n in ints]
    pairs=[(a,b) for a in values for b in values]
    for _ in range(args.samples):
        a,b=rng.getrandbits(32),rng.getrandbits(32)
        # Finite payloads; NaN payload propagation is not standardized by C++.
        if a&0x7f800000==0x7f800000 or b&0x7f800000==0x7f800000:continue
        pairs.append(((rng.choice((1,2,13)),a),(rng.choice((1,2,13)),b)))
    checks=errors=nan_results=0
    for op in (7,8,10,11,12,13,18,20,50,51):
        for (ta,a),(tb,b) in pairs:
            if op==12 and ta==tb==1 and a==0x80000000 and b==0xffffffff:continue # Original IDIV fault.
            state=bytearray(0x200)
            state[0]=ta<<1;state[16]=tb<<1
            struct.pack_into('<I',state,8,a);struct.pack_into('<I',state,24,b)
            struct.pack_into('<I',state,0x140,1);struct.pack_into('<I',state,0x144,op);state[0x1e8]=1
            uc.mem_write(evaluator,bytes(state));uc.mem_write(0x6f2348,b'\0'*16)
            uc.mem_write(stack+0x1000,struct.pack('<I',stop));uc.reg_write(UC_X86_REG_ESP,stack+0x1000)
            uc.reg_write(UC_X86_REG_ECX,evaluator)
            uc.emu_start(0x46d450,stop,timeout=1_000_000,count=10000)
            assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original evaluator did not return'
            result=bytes(uc.mem_read(evaluator,0x200));original_error=struct.unpack_from('<I',result,0x1f0)[0]
            out_type,out_bits=ctypes.c_uint32(),ctypes.c_uint32()
            native_error=lib.value_binary(op,ta,a,tb,b,ctypes.byref(out_type),ctypes.byref(out_bits))
            case=(op,ta,hex(a),tb,hex(b))
            assert bool(original_error)==bool(native_error),('Error status mismatch',case,hex(original_error),native_error)
            if original_error:errors+=1
            else:
                kind,bits=result[0]>>1,struct.unpack_from('<I',result,8)[0]
                assert kind==out_type.value,('Result type mismatch',case,kind,out_type.value)
                both_nan=kind==2 and bits&0x7f800000==0x7f800000 and bits&0x7fffff and out_bits.value&0x7f800000==0x7f800000 and out_bits.value&0x7fffff
                if both_nan:nan_results+=1
                else:assert bits==out_bits.value,('Result bits mismatch',case,hex(bits),hex(out_bits.value))
            checks+=1
    report=dict(checks=checks,error_matches=errors,nan_payloads_not_compared=nan_results,
                x87_control_word='0x037f',excluded='Original INT_MIN/-1 IDIV fault; non-scalar values; NaN input payloads')
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {checks} original scalar operations; {errors} matching errors',flush=True)


if __name__=='__main__':main()
