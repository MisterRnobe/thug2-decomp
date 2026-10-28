#!/usr/bin/env python3
"""Differential checks for QB repeat/if control decisions, not full CScript.

Repeat's count is pre-initialized to avoid original parameter allocators. The
original if's ExecuteCommand is a controlled boolean stub. No engine executes.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX, UC_X86_REG_EAX
from verify_native_prx import EXE_SHA256


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/runtime-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()))
    lib.runtime_repeat_probe.argtypes=[ctypes.c_uint32,ctypes.c_int];lib.runtime_repeat_probe.restype=ctypes.c_int
    lib.runtime_if_probe.argtypes=[ctypes.c_int,ctypes.c_int];lib.runtime_if_probe.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe))
    uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for s in pe.sections:uc.mem_write(0x400000+s.VirtualAddress,s.get_data())
    script,loops,code,stack,stop=0x1000000,0x1001000,0x2000000,0x10000000,0x11000000
    uc.mem_map(script,0x4000);uc.mem_map(code,0x1000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    put=lambda p,n:uc.mem_write(p,struct.pack('<I',n))
    get=lambda p:struct.unpack('<I',uc.mem_read(p,4))[0]
    def call(address):
        put(stack+0x1000,stop);uc.reg_write(UC_X86_REG_ESP,stack+0x1000);uc.reg_write(UC_X86_REG_ECX,script)
        uc.emu_start(address,stop,count=2000)
        assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original helper did not return'
    checks=0
    for counted in (False,True):
        for n in (0,1,2,3,5,10,255,256,0x7fffffff,0x80000000,0xffffffff):
            uc.mem_write(script,b'\0'*0x200);uc.mem_write(loops,b'\0'*32)
            put(script+0xc,code);put(script+0x18,loops);put(script+0x2c,loops)
            put(loops,code+0x100);put(loops+4,code+0x200)
            uc.mem_write(loops+8,bytes([int(counted),0]));put(loops+12,n)
            call(0x472ab0)
            destination=get(script+0xc)
            expected=1 if destination==code+0x200 else 2
            assert destination in (code+0x100,code+0x200)
            assert get(loops+12)==((n-1)&0xffffffff if counted else n)
            assert get(script+0x18)==(0 if counted and n==1 else loops)
            assert lib.runtime_repeat_probe(n,counted)==expected,(n,counted)
            checks+=1
    truth=False;condition_end=code+20
    def command_stub(emu,address,size,user):
        put(script+0xc,condition_end)
        sp=emu.reg_read(UC_X86_REG_ESP);ret=get(sp)
        emu.reg_write(UC_X86_REG_EAX,int(truth));emu.reg_write(UC_X86_REG_ESP,sp+4);emu.reg_write(UC_X86_REG_EIP,ret)
    uc.hook_add(UC_HOOK_CODE,command_stub,begin=0x474b40,end=0x474b40)
    for truth in (False,True):
        for negate in (False,True):
            uc.mem_write(script,b'\0'*0x200);put(script+0xc,code)
            uc.mem_write(code,bytes([0x47,99,0,0x39 if negate else 0x16])+b'\0'*100)
            call(0x475050)
            target=get(script+0xc);assert target in (condition_end,code+100)
            expected=1 if target==condition_end else 2
            assert lib.runtime_if_probe(truth,negate)==expected,(truth,negate)
            checks+=1
    report=dict(control_checks=checks,repeat_checks=22,if_checks=4,
                scope='Pre-initialized repeat; boolean ExecuteCommand stub for original if; native scenarios execute QB and registered test hosts')
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {checks} original/native loop/if control decisions',flush=True)


if __name__=='__main__':main()
