# Runtime arrays and random control flow

Runtime array literals use the original **two-pass** `ParseArray` conventions.
This applies to values parsed with the interpreter's explicit transfer callback;
static global materialization and static value APIs remain deterministic.
Elements are owned immutable native values, without PC pointers or layouts.

The first pass follows random and ordinary jumps to determine capacity and
packed element type. Integer literals retain an already inferred float type;
other literal types replace the inferred type as in the original. Structure
and nested-array elements are skipped lexically by `SkipValue` conventions,
without evaluating their fields or inner random tables. Separators/newlines
are skipped. An empty sizing path returns an empty, untyped array immediately.

For a nonempty path, parsing restarts just after the opening array token. The
second pass follows transfers **again**, sharing the same RNG streams and
stored history. It creates actual elements using the first pass's type. Thus
selected values can differ between passes. Nested arrays each run their own
two passes only when populated; structure fields run during population alone.
The returned PC follows the second path's closing bracket. Stored NoRepeat
and Permute histories remain shared between passes and persist across waits;
array output elements themselves need not satisfy a no-repeat property.

Supported element types are integer, float, checksum name, string, local
string, pair, vector, structure and array. Names in checksum arrays remain
literal names, even if they identify numeric globals. Integer population of a
float array converts through float32, including int32 rounding. Values may
contain runtime control flow within structures and nested arrays. Direct
parameter references and expression-created elements are not literal array
elements and remain unsupported, as does array creation inside expressions.

## Capacity and type boundaries

The original preallocates and zeroes storage based on the sizing pass. A
shorter population therefore leaves zeroes in integer, float and checksum
arrays; the native model preserves those typed zero tails, including the
single inline element case. An empty sizing path performs no population pass
and consumes no second-path random choices.

If population would exceed capacity or has an incompatible type, native parsing
faults before an invalid write/read. The original can overrun its element block
or interpret payload bytes as tokens in these cases. Population shorter than
capacity in pointer-valued arrays is explicitly unsupported: the original
leaves null pointer slots, which the current immutable value API does not
represent. Supported strings/containers are never substituted for those nulls.

Both passes consume value-evaluation steps, and all transfers/random draws
charge lifetime runtime fuel. Capacity is checked against `EvalLimits::nodes`
before reserving storage. Nested population also uses the existing depth and
allocation limits; lexical skips are iterative and bounded by the source.
Jump cycles stop at limits. These checks do not require an engine allocator.

## Validation

All **15 CTest suites** pass in Release and ASan/UBSan builds. Array fixtures
cover separate sizing/population choices, sticky float inference and conversion,
empty sizing paths, numeric/checksum zero tails, strings/local strings,
coordinates, literal global names, nested arrays and structures, ordinary
separators, unsupported inputs, mismatched types/capacity, allocation limits
and transfer cycles. Repeated array construction across Wait checks retained
NoRepeat and Permute histories against the shared selector.

```sh
/path/to/project/.venv/bin/python tools/verify_native_array_random.py \
  --exe /path/to/Game/THUG2PM.exe --report work/array-random-oracle.json
```

The optional x86 oracle compares **246 cases with zero mismatches**: recursive
typed contents, stop PC and eight subsequent stream probes. Original
ParseArray, SkipValue, ParseStructure, random transfers, Rnd/Rnd2 and shuffle
code execute. Heap contexts, array sizing, object/string/component allocation,
stored-record lookup/allocation and scalar AddParameter emission are controlled
stubs. It does not validate original allocators, general component insertion,
cache ownership or full interpreter execution. Unsafe original overflows,
type confusion and null pointer tails are excluded; native fault cases are
covered by unit tests instead. The original EXE is read-only.

The 15,728-entry catalog, seed 0, empty inputs and 500 fuel gives 939 finished,
40 limited; faults: 14,311 missing, 24 type and 414 unsupported. With only Wait
registered and up to four 60 Hz updates: 940 finished, 102 waiting, 40 limited;
faults: 14,201 missing, 24 type and 421 unsupported. These current counts also
include RandomRange and reference-assignment support. Runtime arrays perform both passes, including ordinary literal
arrays, so their work is reflected in the small audit fuel budget. These probes are not complete gameplay validation.
Release and ASan/UBSan Wait-only catalog reports match exactly.

Local reports: `work/array-random-oracle.json`, `array-random-no-host.json`,
`array-random-wait.json` and `array-random-wait-sanitize.json`. Reports, game
assets and EXE are not committed.
