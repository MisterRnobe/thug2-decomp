# Original-x86 evidence for two-pass `ParseArray` construction

Companion to #36 and prerequisite specification for #23, based on phase-2 commit
`837f5e7`. The optional local harness
[`tools/verify_js_array_construction.py`](../tools/verify_js_array_construction.py)
and synthetic specification module
[`tools/qbjs/array-oracle-fixtures.ts`](../tools/qbjs/array-oracle-fixtures.ts)
pass **162 safe two-pass runs across 17 synthetic cases** plus **5 bounded
mismatch diagnostics** against the exact locally supplied `THUG2PM.exe` with
SHA-256 `7eb982acfa06ee0cb6884c4334d4c049258903ceaa287fd1d2e700a731b91632`.
No EXE bytes, game QB files or generated reports are committed.

## What actually executes vs. controlled hooks

Unicorn (`UC_ARCH_X86`, `UC_MODE_32`, x87 `FPCW = 0x037f`) executes the
unchanged original x86 instructions from `THUG2PM.exe` for array sizing, element
allocation sizing, population, nested structure/array parsing, random jump
resolution, stored random history management and dual-stream RNG:

| Function | Address & ABI | Role in `ParseArray` execution |
| --- | --- | --- |
| `Script::ParseArray` | `0x00471B00` (`__cdecl`: `dest, token, args`) | Executes both Pass 1 (sizing/type inference) and Pass 2 (element population) |
| `Script::SkipValue` | `0x0046FD60` (`token` in `EAX`) | Advances past scalar/string/vector/pair tokens and lexically skips nested `{...}` / `[...]` in Pass 1 |
| `Script::SkipToken` | `0x0046F690` (`__cdecl`: `token`) | Used by Pass 1 for separators (`0x01`, `0x02`, `0x09`) and inside `SkipValue` for nested containers |
| `Script::ResolveRandomJumps` | `0x00470C60` (`__cdecl`: `token`) | Resolves `0x2e`, `0x2f`, `0x37`, `0x40`, `0x41`, `0x49` before the first element and after every token in both passes |
| `Script::FindStoredRandom` | `0x0046F7A0` (`table` in `EBX`, `type` in `EDI`, `count` in `ESI`) | Searches the original doubly linked list at `0x006F2358` matching `GetCurrentScript()` (`0x006F3530`) or `m_owner == 0` |
| `Script::NewStoredRandom` | `0x00470BB0` (`__cdecl`) | Allocates from `PoolPair` `0x006F2578`, links into `0x006F2358`/`0x006F235C`, and evicts oldest at `0x006F2360 >= 100` |
| `Script::CStoredRandom::Shuffle` | `0x0046F8A0` (`__thiscall`: `avoidRepeat`) | Executes the `10 * count` swap loop and wrap-around repeat avoidance for `0x41` (`RandomPermute`) |
| `Mth::InitialRand`, `Rnd`, `Rnd2` | `0x00403DA0`, `0x00403DE0`, `0x00403E30` (`__cdecl`) | Seed and advance stream 0 (`0x00690FD4`) and stream 1 (`0x00690FC8`) |
| `Script::CArray::SetSizeAndType` | `0x0046CC50` (`__thiscall`: `size, type`) | Sets `m_type`/`m_size`, keeps `size == 1` in-place at `m_single` (`+8`), or calls `AllocateElements` for `size > 1` |
| `Script::ParseStructure`, `AddParameter` | `0x00471860` (`__cdecl`), `0x00470130` (`ECX, EDX, stack`) | Parses nested structures (`0x03 .. 0x04`) in Pass 2, including bare-flag lookahead `ResolveRandomJumps` |
| `Script::CStruct::AddComponent` & typed `Add*` | `0x00477920`, `0x00477B80`, `0x00477C60`, `0x00477D40`, etc. | Constructs and merges structure components with typed replacement |
| `Script::CreateString`, `NewStruct`, `NewArray`, `NewPair`, `NewVector` | `0x004763A0`, `0x00409030`, `0x00408F90`, `0x0046D3D0`, `0x0046D410` | Allocates and initializes string copies, nested containers, pairs and vectors |

### Replaced low-level allocator hooks and non-replacing observers

No script parser or RNG function is stubbed. Only six low-level memory manager
entry points are intercepted to supply a bounded 1 MiB zeroed arena (`0x04000000`)
without running full engine heap initialization:

1. `0x005BA3D0` (`Mem::Pool::Allocate`, `__thiscall`, 0 stack args): returns a
   zeroed 128-byte slot from the arena. The six `Mem::PoolPair` globals
   (`0x006F2578`, `0x006F2338`, `0x006F3614`, `0x006F22B4`, `0x006F3644`,
   `0x006F3654`) have `pools[0]` pointed to synthetic `Mem::Pool` descriptors
   whose `[m_start, m_end)` range covers the arena so original
   `Mem::Pool::Contains` (`0x005BA3B0`) succeeds when `PoolPair::Free` runs.
2. `0x005BA400` (`Mem::Pool::Free`, `__thiscall`, 1 stack arg): validates that
   the freed pointer belongs to the arena and pops the argument.
3. `0x005BBAE0` (`Mem::Malloc`, `__cdecl`, 1 stack arg): allocates a 16-byte
   aligned block from the arena (used by `AllocateElements` when `size > 1` and
   by `CreateString`). Requests above 256 KiB are rejected.
4. `0x005BBB60` (`Mem::FreeMalloc`, `__cdecl`, 1 stack arg): validates that the
   pointer is null or within the arena.
5. `0x005BB0E0` (`Mem::Manager::PushContext`, `__thiscall`, 1 stack arg) and
   `0x005BB110` (`Mem::Manager::PopContext`, `__thiscall`, 0 stack args): track
   and verify balanced heap-context push/pop depth around `SetSizeAndType`,
   `AllocateElements`, `CreateString` and nested `ParseArray`.

Two **observer-only** hooks (which do not alter registers, stack or control
flow) record internal phase boundaries:

- `0x0046CC50` (`CArray::SetSizeAndType`): when `ECX == DEST_ADDR`, snapshots
  `(count, elementType)` and both RNG stream states (`0x00690FD4` and
  `0x00690FC8`) at the exact boundary between Pass 1 (sizing) and Pass 2
  (population) before original `SetSizeAndType` executes.
- `0x00470C60` (`ResolveRandomJumps`): counts entry visits while original
  `ResolveRandomJumps` executes.

Each native call is bounded by a 200,000-instruction / 2-second cap and verifies
the return `EIP`, `ESP` calling convention, balanced heap context depth and a
sentinel guard word at `DEST_ADDR + 12` (immediately past the 12-byte root
`CArray`).

## Observed two-pass behavior

| Category | Cases & runs | Verified original x86 behavior |
| --- | --- | --- |
| Weighted `Random` (`0x2f`, stream 0) | 7 cases, 42 runs | Pass 1 and Pass 2 both start at `first = token + 1` and independently call `ResolveRandomJumps`. Stream 0 advances during Pass 1 and again during Pass 2; stream 1 is untouched. Verified for `Integer`, `Float` (including `-0.0`), `Name`, `String`, `LocalString`, `Pair` and `Vector`. |
| Weighted `Random2` (`0x37`, stream 1) & interleaving | 1 case, 6 runs | `0x37` advances only stream 1 (`0x00690FC8`), `0x2f` advances only stream 0 (`0x00690FD4`), in exact visit order across Pass 1 followed by Pass 2. |
| Stored `RandomNoRepeat` (`0x40`) | 1 case, 36 runs (6 seeds × 3 repeats × 2 owners) | Pass 1 creates or looks up `CStoredRandom` in `0x006F2358` keyed by `(table, 0x40, count, owner)` and updates `m_last`. Pass 2 hits the **same** stored record and retries while equal to `m_last`, so Pass 2 selects a different branch than Pass 1. Concurrent owners (`1` vs `2`) keep separate records. |
| Stored `RandomPermute` (`0x41`) | 1 case, 36 runs (6 seeds × 3 repeats × 2 owners) | First visit in Pass 1 allocates `CStoredRandom`, runs `Shuffle(false)`, increments `m_index` from `0` to `1` and picks `m_order[1]`. Pass 2 increments `m_index` to `2` and picks `m_order[2]`. The next evaluation's Pass 1 wraps (`++m_index >= m_count`), runs `Shuffle(true)` avoiding the prior last element at index `0`, resets `m_index = 0` and picks `m_order[0]`. |
| Nested array skip-path (`SkipValue`) | 2 cases, 12 runs | When Pass 1 encounters a nested `[` (`0x05`), `SkipValue` (`0x0046FD60`) scans tokens via `SkipToken` (`0x0046F690`) until the matching `]` (`0x06`) **without calling `ResolveRandomJumps` inside the nested array**. An inner random table inside a nested array therefore consumes **0 draws** during outer Pass 1, and **2 draws** (inner Pass 1 + inner Pass 2) when outer Pass 2 recursively calls `ParseArray`. If an outer random table chooses between two nested arrays, outer Pass 1 draws once from the outer table (skipping the chosen nested array's body), and outer Pass 2 draws once from the outer table and then runs two passes of the nested array chosen in Pass 2. |
| Nested structure skip-path (`SkipValue` vs `ParseStructure`) | 1 case, 6 runs | Outer Pass 1 skips `{ ... }` (`0x03 .. 0x04`) lexically via `SkipValue` with **0 inner RNG draws**. Outer Pass 2 calls `ParseStructure` (`0x00471860`), which evaluates the structure in a **single pass** (1 draw per random field value, plus 1 extra `ResolveRandomJumps` lookahead after a bare `0x16` flag name when checking whether `0x07` `=` follows). |
| Numeric promotion (`Integer` + `Float` → `Float`) | 2 cases, 12 runs | In Pass 1, `0x17`/`0x18` sets `type = 1` only when `type != 2`, while `0x1a` sets `type = 2` (sticky). In Pass 2, when `type == 2`, `0x17`/`0x18` tokens are converted from `int32_t` to `float32` via x87 (`16777217` → `16777216.0f` = `0x4B800000`). |
| Multi-element branches & separators | 1 case, 6 runs | Branches emitting multiple elements (e.g. two integers separated by `0x01`/`0x02`/`0x09`) increment `count` for each element in Pass 1 and populate each element in Pass 2. |
| Empty arrays & empty random branches | 1 case, 6 runs | If `ResolveRandomJumps(first)` immediately reaches `0x06` (`[]`), or if Pass 1 selects only empty branches (`[ Random(@ @ ) ]`) leaving `type == 0`, `ParseArray` returns `p + 1` immediately after Pass 1 **without calling `SetSizeAndType` and without running Pass 2**. |

All 162 runs match the independent reference model in
[`tools/qbjs/array-oracle-fixtures.ts`](../tools/qbjs/array-oracle-fixtures.ts)
on:

1. `endOffset` (returned token pointer minus `CODE_ADDR`),
2. Pass 1 `(count, elementType, stream0, stream1)` at the `SetSizeAndType` boundary,
3. Pass 2 `(elementType, size, elements, stream0, stream1)` after `ParseArray` returns,
4. Eight subsequent alternating stream 0 / stream 1 probes.

## Bounded mismatch diagnostics (when Pass 1 and Pass 2 disagree on shape)

Because Pass 1 and Pass 2 draw from the RNG independently, a random table whose
branches have different element counts or incompatible element types can diverge
between Pass 1 and Pass 2. The harness exercises five such cases inside bounded
Unicorn memory without triggering host process UB:

| Diagnostic ID | Pass 1 choice | Pass 2 choice | Observed original x86 behavior in Unicorn |
| --- | --- | --- | --- |
| `scalar_underfill_zero_tail` | 2 `Integer` elements (`count = 2, type = 1`) | 1 `Integer` element (`11`) | `SetSizeAndType(2, 1)` zeroes both slots; Pass 2 writes `11` to slot `0` and leaves slot `1` as `0`. |
| `string_pointer_underfill_null_slot` | 2 `String` elements (`count = 2, type = 3`) | 1 `String` element (`"one"`) | `SetSizeAndType(2, 3)` zeroes both pointer slots; Pass 2 writes `"one"` to slot `0` and leaves slot `1` as a raw `NULL` pointer (`0x00000000`). |
| `single_to_multi_overfill_corrupts_past_carray` | 1 `Integer` element (`count = 1, type = 1`) | 2 `Integer` elements (`31, 32`) | For `count == 1`, `CArray` stores its sole element in-place at `dest + 8` (`m_single`) without allocating `m_elements`. Pass 2 writes `31` to `dest + 8` and **writes `32` out-of-bounds to `dest + 12`**, overwriting the guard word past `sizeof(CArray) == 12`. |
| `heap_buffer_overfill_corrupts_past_elements` | 2 `Integer` elements (`count = 2, type = 1`) | 3 `Integer` elements (`31, 32, 33`) | `SetSizeAndType(2, 1)` allocates an 8-byte `m_elements` block. Pass 2 writes `31` and `32` in bounds and **writes `33` out-of-bounds to `m_elements + 8`**. |
| `type_mismatch_pass1_int_pass2_float_desync` | 1 `Integer` element (`count = 1, type = 1`) | 1 `Float` element (`0x1a`, payload `0x3f060606`) | In Pass 2, `case ESYMBOLTYPE_INTEGER` executes `++p` (advancing 1 byte instead of 5) and ignores `t == 0x1a`. The loop then reads the first byte of the float payload (`0x06`), mistakes it for closing `]`, and returns 4 bytes early with slot `0` left unpopulated (`0`). |

## Conformance specification for #23

When implementing two-pass random array construction in #23:

1. **Preserve two distinct passes over the array construction plan**:
   - **Pass 1 (Sizing & Type Inference)**:
     - Evaluate top-level `ResolveRandomJumps` branches in source order using
       the shared `qb.random().choose(op, site, weights, ctx.owner, ctx.charge)`.
     - Apply the original type inference rule: `Integer` sets `type = 1` only if
       `type !== 2`; `Float` sets `type = 2` (sticky); other element tokens
       overwrite `type`.
     - **Do not recursively evaluate nested arrays or structures in Pass 1**:
       a nested array element contributes `+1` to `count` and `type = 12` while
       skipping its internal tokens (`0` inner RNG draws in outer Pass 1); a
       nested structure element contributes `+1` to `count` and `type = 10`
       while skipping its internal tokens (`0` inner RNG draws in outer Pass 1).
     - If Pass 1 finishes with `count === 0` or `type === 0`, return the empty
       array `{ type: 12, elementType: 0, elements: [] }` immediately **without
       running Pass 2**.
   - **Pass 2 (Population)**:
     - Start again from the beginning of the array plan and invoke
       `qb.random().choose(...)` with the updated RNG and stored-random state
       left by Pass 1.
     - For `type === 2` (`Float`), promote any `Integer` element selected in
       Pass 2 to float32 bits (`Math.fround(bits | 0)`).
     - For `type === 12` (`Array`), recursively run the two-pass builder for
       each selected nested array element.
     - For `type === 10` (`Structure`), evaluate the selected structure's fields
       once in Pass 2.
2. **Reject unsafe cross-pass count/type mismatches and static heterogeneities**:
   - In the reader/validator, continue rejecting non-numeric mixed element types
     and unsupported tokens (`RandomRange` `0x30`/`0x38`, expressions `0x0e`,
     parameter reads `0x2d` directly inside array literals, since original
     `ParseArray` only handles `0x03, 0x05, 0x16, 0x17, 0x18, 0x1a, 0x1b, 0x1c,
     0x1e, 0x1f` and loops infinitely on `0x30`/`0x0e`/`0x2d`).
   - At runtime (or statically if branch shapes differ), if Pass 2 produces a
     different element count than Pass 1 (`populatedCount !== count`) or selects
     an element whose type is incompatible with Pass 1's `elementType`, fail
     explicitly with a bounded error carrying the `Site` rather than reproducing
     out-of-bounds writes, null pointer slots or token-stream desynchronization.
3. **Owner 0 (module-load) policy**:
   - In original x86 code, `GetCurrentScript()` (`0x006F3530`) is `0` outside
     script execution, so `FindStoredRandom` matches `m_owner == 0` and reuses
     stored records globally. Per the project contract, `0x40`/`0x41` with
     `owner === 0` (module-load globals/defaults) must continue to reject
     explicitly until load-time stored-random policy is separately specified;
     weighted `0x2f`/`0x37` arrays in globals execute both passes at load time.

## Exclusions and boundaries

- This oracle validates `ParseArray` (`0x00471B00`) and its direct callees in
  isolation; it does not run `CScript::Update` or the full game loop.
- `ParseStructure` inside `ParseArray` is tested on literal/random fields and
  bare flags (`args = nullptr`); caller parameter reads (`0x2d`) and expression
  fields (`0x0e`) inside structures within arrays are covered by existing
  structure/expression contracts rather than this harness.
- Symbol-type codes `7, 8, 9, 11` in `ParseArray` (which cannot be produced by
  Pass 1's switch) and unallocated game pool exhaustion are outside scope.

## Reproduce locally

Use the project Python venv (`/Users/nikita-medvdv/IdeaProjects/thug2-recomp/bins/.venv`)
and local `THUG2PM.exe`:

```sh
mkdir -p work
/Users/nikita-medvdv/IdeaProjects/thug2-recomp/bins/.venv/bin/python \
  tools/verify_js_array_construction.py \
  --exe ../thug2-recomp/bins/work/english-game/Game/THUG2PM.exe \
  --quickjs-runner work/qb-js-release/qb-js-script-tests \
  --report work/array-oracle-report.json
```

The `--report` file is optional, must not already exist, and stays in ignored
`work/`.
