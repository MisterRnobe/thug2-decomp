#!/usr/bin/env python3
# NOT ORIGINAL - optional local x86 differential harness for issue #36 / #23.
"""Verify original x86 ParseArray two-pass behavior against independent model.

Executes unchanged x86 code from local THUG2PM.exe in Unicorn (FPCW 0x037f):
- ParseArray (0x00471B00), SkipValue (0x0046FD60), SkipToken (0x0046F690)
- ResolveRandomJumps (0x00470C60), FindStoredRandom (0x0046F7A0),
  NewStoredRandom (0x00470BB0), CStoredRandom::Shuffle (0x0046F8A0)
- InitialRand (0x00403DA0), Rnd (0x00403DE0), Rnd2 (0x00403E30)
- CArray::SetSizeAndType (0x0046CC50), ParseStructure (0x00471860),
  AddParameter (0x00470130), CStruct::AddComponent (0x00477920), CreateString (0x004763A0)

Only low-level memory allocator entry points (Mem::Pool::Allocate/Free,
Mem::Malloc/FreeMalloc, Mem::Manager::PushContext/PopContext) are replaced with
a bounded arena. Observer-only hooks at CArray::SetSizeAndType and
ResolveRandomJumps record Pass 1 (sizing) vs Pass 2 (population) boundaries
without replacing those functions.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_ECX,
    UC_X86_REG_EIP,
    UC_X86_REG_ESP,
    UC_X86_REG_FPCW,
)

EXE_SHA256 = "7eb982acfa06ee0cb6884c4334d4c049258903ceaa287fd1d2e700a731b91632"

# Original x86 function addresses in THUG2PM.exe
INITIAL_RAND = 0x00403DA0
RND_STREAM0 = 0x00403DE0
RND_STREAM1 = 0x00403E30
SET_SIZE_AND_TYPE = 0x0046CC50
RESOLVE_RANDOM_JUMPS = 0x00470C60
PARSE_ARRAY = 0x00471B00

# Replaced low-level allocator entry points
POOL_ALLOCATE = 0x005BA3D0
POOL_FREE = 0x005BA400
MEM_MALLOC = 0x005BBAE0
MEM_FREE_MALLOC = 0x005BBB60
PUSH_CONTEXT = 0x005BB0E0
POP_CONTEXT = 0x005BB110

# Original global addresses
STREAM1_STATE = 0x00690FC8
STREAM0_STATE = 0x00690FD4
STORED_NEWEST = 0x006F2358
STORED_OLDEST = 0x006F235C
STORED_COUNT = 0x006F2360
CURRENT_SCRIPT = 0x006F3530
MEM_MANAGER_INSTANCE = 0x007D1800

POOL_PAIRS = (
    0x006F2578,  # CStoredRandom pool pair
    0x006F2338,  # CComponent pool pair
    0x006F3614,  # CStruct pool pair
    0x006F22B4,  # CArray pool pair
    0x006F3644,  # CPair pool pair
    0x006F3654,  # CVector pool pair
)

CODE_ADDR = 0x02000000
DEST_ADDR = 0x03000000
MANAGER_ADDR = 0x03001000
POOLS_ADDR = 0x03002000
ARENA_ADDR = 0x04000000
ARENA_SIZE = 0x00100000
STACK_ADDR = 0x10000000
STOP_ADDR = 0x11000000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class ParseArrayOracle:
    def __init__(self, exe_bytes: bytes) -> None:
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        pe = pefile.PE(data=exe_bytes)
        require(
            pe.OPTIONAL_HEADER.ImageBase == 0x400000 and pe.FILE_HEADER.Machine == 0x14C,
            "Expected 32-bit x86 PE image at 0x00400000",
        )
        self.uc.mem_map(0x400000, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
        for section in pe.sections:
            self.uc.mem_write(0x400000 + section.VirtualAddress, section.get_data())

        for address, size in (
            (0, 0x1000),
            (CODE_ADDR, 0x10000),
            (DEST_ADDR, 0x4000),
            (ARENA_ADDR, ARENA_SIZE),
            (STACK_ADDR, 0x4000),
            (STOP_ADDR, 0x1000),
        ):
            self.uc.mem_map(address, size)

        self.uc.reg_write(UC_X86_REG_FPCW, 0x037F)
        self.put(MEM_MANAGER_INSTANCE, MANAGER_ADDR)

        # Configure PoolPair descriptors so PoolPair::Allocate routes to 0x005BA3D0
        # and Pool::Contains (0x005BA3B0) recognizes arena pointers.
        for idx, pair_addr in enumerate(POOL_PAIRS):
            pool_desc = POOLS_ADDR + idx * 0x80
            self.put(pool_desc + 0x00, ARENA_ADDR)
            self.put(pool_desc + 0x04, ARENA_ADDR + ARENA_SIZE)
            self.put(pair_addr + 0x00, 0)
            self.put(pair_addr + 0x08, pool_desc)

        self.cursor = ARENA_ADDR
        self.context_depth = 0
        self.random_visits = 0
        self.pass1_snapshot = None

        for addr in (
            POOL_ALLOCATE,
            POOL_FREE,
            MEM_MALLOC,
            MEM_FREE_MALLOC,
            PUSH_CONTEXT,
            POP_CONTEXT,
            SET_SIZE_AND_TYPE,
            RESOLVE_RANDOM_JUMPS,
        ):
            self.uc.hook_add(UC_HOOK_CODE, self._on_hook, begin=addr, end=addr)

    def put(self, address: int, value: int) -> None:
        self.uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def get(self, address: int) -> int:
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def stream_state(self, stream: int) -> dict[str, int]:
        base = STREAM1_STATE if stream == 1 else STREAM0_STATE
        add, mul, value = struct.unpack("<III", self.uc.mem_read(base, 12))
        return {"add": add, "mul": mul, "value": value}

    def reset_stored_randoms(self) -> None:
        self.put(STORED_NEWEST, 0)
        self.put(STORED_OLDEST, 0)
        self.put(STORED_COUNT, 0)

    def reset_arena(self) -> None:
        self.cursor = ARENA_ADDR
        self.context_depth = 0

    def _alloc(self, size: int) -> int:
        span = max(0x80, (size + 15) & ~15)
        require(self.cursor + span <= ARENA_ADDR + ARENA_SIZE, "Synthetic arena exhausted")
        addr = self.cursor
        self.cursor += span
        self.uc.mem_write(addr, b"\x00" * span)
        return addr

    def _finish_call(self, retval: int = 0, pop_bytes: int = 0) -> None:
        sp = self.uc.reg_read(UC_X86_REG_ESP)
        ret_eip = self.get(sp)
        self.uc.reg_write(UC_X86_REG_EAX, retval & 0xFFFFFFFF)
        self.uc.reg_write(UC_X86_REG_ESP, sp + 4 + pop_bytes)
        self.uc.reg_write(UC_X86_REG_EIP, ret_eip)

    def _on_hook(self, _uc: Uc, address: int, _size: int, _user: object) -> None:
        sp = self.uc.reg_read(UC_X86_REG_ESP)
        # Observer-only hooks: record state and let original x86 instructions run.
        if address == RESOLVE_RANDOM_JUMPS:
            token_ptr = self.get(sp + 4)
            if CODE_ADDR <= token_ptr < CODE_ADDR + 0x10000:
                op = self.uc.mem_read(token_ptr, 1)[0]
                if op in (0x2F, 0x37, 0x40, 0x41):
                    self.random_visits += 1
            return

        if address == SET_SIZE_AND_TYPE:
            this_ptr = self.uc.reg_read(UC_X86_REG_ECX)
            if this_ptr == DEST_ADDR and self.pass1_snapshot is None:
                count_arg = self.get(sp + 4)
                type_arg = self.get(sp + 8)
                self.pass1_snapshot = {
                    "count": count_arg,
                    "elementType": type_arg,
                    "stream0": self.stream_state(0),
                    "stream1": self.stream_state(1),
                }
            return

        # Replacing low-level allocator hooks:
        if address == POOL_ALLOCATE:
            self._finish_call(self._alloc(0x60), pop_bytes=0)
        elif address == POOL_FREE:
            ptr = self.get(sp + 4)
            require(ARENA_ADDR <= ptr < self.cursor, "Invalid pool free pointer")
            self._finish_call(0, pop_bytes=4)
        elif address == MEM_MALLOC:
            req_size = self.get(sp + 4)
            require(req_size <= 0x40000, "Excessive Mem::Malloc request")
            self._finish_call(self._alloc(req_size), pop_bytes=0)
        elif address == MEM_FREE_MALLOC:
            ptr = self.get(sp + 4)
            require(ptr == 0 or ARENA_ADDR <= ptr < self.cursor, "Invalid Mem::FreeMalloc pointer")
            self._finish_call(0, pop_bytes=0)
        elif address == PUSH_CONTEXT:
            self.context_depth += 1
            self._finish_call(0, pop_bytes=4)
        elif address == POP_CONTEXT:
            require(self.context_depth > 0, "Unbalanced Mem::Manager::PopContext")
            self.context_depth -= 1
            self._finish_call(0, pop_bytes=0)

    def call_cdecl(self, func_addr: int, args: list[int], max_instructions: int = 200000) -> int:
        sp = STACK_ADDR + 0x2000
        self.uc.mem_write(sp, struct.pack("<" + "I" * (len(args) + 1), STOP_ADDR, *args))
        self.uc.reg_write(UC_X86_REG_ESP, sp)
        self.uc.emu_start(func_addr, STOP_ADDR, timeout=2_000_000, count=max_instructions)
        require(self.uc.reg_read(UC_X86_REG_EIP) == STOP_ADDR, f"Function 0x{func_addr:08x} did not return within cap")
        require(self.uc.reg_read(UC_X86_REG_ESP) == sp + 4, f"Unexpected stack pointer after 0x{func_addr:08x}")
        return self.uc.reg_read(UC_X86_REG_EAX)

    def read_cstring(self, ptr: int) -> list[int]:
        out: list[int] = []
        for i in range(8192):
            b = self.uc.mem_read(ptr + i, 1)[0]
            if b == 0:
                return out
            out.append(b)
        raise RuntimeError("Unterminated C string in synthetic heap")

    def decode_element(self, kind: int, raw: int, depth: int = 0) -> dict:
        require(depth < 32, "Container decoding depth exceeded")
        if raw == 0 and kind in (3, 4, 5, 6, 10, 12):
            return {"type": 0, "bits": 0}
        if kind in (1, 2, 13):
            return {"type": kind, "bits": raw}
        if kind in (3, 4):
            return {"type": kind, "bytes": self.read_cstring(raw)}
        if kind == 5:
            return {"type": 5, "coordinates": [self.get(raw + 4), self.get(raw + 8)]}
        if kind == 6:
            return {"type": 6, "coordinates": [self.get(raw + 4), self.get(raw + 8), self.get(raw + 12)]}
        if kind == 10:
            members = []
            node = self.get(raw + 4)
            while node != 0:
                comp_type = self.uc.mem_read(node, 1)[0] >> 1
                comp_name = self.get(node + 4)
                comp_raw = self.get(node + 8)
                members.append([comp_name, self.decode_element(comp_type, comp_raw, depth + 1)])
                node = self.get(node + 12)
            return {"type": 10, "members": members}
        if kind == 12:
            return self.decode_array(raw, depth + 1)
        raise RuntimeError(f"Unsupported element type {kind}")

    def decode_array(self, array_ptr: int, depth: int = 0) -> dict:
        elem_type = self.uc.mem_read(array_ptr + 1, 1)[0]
        size = self.get(array_ptr + 4)
        buf_ptr = array_ptr + 8 if size == 1 else self.get(array_ptr + 8)
        elements = []
        for i in range(size):
            raw = self.get(buf_ptr + i * 4)
            elements.append(self.decode_element(elem_type, raw, depth))
        return {"type": 12, "elementType": elem_type, "size": size, "elements": elements}

    def run_parse_array(self, tokens: list[int], owner: int) -> dict:
        self.put(CURRENT_SCRIPT, owner)
        self.uc.mem_write(DEST_ADDR, b"\x00" * 32)
        self.put(DEST_ADDR + 12, 0xDEADBEEF)  # Sentinel right after 12-byte CArray
        self.uc.mem_write(CODE_ADDR, bytes(tokens) + b"\x00")
        self.random_visits = 0
        self.pass1_snapshot = None

        end_ptr = self.call_cdecl(PARSE_ARRAY, [DEST_ADDR, CODE_ADDR, 0])
        require(self.context_depth == 0, "Heap context stack not balanced after ParseArray")
        end_offset = end_ptr - CODE_ADDR

        decoded = self.decode_array(DEST_ADDR)
        s0_after = self.stream_state(0)
        s1_after = self.stream_state(1)

        if self.pass1_snapshot is None:
            # Empty array or type == 0 returned before SetSizeAndType
            pass1 = {
                "count": 0,
                "elementType": 0,
                "stream0": s0_after,
                "stream1": s1_after,
            }
            pass2_ran = False
        else:
            pass1 = self.pass1_snapshot
            pass2_ran = True

        guard_word = self.get(DEST_ADDR + 12)
        return {
            "endOffset": end_offset,
            "pass1": pass1,
            "pass2Ran": pass2_ran,
            "pass2": {
                "elementType": decoded["elementType"],
                "size": decoded["size"],
                "elements": decoded["elements"],
                "stream0": s0_after,
                "stream1": s1_after,
            },
            "guardWordAfterCArray": guard_word,
            "storedRandomCount": self.get(STORED_COUNT),
        }


def run_verification(exe_bytes: bytes, root: Path, quickjs_runner: Path | None) -> dict:
    fixtures_ts = root / "tools/qbjs/array-oracle-fixtures.ts"
    dataset = json.loads(subprocess.check_output(["node", str(fixtures_ts)], encoding="utf-8"))

    oracle = ParseArrayOracle(exe_bytes)
    total_runs = 0
    category_counts: dict[str, int] = {}
    verified_cases = []

    for case in dataset["cases"]:
        case_id = case["id"]
        category = case["category"]
        tokens = case["tokens"]
        category_counts[category] = category_counts.get(category, 0)

        for seq_idx, seed in enumerate(case["seeds"]):
            oracle.reset_arena()
            oracle.reset_stored_randoms()
            oracle.call_cdecl(INITIAL_RAND, [seed])

            expected_seq = case["runs"][seq_idx]
            for step_exp in expected_seq:
                owner = step_exp["owner"]
                actual = oracle.run_parse_array(tokens, owner)
                require(actual["guardWordAfterCArray"] == 0xDEADBEEF, f"Guard word corrupted in {case_id}")

                probes = []
                for i in range(8):
                    fn = RND_STREAM1 if (i % 2 == 1) else RND_STREAM0
                    draw = oracle.call_cdecl(fn, [2])
                    probes.append(((draw + 1) * 11) & 0xFFFFFFFF)

                require(actual["endOffset"] == step_exp["endOffset"], f"{case_id} seed {seed}: endOffset mismatch")
                require(actual["pass1"] == step_exp["pass1"], f"{case_id} seed {seed}: Pass 1 snapshot mismatch")
                require(actual["pass2Ran"] == step_exp["pass2Ran"], f"{case_id} seed {seed}: pass2Ran mismatch")
                for key in ("elementType", "size", "elements", "stream0", "stream1"):
                    require(
                        actual["pass2"][key] == step_exp["pass2"][key],
                        f"{case_id} seed {seed}: Pass 2 {key} mismatch: {actual['pass2'][key]} != {step_exp['pass2'][key]}",
                    )
                require(probes == step_exp["subsequentProbes"], f"{case_id} seed {seed}: subsequent RNG probes mismatch")
                total_runs += 1
                category_counts[category] += 1

        verified_cases.append(
            {
                "id": case_id,
                "category": category,
                "runsVerified": sum(len(seq) for seq in case["runs"]),
            }
        )

    # Probe documented unsafe mismatch cases in bounded Unicorn memory without host UB.
    unsafe_results = []
    for ucase in dataset["unsafeCases"]:
        uid = ucase["id"]
        hazard = ucase["hazard"]
        seed = ucase["seed"]
        tokens = ucase["tokens"]

        oracle.reset_arena()
        oracle.reset_stored_randoms()
        oracle.call_cdecl(INITIAL_RAND, [seed])

        if hazard == "pointer_underfill_null":
            res = oracle.run_parse_array(tokens, owner=1)
            if uid == "scalar_underfill_zero_tail":
                require(res["pass1"]["count"] == 2 and res["pass1"]["elementType"] == 1, f"Unexpected pass1 in {uid}")
                require(
                    res["pass2"]["elements"] == [{"type": 1, "bits": 11}, {"type": 1, "bits": 0}],
                    f"Expected zero-padded tail in {uid}, got {res['pass2']['elements']}",
                )
                require(res["guardWordAfterCArray"] == 0xDEADBEEF, f"Unexpected guard overwrite in {uid}")
                unsafe_results.append(
                    {
                        "id": uid,
                        "hazard": hazard,
                        "observed": "Pass 1 count=2, Pass 2 wrote 1 integer element (11); trailing slot[1] remained 0",
                    }
                )
            else:
                require(res["pass1"]["count"] == 2 and res["pass1"]["elementType"] == 3, f"Unexpected pass1 in {uid}")
                require(
                    res["pass2"]["elements"]
                    == [{"type": 3, "bytes": [111, 110, 101]}, {"type": 0, "bits": 0}],
                    f"Expected null pointer slot in {uid}, got {res['pass2']['elements']}",
                )
                unsafe_results.append(
                    {
                        "id": uid,
                        "hazard": hazard,
                        "observed": "Pass 1 count=2 (String), Pass 2 wrote 1 String ('one'); trailing slot[1] remained NULL pointer (0x00000000)",
                    }
                )
        elif hazard == "population_overfill_single":
            res = oracle.run_parse_array(tokens, owner=1)
            require(res["pass1"]["count"] == 1 and res["pass1"]["elementType"] == 1, f"Unexpected pass1 in {uid}")
            require(res["pass2"]["elements"] == [{"type": 1, "bits": 31}], f"Expected 31 in m_single for {uid}")
            # The 2nd element (32) overwrote DEST_ADDR + 12 past the 12-byte CArray!
            require(
                res["guardWordAfterCArray"] == 32,
                f"Expected out-of-bounds write of 32 to DEST+12 in {uid}, got {res['guardWordAfterCArray']}",
            )
            unsafe_results.append(
                {
                    "id": uid,
                    "hazard": hazard,
                    "observed": "Pass 1 count=1 (in-place m_single), Pass 2 wrote 2 elements (31, 32), overwriting DEST+12 past CArray",
                }
            )
        elif hazard == "population_overfill_heap":
            res = oracle.run_parse_array(tokens, owner=1)
            require(res["pass1"]["count"] == 2 and res["pass1"]["elementType"] == 1, f"Unexpected pass1 in {uid}")
            require(
                res["pass2"]["elements"] == [{"type": 1, "bits": 31}, {"type": 1, "bits": 32}],
                f"Expected [31, 32] in allocated slots for {uid}",
            )
            elem_buf = oracle.get(DEST_ADDR + 8)
            oob_word = oracle.get(elem_buf + 8)
            require(oob_word == 33, f"Expected 3rd element (33) written out-of-bounds at m_elements+8, got {oob_word}")
            unsafe_results.append(
                {
                    "id": uid,
                    "hazard": hazard,
                    "observed": "Pass 1 count=2 (8-byte m_elements buffer), Pass 2 wrote 3 elements (31, 32, 33), writing 33 out-of-bounds at m_elements+8",
                }
            )
        elif hazard == "type_mismatch_desync":
            res = oracle.run_parse_array(tokens, owner=1)
            require(res["pass1"]["count"] == 1 and res["pass1"]["elementType"] == 1, f"Unexpected pass1 in {uid}")
            require(
                res["pass2"]["elements"] == [{"type": 1, "bits": 0}] and res["endOffset"] < len(tokens),
                f"Expected unwritten m_single=0 and premature mid-token stop after type desync in {uid}",
            )
            unsafe_results.append(
                {
                    "id": uid,
                    "hazard": hazard,
                    "observed": "Pass 1 inferred Integer (1), Pass 2 chose Float (0x1a); case ESYMBOLTYPE_INTEGER stepped 1 byte into float payload (0x06), stopped mid-token and left slot unpopulated (0)",
                }
            )

    quickjs_verified = False
    if quickjs_runner is not None:
        numeric_js = (root / "scripting/numeric.js").read_text(encoding="utf-8")
        random_js = (root / "scripting/random.js").read_text(encoding="utf-8")
        values_js = (root / "scripting/values.js").read_text(encoding="utf-8")
        qjs_script = (
            "if (globalThis.qbNumeric === undefined) {\n" + numeric_js + "\n}\n"
            + "if (globalThis.qbRandom === undefined) {\n" + random_js + "\n}\n"
            + "if (globalThis.qbValues === undefined) {\n" + values_js + "\n}\n"
            + "\n(() => {\n"
            + f"const dataset = {json.dumps(dataset['cases'])};\n"
            + """
const rng = qbRandom.create(0);
if (globalThis.qbValues?.apiVersion !== 2) throw Error("Expected qbValues API 2");
let checks = 0;
for (const c of dataset) {
    for (let sIdx = 0; sIdx < c.seeds.length; sIdx++) {
        rng.seed(c.seeds[sIdx]);
        rng.clearStored();
        for (const run of c.runs[sIdx]) {
            const ctx = {
                site: { source: "oracle.qb", script: 1, offset: 0 },
                owner: run.owner,
                symbol: () => undefined,
                charge: () => {},
            };
            const toVal = (el) => {
                if (el.type === 10) return qbValues.structure(el.members.map(([k, v]) => [k, toVal(v)]), ctx);
                if (el.type === 12) return qbValues.array(el.elementType, el.elements.map(toVal), ctx);
                return qbValues.copy(el, "value", ctx);
            };
            const arr = qbValues.array(run.pass2.elementType, run.pass2.elements.map(toVal), ctx);
            if (arr.type !== 12 || arr.elementType !== run.pass2.elementType || arr.elements.length !== run.pass2.size) {
                throw Error("Array init mismatch");
            }
            checks++;
        }
    }
}
if (checks !== """
            + str(total_runs)
            + """) throw Error("QuickJS run count mismatch");
globalThis.qbIntegrationTestResult = { completed: true, checks };
})();
"""
        )
        with tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8", delete=True) as tmp:
            tmp.write(qjs_script)
            tmp.flush()
            subprocess.check_call([str(quickjs_runner), tmp.name], stdout=subprocess.DEVNULL)
        quickjs_verified = True

    return {
        "exeSha256": EXE_SHA256,
        "safeCases": len(verified_cases),
        "safeRunsVerified": total_runs,
        "categoryCounts": category_counts,
        "unsafeDiagnosticsVerified": len(unsafe_results),
        "unsafeDiagnostics": unsafe_results,
        "quickjsVerified": quickjs_verified,
        "executedOriginalFunctions": {
            "ParseArray": "0x00471B00",
            "SkipValue": "0x0046FD60",
            "SkipToken": "0x0046F690",
            "ResolveRandomJumps": "0x00470C60",
            "FindStoredRandom": "0x0046F7A0",
            "NewStoredRandom": "0x00470BB0",
            "CStoredRandom::Shuffle": "0x0046F8A0",
            "InitialRand": "0x00403DA0",
            "Rnd": "0x00403DE0",
            "Rnd2": "0x00403E30",
            "CArray::SetSizeAndType": "0x0046CC50",
            "ParseStructure": "0x00471860",
            "AddParameter": "0x00470130",
            "CStruct::AddComponent": "0x00477920",
            "CreateString": "0x004763A0",
        },
        "replacedAllocatorHooks": {
            "Mem::Pool::Allocate": "0x005BA3D0",
            "Mem::Pool::Free": "0x005BA400",
            "Mem::Malloc": "0x005BBAE0",
            "Mem::FreeMalloc": "0x005BBB60",
            "Mem::Manager::PushContext": "0x005BB0E0",
            "Mem::Manager::PopContext": "0x005BB110",
        },
        "observerOnlyHooks": {
            "CArray::SetSizeAndType": "0x0046CC50",
            "ResolveRandomJumps": "0x00470C60",
        },
    }


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="Path to local THUG2PM.exe")
    parser.add_argument("--quickjs-runner", type=Path, default=None, help="Optional qb-js-script-tests binary")
    parser.add_argument("--report", type=Path, default=None, help="Optional new .json report path in ignored work/")
    args = parser.parse_args()

    require(args.exe.is_file(), f"Missing executable: {args.exe}")
    exe_bytes = args.exe.read_bytes()
    require(hashlib.sha256(exe_bytes).hexdigest() == EXE_SHA256, "Unexpected THUG2PM.exe SHA-256")
    if args.report is not None:
        require(
            args.report.suffix == ".json" and not args.report.exists(),
            "Report path must be a non-existing .json file",
        )

    report = run_verification(exe_bytes, root, args.quickjs_runner)
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("x", encoding="utf-8") as f:
            f.write(json.dumps(report, indent=2) + "\n")

    print(
        f"PASS: {report['safeRunsVerified']} two-pass ParseArray runs across {report['safeCases']} cases "
        f"+ {report['unsafeDiagnosticsVerified']} bounded mismatch diagnostics (0 mismatches)"
    )


if __name__ == "__main__":
    main()
