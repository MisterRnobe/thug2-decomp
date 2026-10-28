#!/usr/bin/env python3
"""Check runtime random argument paths against original ParseArguments.

Original ParseArguments and ResolveRandomJumps/Rnd/Rnd2 execute. AddParameter
is a controlled literal-emission stub (int/float/name), without CStruct or
component allocators. Checks result parameters, stop PC and eight subsequent
stream probes. Expressions, structures and arrays are not x86-oracled here.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX
from verify_native_prx import EXE_SHA256


def choice(op,weights,branches):
    b=bytearray([op])+struct.pack('<I',len(weights))+struct.pack('<'+'H'*len(weights),*weights)
    offsets=len(b);b+=b'\0'*(4*len(weights));exits=[]
    for i,branch in enumerate(branches):
        field=offsets+i*4;struct.pack_into('<I',b,field,len(b)-field-4);b+=branch
        b+=b'\x2e';exits.append(len(b));b+=b'\0'*4
    for field in exits:struct.pack_into('<I',b,field,len(b)-field-4)
    return b


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/inline-random-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()));lib.runtime_inline_arguments.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t];lib.runtime_inline_arguments.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe));uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for s in pe.sections:uc.mem_write(0x400000+s.VirtualAddress,s.get_data())
    code,dest,stack,stop=0x2000000,0x3000000,0x10000000,0x11000000
    uc.mem_map(0,0x1000);uc.mem_map(code,0x10000);uc.mem_map(dest,0x1000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    def get(p):return struct.unpack('<I',uc.mem_read(p,4))[0]
    def put(p,n):uc.mem_write(p,struct.pack('<I',n))
    emitted=[]
    def add_literal(emu,address,size,user):
        p=emu.reg_read(UC_X86_REG_ECX);name=emu.reg_read(UC_X86_REG_EDX);token=uc.mem_read(p,1)[0]
        assert token in (0x16,0x17,0x1a),hex(token)
        emitted.append((name,{0x16:13,0x17:1,0x1a:2}[token],get(p+1)))
        sp=emu.reg_read(UC_X86_REG_ESP);ret=get(sp);emu.reg_write(UC_X86_REG_EAX,p+5);emu.reg_write(UC_X86_REG_ESP,sp+4);emu.reg_write(UC_X86_REG_EIP,ret)
    uc.hook_add(UC_HOOK_CODE,add_literal,begin=0x470130,end=0x470130)
    def call(address,arguments):
        uc.mem_write(stack+0x1000,struct.pack('<'+'I'*(len(arguments)+1),stop,*arguments));uc.reg_write(UC_X86_REG_ESP,stack+0x1000)
        uc.emu_start(address,stop,count=100000);assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original parser did not return'
        return uc.reg_read(UC_X86_REG_EAX)
    literal=lambda token,n:bytes([token])+struct.pack('<I',n)
    integer=lambda n:literal(0x17,n)
    name=lambda n:literal(0x16,n)
    checks=0
    for seed in (0,1,123,0x7fffffff,0x80000000,0xffffffff):
        for op in (0x2f,0x37):
            for weights in ((1,1),(0,5),(3,1)):
                value=choice(op,weights,[integer(11),integer(22)])
                named=name(700)+b'\x07'+value
                scenarios=[value,named,name(700)+value,named+name(701)+b'\x07'+integer(7),name(702)+value+name(703),choice(op,weights,[name(710),name(711)])]
                scenarios += [name(700)+b'\x07'+choice(op,weights,[integer(11)+b'\x01',integer(22)+b'\x01'])]
                for source in scenarios:
                    data=bytes(source)+b'\x01';uc.mem_write(code,data);emitted.clear();call(0x403da0,[seed]);end=call(0x470f30,[dest,code,0])-code
                    expected=[end,len(emitted)]
                    for m in emitted:expected.extend(m)
                    for i in range(8):expected.append((call(0x403e30 if i%2 else 0x403de0,[2])+1)*11)
                    buffer=ctypes.create_string_buffer(data);out=(ctypes.c_uint32*256)();n=lib.runtime_inline_arguments(buffer,len(data),seed,out,256)
                    assert n>0 and list(out)[:n]==expected,(seed,hex(op),data.hex(),expected,list(out)[:max(0,n)],n)
                    checks+=1
    report={'checks':checks,'mismatches':0,'scope':__doc__.strip()};args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'checks':checks,'mismatches':0}))

if __name__=='__main__':main()
