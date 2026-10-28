#!/usr/bin/env python3
"""Compare portable wait setters/updates to the supported x86 EXE.

The slomo-duration and game-clock providers are controlled stubs. This checks
wait decisions/rounding, not the original scheduler, CFunc argument parser,
object waits or clock providers. The original file is opened read-only.
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
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX
from verify_native_prx import EXE_SHA256


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/wait-oracle.json'))
    args=ap.parse_args()
    assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()));words=(ctypes.c_uint32*5)()
    lib.wait_make.argtypes=[ctypes.c_uint,ctypes.c_uint32,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]
    lib.wait_make.restype=ctypes.c_int
    lib.wait_advance.argtypes=[ctypes.POINTER(ctypes.c_uint32),ctypes.c_double,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint)]
    lib.wait_advance.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe))
    uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for section in pe.sections:uc.mem_write(0x400000+section.VirtualAddress,section.get_data())
    script,frame,stack,stop=0x1000000,0x1001000,0x10000000,0x11000000
    uc.mem_map(script,0x2000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    def put(p,n):uc.mem_write(p,struct.pack('<I',n))
    def get(p):return struct.unpack('<I',uc.mem_read(p,4))[0]
    def call(address,argument=None):
        put(stack+0x1000,stop)
        if argument is not None:put(stack+0x1004,argument)
        uc.reg_write(UC_X86_REG_ESP,stack+0x1000);uc.reg_write(UC_X86_REG_ECX,script)
        uc.emu_start(address,stop,count=5000)
        assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original wait helper did not return'
    # Stub only the time providers. fld qword [frame]; ret, and mov eax,[frame+8]; ret.
    uc.mem_write(0x5beaa0,b'\xdd\x05'+struct.pack('<I',frame)+b'\xc3')
    uc.mem_write(0x44ece0,b"\xa1"+struct.pack("<I",frame+8)+b"\xc3")
    put(0x696d60,0)
    def clock(now,seconds):
        uc.mem_write(frame,struct.pack('<d',seconds))
        put(frame+8,now)
    def original():
        return [get(script+0xa4),get(script+0x98),get(script+0xa0),get(script+0xa8),uc.mem_read(script+0x9c,1)[0]]
    def compare(kind):
        actual=original();expected=list(words)
        # Setters intentionally leave unrelated fields unchanged in CScript.
        fields={1:[0,1],11:[0,1],2:[0,2,4],4:[0,2,3]}[kind]
        assert all(actual[i]==expected[i] for i in fields),(kind,actual,expected)
    rng=random.Random(0x74687567);setters={1:0x472bd0,11:0x472bf0,2:0x472c10,4:0x473310}
    amounts=[0,1,2,15,16,17,33,1000,0x7fffffff,0x80000000,0xffffffff]+[rng.getrandbits(32) for _ in range(100)]
    checks=0
    for kind,address in setters.items():
        for amount in amounts:
            uc.mem_write(script,b'\0'*0x200);now=0xfffffff0;clock(now,0)
            call(address,amount);assert lib.wait_make(kind,amount,now,words)==0
            compare(kind);checks+=1;gate=ctypes.c_uint(0)
            # First/immediate check, repeated and zero/slowed/equal frame checks.
            durations=[0,1/60,0,1/120,1/30]
            if kind==2:durations += [struct.unpack('<f',struct.pack('<I',words[2]))[0],1/60]
            for i,seconds in enumerate(durations):
                now=(now+[0,1,15,17,33,0x7fffffff,0x80000000][i])&0xffffffff
                gate.value=i%2;put(0x6f352c,gate.value);clock(now,seconds)
                call(0x473050);assert lib.wait_advance(words,seconds,now,ctypes.byref(gate))==0
                compare(kind);assert get(0x6f352c)==gate.value;checks+=1
    report={'checks':checks,'setter_cases':len(amounts)*len(setters),'mismatches':0,
            'scope':'wait setters and UpdateWait; controlled clock providers; not CFunc_Wait or full scheduler'}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
