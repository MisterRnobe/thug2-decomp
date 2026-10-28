# Portable QB values and static expressions

`thug2/values.hpp` gives token and precompiled QB values one immutable, owning
representation: scalar bits, byte strings/local strings, pairs/vectors, typed
arrays, ordered structure members, names and stored script bodies. Names and
integer/float bits remain distinct types. Child references are `shared_ptr` to
native values, never 32-bit game pointers or serialized offsets.

```cpp
#include "thug2/values.hpp"

// After loading the required QB files into a SymbolTable:
thug2::qb::ValueEvaluator evaluator(symbols);
auto value = evaluator.global(thug2::name_crc("BlockingFunctions"));
auto preview = thug2::qb::format_value(value);
```

The evaluator snapshots definitions when constructed. Values are materialized
lazily and cached, with explicit cycle detection and per-request depth/node/
step budgets (`EvalLimits`: 128 / 250,000 / 1,000,000 by default).
Container additions also check their resulting element/member count before
allocation, preventing exponential growth through repeated concatenation.
Build another evaluator after changing the table. Values and the snapshot outlive table
replacement/unload/clear. This evaluates against the **final snapshot**, not the
original engine's sequence of load-time assignments.

`global` follows explicit aliases; a missing root raises `Missing`, while a
dangling alias stays a typed Name of the absent target, preserving the original
lookup fallback. Names inside arrays/structures remain names until an operation
requires resolution. Expressions resolve global names, except member selectors
after a dot. Structure lookup supports included global structures and returns
the last matching member; it remains bounded for cyclic inclusions.

## Parsing and operations

Token literals and containers become values; precompiled values are converted
from the previously validated image tree. Integer/float arrays promote to
float. Token structure insertion follows typed replacement rules: duplicate
name/type entries and numeric replacements are removed, but distinct unnamed
name flags coexist. Precompiled member order is preserved as stored. Script
values retain code bytes without executing them.

Expressions support `+`, `-`, `*`, `/`, `=`, `<`, `>`, `and`, `or`, parentheses,
array indexing, vector/pair dot products and structure-member access. They also
support compiler-generated implicit subtraction (`value` followed by a negative
numeric literal). The operator precedence is read from the supported original
EXE and encoded explicitly:

| Precedence | Operators |
| --- | --- |
| 100 | index, dot, multiply |
| 99 | divide |
| 98 | add, subtract |
| 80 | less-than |
| 78 | greater-than |
| 76 | equals |
| 60 | and |
| 58 | or |

Equal-precedence operators associate left. `]` follows the original parser's
skip behavior rather than introducing C-style grouping: use parentheses for a
compound index. Integer arithmetic wraps at 32 bits; integer division truncates
toward zero. `and/or` combine an integer's actual bits, not normalized booleans;
a nonzero float contributes 1. Both sides are evaluated. Strings compare by
length for `<`/`>` and by bytes for equality; concatenation has the original
199-byte result limit. Division checks integer zero and the original float
threshold `abs(divisor) < 1e-6f`; INT_MIN/-1 becomes a diagnosed error instead
of reproducing an IDIV fault.

Additional operations include checksum-plus-integer/name, string/local-string
addition, pair/vector arithmetic and cross products, homogeneous array addition,
and structure merge/subtraction. Arrays of incompatible types are rejected
rather than copying pointer-shaped data under the wrong type. Array indices
are bounded. Local-string indexing remains unsupported, matching the original
accessor's restriction. Runtime pointer/ownership identity comparisons are
explicitly unsupported for container/script values; immutable sharing is not
treated as original game pointer identity.

`src/extended.hpp` uses binary64 error-free transforms and `std::fma` to carry
low bits and round to a 64-bit significand before binary32 storage. This fixes
a concrete mixed integer/float tie case where ordinary double arithmetic
differs from original x87. It requires normal IEEE round-to-nearest host
arithmetic. It is **not a general x87 emulator**: control flags, exceptions,
other precision modes and NaN payload propagation are not reproduced. The
supported domain is QB binary32/int32 arithmetic. Vector arithmetic uses the
same helper; the differential operation test below currently covers scalars.

Random branches/ranges and engine/member calls inside expressions raise
`Unsupported` instead of inventing results. The bounded [executor](RUNTIME.md)
now supplies caller parameters through explicit read/argument contexts and
dispatches statement-level QB/host calls. Failures are typed as `unsupported`,
`missing`, `cycle`, `limit`, `type`, `arithmetic` or `syntax`. This is a static
value reader/evaluator, not the full original ParseQB/interpreter: temporary
loads, mutable references, ownership-dependent comparisons, original 20-entry
evaluator stacks and specialized AddParameter zero-vector omission are not
emulated. Mixed nonnumeric arrays and unsupported operators fail explicitly.

## CLI

```sh
work/native-build/thug2-symbols value BlockingFunctions \
  --load scripts/aaaaloopchecker.qb --dir work/english-game/Game/Data

work/native-build/thug2-symbols value 0x399ba16f \
  --load models/UI_bg/UI_bg.qb.xbx --dir work/english-game/Game/Data
```

`value` accepts a name or `0xCRC`, printing the type and a bounded item preview.
Strings are escaped bytewise. Evaluation failures print their category and
return 5; other existing command exit codes are unchanged. Script output is a
code-size description, not execution. `format_value` limits displayed nodes
(100 by default), not total string bytes.

## Validation on 2026-10-28

```sh
ctest --test-dir work/native-build --output-on-failure
work/native-build/values-corpus work/english-game/Game/Data work/native-validation/values.json
.venv/bin/python tools/verify_native_values.py --samples 10000
```

All **24,275 current global values** from the 565-module catalog materialize
without diagnostics. They include 15,733 stored script bodies, which are kept
as data and **not executed**. The audit uses the deterministic inspection order
documented in [SYMBOLS.md](SYMBOLS.md), not game startup order. Runtime-context
features remain unsupported even though they do not occur in these global
initializers. This count is coverage, not proof of semantic equality for every
container and operation.

This work also fixed the token indexer to recognize adjacent definitions without
EOL and trailing checksum-name dictionaries, recovering one previously omitted
global. Current counts are 10,351 indexed token globals, 27,573 total definition
events and 24,275 active definitions; the token/frame/script-checksum counts did
not change. The new cases have a regression test, and symbol lookup was checked
again against the original procedures.

The optional scalar oracle runs original `CExpressionEvaluator::Execute`
(`0x0046d450`) with synthetic components, an empty symbol table and x87 control
word `0x037f`. **106,469 operations match**, including 32,620 matching error
outcomes. It checks result type/bits, except NaN payload bits; INT_MIN/-1 is
excluded because the original faults. Inputs include boundary values and
10,000 deterministic random pairs. The regression that exposed double-rounding
is also a unit test. No game allocators/callbacks or engine execution are used.
Only the oracle needs the supported EXE, existing project `.venv` dependencies
and Unicorn outside the macOS execution sandbox.

Unit tests additionally cover precedence, implicit negatives, both file formats,
structure selectors, array promotion/indexing, snapshots and bounded failures.
Release and AddressSanitizer/UndefinedBehaviorSanitizer unit and full-catalog
runs pass; the array-expansion guard also has a dedicated bounded-growth test.
Reports under `work/native-validation/`: `values.json`, `values-sanitize.json`,
`values-oracle.json`, `values-cli.json`. The original game and DLL are unchanged.
