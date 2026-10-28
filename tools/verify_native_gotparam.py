#!/usr/bin/env python3
"""Compare GotParam with its complete original x86 CFunction and getters.

Synthetic read-only PC parameter structures/global symbols model the same QB
values passed to the portable interpreter. Original GotParam, GetChecksum,
FindComponent and symbol lookup execute without stubs or allocators.
Cycle/limit cases are native unit tests, not executed in unsafe original code.
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


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,default=Path('work/english-game/Game/THUG2PM.exe'))
    ap.add_argument('--library',type=Path,default=Path('work/native-build/libcore-oracle.dylib'))
    ap.add_argument('--report',type=Path,default=Path('work/native-validation/gotparam-oracle.json'))
    args=ap.parse_args();assert hashlib.sha256(args.exe.read_bytes()).hexdigest()==EXE_SHA256
    lib=ctypes.CDLL(str(args.library.resolve()));lib.gotparam_probe.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t];lib.gotparam_probe.restype=ctypes.c_int
    uc=Uc(UC_ARCH_X86,UC_MODE_32);pe=pefile.PE(str(args.exe));uc.mem_map(0x400000,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095)
    for s in pe.sections:uc.mem_write(0x400000+s.VirtualAddress,s.get_data())
    arena,buckets,script,stack,stop=0x2000000,0x3000000,0x4000000,0x10000000,0x11000000
    uc.mem_map(0,0x1000);uc.mem_map(arena,0x100000);uc.mem_map(buckets,0x4000);uc.mem_map(script,0x1000);uc.mem_map(stack,0x2000);uc.mem_map(stop,0x1000)
    def put(p,n):uc.mem_write(p,struct.pack('<I',n&0xffffffff))
    cursor=arena
    def alloc(n):
        nonlocal cursor
        p=cursor;cursor+=(n+15)&~15;uc.mem_write(p,b'\0'*n);return p
    # Values are (type, payload); structures hold canonical component lists.
    integer=lambda n:(1,n&0xffffffff)
    name=lambda n:(13,n)
    structure=lambda fields:(10,fields)
    def pc_value(v):
        kind,data=v
        if kind==10:
            p=alloc(12);last=0
            for field,child in data:
                node=alloc(16);uc.mem_write(node,bytes([child[0]<<1]));put(node+4,field);put(node+8,pc_value(child))
                if last:put(last+12,node)
                else:put(p+4,node)
                last=node
            return p
        return data
    word=lambda n:struct.pack('<I',n&0xffffffff)
    def qb_value(v):
        kind,data=v
        if kind==10:return b'\x03'+b''.join(qb_field(n,c) for n,c in data)+b'\x04'
        return bytes([{1:0x17,2:0x1a,13:0x16}[kind]])+word(data)
    def qb_field(n,v):return (b'\x16'+word(n)+b'\x07' if n else b'')+qb_value(v)
    globals={700:integer(99),800:structure([(0,name(700)),(702,integer(0))]),801:name(800),802:structure([(0,name(801)),(703,integer(0))])}
    global_qb=b''.join(qb_field(n,v)+b'\x01' for n,v in globals.items())+b'\0'
    callers=[[],[(700,integer(0))],[(700,(2,0))],[(0,name(700))],[(0,integer(0))],[(0,name(800))],[(0,name(801))],[(0,name(802))],[(800,integer(0))],[(800,structure([(700,integer(1))]))],[(0,name(999))]]
    callers += [[(n,integer(v))] for n in (0,700,701,702,703,800,801,999) for v in (0,1,0xffffffff)]
    queries=[[],[(0,integer(123))]]+ [[(0,name(n))] for n in (0,700,701,702,703,800,801,802,999)]
    queries += [[(0,name(700)),(0,name(701))],[(123,name(700))],[(0,name(701)),(0,integer(0))]]
    checks=0
    for fields in callers:
        for query in queries:
            cursor=arena;uc.mem_write(buckets,b'\0'*0x4000);heads=[0]*4096
            for n,v in globals.items():
                raw=pc_value(v);entry=alloc(20);slot=n&4095
                uc.mem_write(entry,struct.pack('<BBHIIII',1,v[0],0,n,0,raw,heads[slot]));heads[slot]=entry
            uc.mem_write(buckets,struct.pack('<4096I',*heads));put(0x6f3624,buckets)
            params=pc_value(structure(query));caller=pc_value(structure(fields));put(script+0x14,caller)
            uc.mem_write(stack+0x1000,struct.pack('<III',stop,params,script));uc.reg_write(UC_X86_REG_ESP,stack+0x1000)
            uc.emu_start(0x44bd30,stop,count=20000);assert uc.reg_read(UC_X86_REG_EIP)==stop,'Original GotParam did not return'
            expected=uc.reg_read(UC_X86_REG_EAX)&255
            caller_qb=qb_value(structure(fields))+b'\0';query_qb=b''.join(qb_field(n,v) for n,v in query)
            buffers=[ctypes.create_string_buffer(x) for x in (global_qb,caller_qb,query_qb)]
            actual=lib.gotparam_probe(buffers[0],len(global_qb),buffers[1],len(caller_qb),buffers[2],len(query_qb))
            assert actual==expected,(fields,query,expected,actual);checks+=1
    report={'checks':checks,'mismatches':0,'scope':__doc__.strip()};args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'checks':checks,'mismatches':0}))

if __name__=='__main__':main()
