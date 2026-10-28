#!/usr/bin/env python3
"""Compare portable random streams/choice histories with original x86 code.

Rnd/Rnd2, ResolveRandomJumps and CStoredRandom::Shuffle execute unchanged.
Stored-record lookup/allocation are controlled stubs for one table/owner at a
time. No engine allocator, full interpreter or inline-value parser executes.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX
from verify_native_prx import EXE_SHA256


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/random-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()))
    for prefix in ('random_selector','random_stream'):
        create=getattr(lib,prefix+'_create');create.argtypes=[ctypes.c_uint32];create.restype=ctypes.c_void_p
        destroy=getattr(lib,prefix+'_destroy');destroy.argtypes=[ctypes.c_void_p];destroy.restype=None
    lib.random_selector_pick.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint16),ctypes.c_size_t];lib.random_selector_pick.restype=ctypes.c_int
    lib.random_stream_draw.argtypes=[ctypes.c_void_p,ctypes.c_int32,ctypes.c_int];lib.random_stream_draw.restype=ctypes.c_int32
    lib.random_stream_state.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.POINTER(ctypes.c_uint32)];lib.random_stream_state.restype=None
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe))
    uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for section in pe.sections:uc.mem_write(0x400000+section.VirtualAddress,section.get_data())
    code,stored,stack,stop=0x2000000,0x3000000,0x10000000,0x11000000
    uc.mem_map(code,0x10000);uc.mem_map(stored,0x1000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    put=lambda p,n:uc.mem_write(p,struct.pack('<I',n&0xffffffff))
    get=lambda p:struct.unpack('<I',uc.mem_read(p,4))[0]
    def call(address,argument):
        put(stack+0x1000,stop);put(stack+0x1004,argument);uc.reg_write(UC_X86_REG_ESP,stack+0x1000)
        uc.emu_start(address,stop,count=100000)
        assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original random helper did not return'
        return uc.reg_read(UC_X86_REG_EAX)
    exists=False
    def finish(emu,value):
        sp=emu.reg_read(UC_X86_REG_ESP);ret=get(sp);emu.reg_write(UC_X86_REG_EAX,value);emu.reg_write(UC_X86_REG_ESP,sp+4);emu.reg_write(UC_X86_REG_EIP,ret)
    def lookup(emu,address,size,user):finish(emu,stored if exists else 0)
    def allocate(emu,address,size,user):
        nonlocal exists
        exists=True;uc.mem_write(stored,b'\0'*0x60);finish(emu,stored)
    uc.hook_add(UC_HOOK_CODE,lookup,begin=0x46f7a0,end=0x46f7a0)
    uc.hook_add(UC_HOOK_CODE,allocate,begin=0x470bb0,end=0x470bb0)
    put(0x6f3530,1)
    seeds=(0,1,123,0x7fffffff,0x80000000,0xffffffff)
    stream_checks=choice_checks=0
    for seed in seeds:
        call(0x403da0,seed);native=lib.random_stream_create(seed)
        try:
            for i in range(300):
                second=i%3==0;n=(0,1,2,66,32768,65535,0x7fffffff,-1,-0x80000000)[i%9]
                original=ctypes.c_int32(call(0x403e30 if second else 0x403de0,n)).value
                actual=lib.random_stream_draw(native,n,second);assert actual==original,(seed,i,n,actual,original)
                words=(ctypes.c_uint32*3)();lib.random_stream_state(native,second,words)
                original_state=struct.unpack('<III',uc.mem_read(0x690fc8 if second else 0x690fd4,12))
                assert tuple(words)==original_state;stream_checks+=1
        finally:lib.random_stream_destroy(native)
    for opcode in (0x2f,0x37,0x40,0x41):
        for weights in ((1,),(1,1),(0,5,0),(1,2,7),(32767,32767),(1,)*66):
            if opcode==0x41 and len(weights)==1:continue
            for seed in seeds:
                call(0x403da0,seed);exists=False;native=lib.random_selector_create(seed)
                count=len(weights);offsets=5+count*2;header=bytearray([opcode])+struct.pack('<I',count)+struct.pack('<'+'H'*count,*weights)
                targets=[]
                for i in range(count):
                    field=offsets+4*i;target=offsets+4*count+5*i;targets.append(code+target);header+=struct.pack('<I',target-field-4)
                for i in range(count):header+=b'\x16'+struct.pack('<I',i+100)
                uc.mem_write(code,bytes(header));w=(ctypes.c_uint16*count)(*weights)
                try:
                    for i in range(100):
                        destination=call(0x470c60,code);assert destination in targets
                        original=targets.index(destination);actual=lib.random_selector_pick(native,opcode,w,count)
                        assert actual==original,(hex(opcode),count,seed,i,actual,original);choice_checks+=1
                finally:lib.random_selector_destroy(native)
    report={'stream_checks':stream_checks,'choice_checks':choice_checks,'mismatches':0,'scope':__doc__.strip()}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='scope'}))

if __name__=='__main__':main()
