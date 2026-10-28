#!/usr/bin/env python3
"""Compare ExecuteSwitch destinations with the portable QB executor.

Original scanning, grouped labels, SkipToken, SkipEndOfLines, nested-switch
skipping and ComponentsEqual run in x86. Literal argument parsing and component
allocation/copy/cleanup are controlled stubs. No original engine allocator or
full interpreter runs. The supported EXE is opened read-only.
"""
import argparse
import ctypes
import hashlib
import json
import random
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX, UC_X86_REG_EAX, UC_X86_REG_FPCW
from verify_native_prx import EXE_SHA256


def block(value,cases,fallback=True):
    b=bytearray(b'\x3c'+value+b'\x01');exits=[];previous=None
    def patch(field,target):struct.pack_into('<H',b,field,target-field)
    for literal,body in cases:
        if previous is not None:patch(previous,len(b))
        previous=len(b)+2;b+=b'\x3e\x49\0\0'+literal+b'\x01'+body
        exits.append(len(b)+1);b+=b'\x49\0\0'
    if previous is not None:patch(previous,len(b))
    if fallback:
        b+=b'\x3f\x49\0\0';exits.append(len(b)-2);b+=b'\x16'+struct.pack('<I',103)+b'\x01'
    end=len(b);b+=b'\x3d\x01'
    for field in exits:patch(field,end)
    return b


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/switch-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()))
    lib.runtime_switch_destination.argtypes=[ctypes.c_void_p,ctypes.c_size_t];lib.runtime_switch_destination.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe))
    uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for section in pe.sections:uc.mem_write(0x400000+section.VirtualAddress,section.get_data())
    script,params,component,copy,code,buckets,stack,stop=0x1000000,0x1001000,0x1001100,0x1001200,0x2000000,0x3000000,0x10000000,0x11000000
    uc.mem_map(0,0x1000);uc.mem_map(script,0x3000);uc.mem_map(code,0x10000);uc.mem_map(buckets,0x4000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    def put(p,n):uc.mem_write(p,struct.pack('<I',n))
    def get(p):return struct.unpack('<I',uc.mem_read(p,4))[0]
    put(0x6f3624,buckets);put(0x6f2338,0);put(0x6f2340,component) # Nonzero pool routes to the allocation stub.
    uc.reg_write(UC_X86_REG_FPCW,0x037f)
    def return_from(emu,value=0,pop=0):
        sp=emu.reg_read(UC_X86_REG_ESP);destination=get(sp)
        emu.reg_write(UC_X86_REG_EAX,value);emu.reg_write(UC_X86_REG_ESP,sp+4+pop);emu.reg_write(UC_X86_REG_EIP,destination)
    def clear(emu,address,size,user):put(params+4,0);return_from(emu)
    def allocate(emu,address,size,user):uc.mem_write(copy,b'\0'*16);return_from(emu,copy)
    def copy_component(emu,address,size,user):
        sp=emu.reg_read(UC_X86_REG_ESP);uc.mem_write(get(sp+4),bytes(uc.mem_read(get(sp+8),16)));return_from(emu)
    def cleanup(emu,address,size,user):return_from(emu)
    def parse(emu,address,size,user):
        sp=emu.reg_read(UC_X86_REG_ESP);dest,p=get(sp+4),get(sp+8);first=None
        while uc.mem_read(p,1)[0] not in (0,1,2):
            token=uc.mem_read(p,1)[0]
            if token in (0x16,0x17,0x1a):
                kind={0x16:13,0x17:1,0x1a:2}[token];raw=get(p+1);length=5
            elif token in (0x1b,0x1c):kind=3 if token==0x1b else 4;raw=p+5;length=5+get(p+1)
            elif token in (0x1e,0x1f):kind=6 if token==0x1e else 5;raw=p-3;length=13 if kind==6 else 9 # PC coordinates start after a vtable word.
            else:raise AssertionError(('Unexpected literal stub token',hex(token)))
            if first is None:first=(kind,raw)
            p+=length
        put(dest+4,component if first else 0)
        uc.mem_write(component,b'\0'*16)
        if first:uc.mem_write(component,bytes([first[0]<<1]));put(component+8,first[1])
        return_from(emu,p)
    for addr,callback in [(0x477090,clear),(0x470f30,parse),(0x5ba3d0,allocate),(0x4773e0,copy_component),(0x4772a0,cleanup),(0x44c170,cleanup)]:
        uc.hook_add(UC_HOOK_CODE,callback,begin=addr,end=addr)
    def verify(body):
        # Body has entry defaults newline and endscript, exactly as native code.
        data=b'\x01'+body+b'\x24';uc.mem_write(code,data);uc.mem_write(script,b'\0'*0x200)
        put(script+0xc,code+1);put(script+0x10,params);put(stack+0x1000,stop)
        uc.reg_write(UC_X86_REG_ESP,stack+0x1000);uc.reg_write(UC_X86_REG_ECX,script)
        uc.emu_start(0x473c60,stop,count=20000)
        assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original switch did not return'
        destination=get(script+0xc)-code
        buffer=ctypes.create_string_buffer(data);native=lib.runtime_switch_destination(buffer,len(data))
        assert destination==native,('Destination mismatch',data.hex(),destination,native)
    scalar=lambda token,n:bytes([token])+struct.pack('<I',n)
    integer=lambda n:scalar(0x17,n&0xffffffff)
    floating=lambda n:scalar(0x1a,n)
    name=lambda n:scalar(0x16,n)
    text=lambda s,local=False:bytes([0x1c if local else 0x1b])+struct.pack('<I',len(s)+1)+s+b'\0'
    coordinates=lambda token,*coords:bytes([token])+struct.pack('<'+'f'*len(coords),*coords)
    literals=[integer(n) for n in (0,1,-1,2,0x1000001,0x7fffffff,-0x80000000)]+[floating(n) for n in (0,0x80000000,0x3f800000,0x4b800000,0x7f800000,0xff800000,0x7fc00000)]
    literals += [name(0x12345678),name(0x87654321),text(b'hello'),text(b'world'),text(b'hello',True),coordinates(0x1f,1,2),coordinates(0x1e,1,2,3)]
    pairs=[(a,b) for a in literals for b in literals];rng=random.Random(0x737769746368)
    pairs += [(scalar(rng.choice((0x17,0x1a,0x16)),rng.getrandbits(32)),scalar(rng.choice((0x17,0x1a,0x16)),rng.getrandbits(32))) for _ in range(200)]
    checks=0
    for value,match in pairs:
        verify(block(value,[(match,name(101)+b'\x01')],True));checks+=1
    for value in (integer(1),integer(2),integer(3),integer(4),b''):
        for fallback in (False,True):
            verify(block(value,[(integer(1),b''),(integer(2),b''),(integer(3),name(101)+b'\x01')],fallback));checks+=1
    nested=block(integer(99),[(integer(99),name(199)+b'\x01')]);outer=block(integer(2),[(integer(2),name(102)+b'\x01')])
    verify(outer[:7]+nested+outer[7:]);checks+=1
    report={'checks':checks,'mismatches':0,'scope':__doc__.strip()}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'checks':checks,'mismatches':0}))

if __name__=='__main__':main()
