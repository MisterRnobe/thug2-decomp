#!/usr/bin/env python3
"""Compare runtime two-pass arrays with original ParseArray and RNG state.

Original ParseArray/SkipValue/ParseStructure, random transfers and RNG execute.
Heap contexts, array sizing, object/string/component allocation and scalar
AddParameter emission use controlled stubs; no engine allocator runs. Compare
recursive typed contents, stop PC and eight subsequent stream probes.
"""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_FPCW
from verify_native_prx import EXE_SHA256
from verify_native_inline_random import choice


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/array-random-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()));lib.runtime_array_probe.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t];lib.runtime_array_probe.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe));uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for s in pe.sections:uc.mem_write(0x400000+s.VirtualAddress,s.get_data())
    code,dest,manager,heap,stack,stop=0x2000000,0x3000000,0x3001000,0x4000000,0x10000000,0x11000000
    uc.mem_map(0,0x1000);uc.mem_map(code,0x10000);uc.mem_map(dest,0x3000);uc.mem_map(heap,0x100000);uc.mem_map(stack,0x4000);uc.mem_map(stop,0x1000)
    def get(p):return struct.unpack('<I',uc.mem_read(p,4))[0]
    def put(p,n):uc.mem_write(p,struct.pack('<I',n&0xffffffff))
    put(0x7d1800,manager);put(manager+0x124,0);put(0x6f3530,1);uc.reg_write(UC_X86_REG_FPCW,0x037f)
    cursor=heap;stored={};pending=None
    def allocate(size):
        nonlocal cursor
        address=cursor;cursor+=(size+15)&~15;assert cursor<=heap+0x100000
        uc.mem_write(address,b'\0'*size);return address
    def finish(emu,value=0,pop=0):
        sp=emu.reg_read(UC_X86_REG_ESP);ret=get(sp);emu.reg_write(UC_X86_REG_EAX,value);emu.reg_write(UC_X86_REG_ESP,sp+4+pop);emu.reg_write(UC_X86_REG_EIP,ret)
    def push_context(emu,address,size,user):finish(emu,pop=4)
    def pop_context(emu,address,size,user):finish(emu)
    def size_array(emu,address,size,user):
        sp=emu.reg_read(UC_X86_REG_ESP);n,kind=get(sp+4),get(sp+8);array=emu.reg_read(UC_X86_REG_ECX);assert n<=1024
        uc.mem_write(array+1,bytes([kind if n else 0]));put(array+4,n);put(array+8,allocate(n*4) if n>1 else 0);finish(emu,pop=8)
    def new_object(emu,address,size,user):finish(emu,allocate(16))
    def cstring(p):
        data=bytearray()
        for i in range(8192):
            c=uc.mem_read(p+i,1)[0]
            if not c:return bytes(data)
            data.append(c)
        raise AssertionError('Unterminated test string')
    def copy_string(emu,address,size,user):
        source=get(emu.reg_read(UC_X86_REG_ESP)+4);data=cstring(source)+b'\0';target=allocate(len(data));uc.mem_write(target,data);finish(emu,target)
    def add_literal(emu,address,size,user):
        p=emu.reg_read(UC_X86_REG_ECX);name=emu.reg_read(UC_X86_REG_EDX);sp=emu.reg_read(UC_X86_REG_ESP);structure=get(sp+4);token=uc.mem_read(p,1)[0]
        assert token in (0x16,0x17,0x1a),hex(token)
        node=allocate(16);uc.mem_write(node,bytes([{0x16:13,0x17:1,0x1a:2}[token]<<1]));put(node+4,name);put(node+8,get(p+1))
        last=get(structure+4)
        if not last:put(structure+4,node)
        else:
            while get(last+12):last=get(last+12)
            put(last+12,node)
        finish(emu,p+5)
    def find_stored(emu,address,size,user):
        nonlocal pending
        pending=(emu.reg_read(UC_X86_REG_EBX),emu.reg_read(UC_X86_REG_EDI),emu.reg_read(UC_X86_REG_ESI));finish(emu,stored.get(pending,0))
    def new_stored(emu,address,size,user):
        assert pending is not None
        node=allocate(0x60);stored[pending]=node;finish(emu,node)
    hooks=[(0x5bb0e0,push_context),(0x5bb110,pop_context),(0x46cc50,size_array),(0x408f90,new_object),(0x409030,new_object),(0x46d3d0,new_object),(0x46d410,new_object),(0x4763a0,copy_string),(0x470130,add_literal),(0x46f7a0,find_stored),(0x470bb0,new_stored)]
    for address,callback in hooks:uc.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
    def call(address,arguments):
        uc.mem_write(stack+0x2000,struct.pack('<'+'I'*(len(arguments)+1),stop,*arguments));uc.reg_write(UC_X86_REG_ESP,stack+0x2000)
        uc.emu_start(address,stop,count=200000);assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original array helper did not return';return uc.reg_read(UC_X86_REG_EAX)
    def emit(kind,raw,depth=0):
        assert depth<32
        if kind==12:
            size=get(raw+4);element_type=uc.mem_read(raw+1,1)[0];ptr=raw+8 if size==1 else get(raw+8)
            out=[12,element_type,size]
            for i in range(size):out+=emit(element_type,get(ptr+i*4),depth+1)
            return out
        if kind==10:
            nodes=[];p=get(raw+4)
            while p:nodes.append(p);p=get(p+12)
            out=[10,len(nodes)]
            for p in nodes:out += [get(p+4)]+emit(uc.mem_read(p,1)[0]>>1,get(p+8),depth+1)
            return out
        if kind in (3,4):data=cstring(raw);return [kind,len(data),*data]
        if kind in (5,6):return [kind]+[get(raw+4+i*4) for i in range(2 if kind==5 else 3)]
        return [kind,raw] if kind else [0]
    checks=0
    def verify(data,seed):
        nonlocal cursor,checks,pending
        cursor=heap;stored.clear();pending=None;uc.mem_write(dest,b'\0'*12);uc.mem_write(code,bytes(data)+b'\0');call(0x403da0,[seed])
        end=call(0x471b00,[dest,code,0])-code;expected=[end]+emit(12,dest)
        for i in range(8):expected.append((call(0x403e30 if i%2 else 0x403de0,[2])+1)*11)
        buf=ctypes.create_string_buffer(bytes(data));out=(ctypes.c_uint32*4096)();n=lib.runtime_array_probe(buf,len(data),seed,out,4096)
        assert n>0 and list(out)[:n]==expected,(seed,bytes(data).hex(),expected,list(out)[:max(n,0)],n)
        checks+=1
    literal=lambda op,n:bytes([op])+struct.pack('<I',n&0xffffffff)
    integer=lambda n:literal(0x17,n)
    floating=lambda f:b'\x1a'+struct.pack('<f',f)
    name=lambda n:literal(0x16,n)
    text=lambda s,local=False:bytes([0x1c if local else 0x1b])+struct.pack('<I',len(s)+1)+s+b'\0'
    array=lambda data:b'\x05'+data+b'\x06'
    coordinates=lambda op,*values:bytes([op])+struct.pack('<'+'f'*len(values),*values)
    seeds=(0,1,123,0x7fffffff,0x80000000,0xffffffff)
    for seed in seeds:
        for op in (0x2f,0x37,0x40,0x41):
            alternatives=3 if op==0x41 else 2
            for values in ([integer(11),integer(22),integer(33)], [name(11),name(22),name(33)], [floating(1.5),floating(2.5),floating(3.5)], [text(b'a'),text(b'bb'),text(b'ccc')], [text(b'a',True),text(b'b',True),text(b'c',True)], [coordinates(0x1f,1,2),coordinates(0x1f,3,4),coordinates(0x1f,5,6)], [coordinates(0x1e,1,2,3),coordinates(0x1e,4,5,6),coordinates(0x1e,7,8,9)]):
                verify(array(choice(op,[1]*alternatives,values[:alternatives])),seed)
        numbers=choice(0x2f,[1,1],[integer(11),integer(22)])
        verify(array(array(numbers)),seed)
        verify(array(choice(0x2f,[1,1],[array(numbers),array(choice(0x37,[1,1],[integer(33),integer(44)]))])),seed)
        verify(array(b'\x03'+name(50)+b'\x07'+numbers+b'\x04'),seed)
        verify(array(b'\x03'+name(700)+numbers+b'\x04'),seed)
        verify(array(choice(0x2f,[1,1],[integer(16777217),floating(2.5)])+floating(7.5)),seed)
        verify(array(choice(0x2f,[1,1],[b'',integer(11)])),seed)
        verify(array(choice(0x2f,[1,1],[b'',name(11)])),seed)
        verify(array(choice(0x2f,[1,1],[b'',floating(1.5)])),seed)
        verify(array(b'\x01\x09\x02'+struct.pack('<I',17)+numbers+b'\x01'),seed)
        verify(array(b''),seed)
        # Do not execute original overflows: test scalar underfill only when
        # its actual first/second choices select a safe capacity/population.
        call(0x403da0,[seed]);first=call(0x403de0,[2]);second=call(0x403de0,[2])
        if first>=second:
            verify(array(choice(0x2f,[1,1],[integer(11),integer(31)+integer(32)])),seed)
            verify(array(choice(0x2f,[1,1],[name(11),name(31)+name(32)])),seed)
            verify(array(choice(0x2f,[1,1],[floating(1.5),floating(3.5)+floating(4.5)])),seed)
    report={'checks':checks,'mismatches':0,'scope':__doc__.strip(),'excluded':'Original population overflows/type-confusion and null pointer tails'}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'checks':checks,'mismatches':0}))

if __name__=='__main__':main()
