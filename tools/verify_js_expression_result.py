#!/usr/bin/env python3
# NOT ORIGINAL - optional, local x86 evidence. No EXE bytes or reports in git.
"""Exercise original GetResult/CopyComponent on synthetic acyclic containers.

Requires pefile/unicorn in a project venv and the exact locally supplied EXE.
Original evaluator, copy, container initialization and cleanup code execute.
Only object pool allocation/free entry points are replaced with a bounded arena;
no game initialization, QB parser, JS emitter or complete VM is exercised.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

EXE_SHA256 = "7eb982acfa06ee0cb6884c4334d4c049258903ceaa287fd1d2e700a731b91632"
GET_RESULT, COPY, CLEANUP = 0x46EC40, 0x4773E0, 0x4772A0
EVALUATOR, SOURCE, DEST, ARENA, STACK, STOP = 0x1000000, 0x1001000, 0x1001100, 0x2000000, 0x10000000, 0x11000000


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Oracle:
    def __init__(self, exe):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        pe = pefile.PE(data=exe)
        require(pe.OPTIONAL_HEADER.ImageBase == 0x400000 and pe.FILE_HEADER.Machine == 0x14C, "Expected x86 image")
        self.uc.mem_map(0x400000, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
        for section in pe.sections:
            self.uc.mem_write(0x400000 + section.VirtualAddress, section.get_data())
        for address, size in [(0, 0x1000), (EVALUATOR, 0x2000), (ARENA, 0x10000), (STACK, 0x2000), (STOP, 0x1000)]:
            self.uc.mem_map(address, size)
        self.next = ARENA
        self.objects = {}
        self.freed = set()
        self.copies = []
        self.cleanups = []
        self.allocations = []
        # AppendStructure's inlined component pool selection, not a game pool.
        self.put(0x6F2338, 0)
        self.put(0x6F2340, ARENA + 0xFF00)
        for address in [GET_RESULT, COPY, CLEANUP, 0x408F90, 0x409030, 0x408FD0, 0x409070, 0x5BA3D0]:
            self.uc.hook_add(UC_HOOK_CODE, self.hook, begin=address, end=address)

    def put(self, address, value):
        self.uc.mem_write(address, struct.pack("<I", value))

    def get(self, address):
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def allocate(self, kind):
        require(self.next < ARENA + 0xF000, "Synthetic arena exhausted")
        address = self.next
        self.next += 0x40
        self.objects[address] = kind
        self.uc.mem_write(address, bytes(0x40))
        return address

    def component(self, address, kind, raw, name=0):
        self.uc.mem_write(address, struct.pack("<B3xIII", kind << 1, name, raw, 0))

    def read_component(self, address):
        return self.uc.mem_read(address, 1)[0] >> 1, self.get(address + 8)

    def array(self, kind=1, payload=7):
        address = self.allocate("array")
        self.uc.mem_write(address, struct.pack("<BBHII", 0, kind, 0, 1, payload))
        return address

    def structure(self, child=None):
        address = self.allocate("structure")
        if child:
            component = self.allocate("component")
            self.component(component, *child, name=0x12345678)
            self.put(address + 4, component)
        return address

    def finish_allocator(self, value):
        sp = self.uc.reg_read(UC_X86_REG_ESP)
        self.uc.reg_write(UC_X86_REG_EAX, value)
        self.uc.reg_write(UC_X86_REG_EIP, self.get(sp))
        self.uc.reg_write(UC_X86_REG_ESP, sp + 4)

    def hook(self, _uc, address, _size, _user):
        sp = self.uc.reg_read(UC_X86_REG_ESP)
        if address == COPY:
            source = self.get(sp + 8)
            self.copies.append({"sourceType": self.read_component(source)[0], "reference": self.get(sp + 12)})
        elif address == CLEANUP:
            self.cleanups.append(self.read_component(self.get(sp + 4))[0])
        elif address in (0x408F90, 0x409030, 0x5BA3D0):
            kind = {0x408F90: "array", 0x409030: "structure", 0x5BA3D0: "component"}[address]
            if address != 0x5BA3D0:
                require(self.get(sp + 4) == (12 if kind == "array" else 8), "Unexpected allocation size")
            self.allocations.append(kind)
            self.finish_allocator(self.allocate(kind))
        elif address in (0x408FD0, 0x409070):
            pointer = self.get(sp + 4)
            kind = "array" if address == 0x408FD0 else "structure"
            require(self.objects.get(pointer) == kind and pointer not in self.freed, "Invalid/double synthetic free")
            self.freed.add(pointer)
            self.finish_allocator(0)

    def call(self, address, arguments, this=0, pop=0):
        sp = STACK + 0x1000
        self.uc.mem_write(sp, struct.pack("<" + "I" * (len(arguments) + 1), STOP, *arguments))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.reg_write(UC_X86_REG_ECX, this)
        self.uc.emu_start(address, STOP, timeout=1_000_000, count=50000)
        require(self.uc.reg_read(UC_X86_REG_EIP) == STOP, "Original function did not return within limits")
        require(self.uc.reg_read(UC_X86_REG_ESP) == sp + 4 + pop, "Unexpected calling convention")
        return self.uc.reg_read(UC_X86_REG_EAX)

    def result(self, kind, raw):
        self.uc.mem_write(EVALUATOR, bytes(0x200))
        self.component(EVALUATOR, kind, raw)
        require(self.call(GET_RESULT, [DEST], EVALUATOR, pop=4) & 255 == 1, "GetResult failed")
        require(self.read_component(EVALUATOR) == (0, 0), "Scratch component not cleared")
        require(self.copies[0] == {"sourceType": {20: 10, 21: 12}.get(kind, kind), "reference": 0},
                "Wrong result-copy request")
        require(self.cleanups[-1] == kind, "Reference tag not restored before scratch cleanup")
        return self.read_component(DEST)


def verify(exe):
    cases = []
    for kind, raw in [(1, 0xFFFFFFFF), (2, 0x80000000), (13, 0x12345678)]:
        oracle = Oracle(exe)
        require(oracle.result(kind, raw) == (kind, raw), "Scalar payload changed")
        cases.append({"case": "scalar", "type": kind, "bits": raw})
    for kind in (12, 21):
        oracle = Oracle(exe)
        original = oracle.array()
        out_type, copy = oracle.result(kind, original)
        require(out_type == 12 and copy != original, "GetResult did not detach array")
        require(oracle.get(copy + 4) == 1 and oracle.get(copy + 8) == 7, "Array copy contents differ")
        if kind == 21:
            oracle.put(original + 8, 9)
            require(oracle.get(copy + 8) == 7, "Array copy aliases original")
        require((original in oracle.freed) == (kind == 12), "Wrong scratch ownership cleanup")
        cases.append({"case": "array result", "inputType": kind, "outputType": out_type,
                      "detached": True, "originalFreed": kind == 12})
    for kind in (10, 20):
        oracle = Oracle(exe)
        original = oracle.structure()
        out_type, copy = oracle.result(kind, original)
        require(out_type == 10 and copy != original and oracle.get(copy + 4) == 0, "Empty structure not detached")
        require((original in oracle.freed) == (kind == 10), "Wrong structure ownership cleanup")
        cases.append({"case": "empty structure result", "inputType": kind, "detached": True})
    for kind in (20, 21):
        oracle = Oracle(exe)
        target = oracle.structure() if kind == 20 else oracle.array()
        oracle.component(SOURCE, kind, target)
        oracle.call(COPY, [DEST, SOURCE, 0])
        require(oracle.read_component(DEST) == (kind, target), "Ordinary ref copy lost target")
        require(not oracle.allocations, "Ordinary ref copy unexpectedly allocated")
        cases.append({"case": "ordinary reference copy", "type": kind, "sameTarget": True})
    for kind in (10, 12):
        oracle = Oracle(exe)
        target = oracle.structure() if kind == 10 else oracle.array()
        oracle.component(SOURCE, kind, target)
        oracle.call(COPY, [DEST, SOURCE, 1])
        require(oracle.read_component(DEST) == ({10: 20, 12: 21}[kind], target), "Borrow copy lost target")
        require(not oracle.allocations, "Borrow copy unexpectedly allocated")
        cases.append({"case": "owning component borrowed copy", "type": kind, "sameTarget": True})
    for nested_type in (20, 21):
        oracle = Oracle(exe)
        nested = oracle.structure((1, 7)) if nested_type == 20 else oracle.array()
        original = oracle.structure((nested_type, nested))
        out_type, copy = oracle.result(20, original)
        child = oracle.get(copy + 4)
        require(out_type == 10 and copy != original, "Outer referenced structure not detached")
        require(child != oracle.get(original + 4), "Outer structure shares its owned component node")
        require(oracle.read_component(child) == (nested_type, nested), "Nested reference was detached")
        payload_address = oracle.get(nested + 4) + 8 if nested_type == 20 else nested + 8
        oracle.put(payload_address, 9)
        require(oracle.read_component(child)[1] == nested and oracle.get(payload_address) == 9,
                "Nested alias lost mutation visibility")
        cases.append({"case": "nested reference result", "childType": nested_type,
                      "outerDetached": True, "childSameTarget": True})
    oracle = Oracle(exe)
    inner = oracle.array()
    outer = oracle.array(12, inner)
    out_type, copy = oracle.result(21, outer)
    inner_copy = oracle.get(copy + 8)
    require(out_type == 12 and copy != outer and inner_copy != inner, "Nested owning array not detached")
    oracle.put(inner + 8, 9)
    require(oracle.get(inner_copy + 8) == 7, "Nested owning array aliases original")
    cases.append({"case": "nested owning array result", "outerDetached": True, "childDetached": True})
    oracle = Oracle(exe)
    original = oracle.structure((1, 7))
    out_type, copy = oracle.result(20, original)
    original_child, copied_child = oracle.get(original + 4), oracle.get(copy + 4)
    require(out_type == 10 and copied_child != original_child, "Structure's owned child not copied")
    oracle.put(original_child + 8, 9)
    late = oracle.allocate("component")
    oracle.component(late, 1, 77, name=0x87654321)
    oracle.put(original_child + 12, late)
    require(oracle.get(copied_child + 8) == 7 and oracle.get(copied_child + 12) == 0,
            "Completed structure observes later field changes")
    cases.append({"case": "completed structure versus later fields", "retainsOldValue": True,
                  "observesLateField": False})
    return {"exeSha256": EXE_SHA256, "cases": cases, "passed": len(cases),
            "scope": "Single ready evaluator value; singleton arrays and structures; allocator hooks only",
            "excluded": "QB parsing, pending operators, full JS pipeline, game allocation failures and lifetime/GC"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    exe = args.exe.read_bytes()
    require(hashlib.sha256(exe).hexdigest() == EXE_SHA256, "Unexpected EXE SHA-256")
    if args.report:
        require(args.report.suffix == ".json" and not args.report.exists(), "Report must be a new private .json file")
    report = verify(exe)
    if args.report:
        with args.report.open("x", encoding="utf-8") as output:
            output.write(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {report['passed']} original-x86 expression-result cases (bounded synthetic allocator)")


if __name__ == "__main__":
    main()
