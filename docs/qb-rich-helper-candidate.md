# Original-data rich bridge candidate

Research result for #25 at `e5031e8`. Selected helper: **array_next_wraparound**.
This is a scenario specification, not a successful original-data execution or a
claim of full-module compatibility. No game body, IR or generated game JS is
included. The helper is a candidate blocked by parser work, not a replacement
for the already working scalar ResetLevelFlags scenario.

## Reproduce the inventory

From the repository root, with Node 24+, create an ignored `work/` directory and
run (substitute your installed game's Data directory):

```sh
node tools/qbjs/rich-helper-inventory.ts /path/to/Game/Data work/rich-helper-inventory.json
```

The report must be a **new** `.json` file outside the input directory. Existing
reports are refused, including hardlink/symlink aliases. No input files are
written. Directory symlinks are skipped; traversal stops at depth 16, 100,000
entries, 4,096 QB files or 256 MiB of successfully read input. Each read has the
existing 4 MiB reader limit. Report size is capped at 32 MiB. The input tree is
assumed stable during scanning; this is not a concurrent-filesystem security
boundary. Keep the report private in ignored `work/`.

For each complete supported module the tool records fingerprints, script CRCs
and offsets, call/global/parameter dependencies, literal types, syntax kinds and
RNG use. `supported` means the *entire module* passed reader and emitter, not
that its hosts or engine services are implemented. External calls remain CRCs;
the small `serviceHint` table is only a naming aid. Unparsed modules report
the actual diagnostic and never publish a partial successful script list.
No literal bytes, expression trees, script bodies or emitted code are reported.

The local corpus contained 470 QB files, 20,025,326 bytes; 276 complete modules
passed reader/emitter at this base. These are inventory counts, not counts of
executable modules. Parsed GetArraySize callers in `env_fx.qb` and
`helper_desc.qb` also require screen/object operations, so were not selected.

## Fingerprint and boundary

| Item | Value |
| --- | --- |
| Module relative to Data | `scripts/game/menu/view_goals_menu.qb` |
| Module size | 33,796 bytes |
| SHA-256 | `a23f36949053d5408ce78f3e8cceadecb3ef0361b584be6896ff1df1ee9728e9` |
| Script | `array_next_wraparound` |
| Script CRC | `1897235657` |
| Raw script-definition offset | `156` / `0x9c` |
| Default | Integer `index = 0` |
| Inputs | Owning Array `array`; optional Integer `index` |
| Host calls on selected path | `GotParam`, `GetArraySize` |
| Selected-path globals / RNG / engine objects | None |

For the initial scenario use an owning homogeneous Integer array (type 12,
elementType 1) containing arbitrary bounded test values. It crosses the rich
C++ start boundary, the callback's caller-parameters boundary, the unnamed
GetArraySize argument boundary, and the resulting snapshot boundary. Its
contents and input model must remain unchanged. Passing only its checksum or
precomputing a scalar size in C++ would not exercise the intended bridge.

The selected GetArraySize argument is a plain parameter lookup, not an
expression-mode reference. Scalar arithmetic/comparison affects only `index`.
Do not silently accept a type-20/21 wrapper in place of this owning array.
The selected path needs neither reference handles nor random selection.

## Independent host model and expected results

Use the reconstructed source as evidence; no original-x86 oracle was run:

- [CFunc_GotParam](../src/game/script/script_util_cfuncs.cpp), `0x0044BD30`,
  reads the unnamed checksum from command args and applies
  `script->m_params->ContainsComponentNamed`. For this scenario the checksum is
  `array` (`1592987976`); membership must be read from the **current caller
  parameters**, not the command's argument list or a cached initial input.
  [CStruct::ContainsComponentNamed](../src/game/script/struct.cpp), `0x00476AF0`,
  delegates to `FindComponent`. Implement this bounded local data case; do not
  advertise unresolved global/included-structure behavior as covered.
- [CFunc_GetArraySize](../src/game/script/array_cfuncs.cpp), `0x0044B030`,
  reads the unnamed Array, optionally descends index1/index2/index3, and adds
  Integer `array_size` (`2136685901`) to the calling script. This scenario uses
  **no descent indices**. Check for exactly the supported bounded argument shape
  and type 12, then compute `elements.size()`; return a real Integer output and
  condition true. Wrong kind/missing array must fault explicitly. Reject supplied
  descent indices in this first host adapter rather than ignoring them.
- [CScript::ExecuteIf](../src/game/script/cscript_exec.cpp), `0x00475050`,
  evaluates the command first, then applies optional token-0x39 negation.

For a present nonempty array of length N and starting index i in `[0,N)`, the
independent model is: compute j = i + 1; return index 0 when j equals N,
otherwise return j. GetArraySize writes `array_size = N`. This describes a
single-step wrap; do not replace the body with a general modulo routine or
assume it normalizes arbitrary out-of-range inputs.

| Case | Exact supplied inputs | Expected effects/result |
| --- | --- | --- |
| A: default | `array = [7,8,9]`, omit index | `index=1`, `array_size=3`; one GotParam, one GetArraySize |
| B: last element | `array = [40,50,60]`, `index=2` | `index=0`, `array_size=3`; same call counts |
| C: singleton | `array = [-7]`, `index=0` | `index=0`, `array_size=1`; same call counts |
| D: absent array | no parameters | Guard returns unnamed Integer 0; default `index=0` remains in root state, no `array_size`; one GotParam, zero GetArraySize |
| E: wrong kind | Integer `array=7`, `index=0` | GotParam finds the name; GetArraySize rejects its wrong kind; task faults, no successful snapshot result |

Positive snapshots must be `finished`, retain the original array's values, and
contain named Integer `index` (`2139920638`) and `array_size` as above. The
function's explicit named return merges with the root parameters under existing
typed-merge rules; do not expect the snapshot to contain only the return field.
For D, verify the unnamed Integer-0 result separately from the named default.
For E, faulting is a deliberate safe-port policy; the reconstructed original
dereferences an absent array after logging, so this is not a claimed original
error-path match. Use a fresh Runtime/task fixture per case and bounded fuel.

## Whole-module blockers and integration handoff

The production reader currently rejects the unmodified module at `0xb1`:
`unsupported condition command token`. This is the helper's `if not` prefix
(token 0x39), not a missing native host. A private grammar-only probe that
consumed that prefix reached the next reader error at reported offset `0x36a`,
`unsupported value token 0x42`: another script uses an object-qualified command
(`name:command`). That probe discarded NOT semantics and was **not executed**;
it is not an alternative translator. Further blockers may exist beyond it.

Follow-up #31 tracks these parser dependencies after file handoff from #22.
Preserve NOT evaluation/side effects and qualified-command identity. The unused
qualified scripts must not be rewritten into unqualified calls or success
stubs. Explicit unsupported invocation until an object contract exists is
acceptable; claiming object support from successful module loading is not.
Repeat complete-module reader/emitter validation after each fix, tracking any
additional blockers. Do not extract only this script or skip unrelated
definitions to declare success.

Integration #27 proceeds after #21/#24 and the complete-module syntax gate:

1. Verify the fingerprint; load **all** definitions from the unmodified module
   using the production reader/emitter into the embedded QuickJS Runtime.
2. Bind bounded rich GotParam/GetArraySize adapters using real callback args
   and caller state. Bind no other engine/UI services for this selected path;
   any unexpected call must fault, not return a default success.
3. Run A–E through rich start/update/snapshot and compare exact types, raw bits,
   ordered relevant fields and host-call counts with the independent table.
   Check the original native input model is unchanged and snapshots are detached.
4. Cancel/release all tasks. Exercise limits/error cleanup and ASan/UBSan in the
   new native driver; preserve scalar regressions. Keep game QB/IR/generated
   JS and reports private. Public CI continues to use synthetic fixtures.

Until these gates pass the result is **candidate selected, execution blocked**.
This research does not require #30 live references and does not block the
independent acyclic codec or random-control-flow tasks.

## Tool checks

Run `node --test tools/qbjs/rich-helper-inventory.test.ts` for synthetic metadata,
whole-module rejection, filesystem limits and input preservation tests. To also
execute a generic inventoried synthetic module with the existing embedded
QuickJS test driver:

```sh
QB_QUICKJS_DRIVER=/absolute/path/to/qb-js-script-tests node --test tools/qbjs/rich-helper-inventory.test.ts
```

Without that variable the QuickJS test is explicitly skipped. This optional
check uses JS host callbacks to validate synthetic translation/scheduling; it
does not exercise the future native rich codec and does not run the selected
game helper. Scoped TypeScript and Biome checks apply to the new tools.
