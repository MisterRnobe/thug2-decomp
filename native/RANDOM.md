# Native QB random jumps

`thug2/random.hpp` implements the original two RNG streams and bounded random
choice histories. The interpreter handles table tokens **at statement dispatch**:
`2f` weighted Random, `37` the second stream, `40` RandomNoRepeat and `41`
RandomPermute. Relative table offsets and jump targets use the existing checked
QB document, without original addresses or pointer layouts.

```cpp
thug2::qb::Interpreter vm(symbols);
vm.seed_random(123); // Both streams; defaults to seed 0 in a fresh interpreter.
auto result = vm.run(thug2::name_crc("MyScript"));
```

CLI accepts `--seed N` (uint32 decimal). A new interpreter/process with the same
seed, definition snapshot and execution order reproduces choices. Seed changes
are rejected during a callback. Reseeding resets the two streams, **not** stored
no-repeat/permutation histories, matching InitialRand. Randomness in the host
engine is not connected to these streams.

## Stream behavior

Both streams start with add `0x0aa2fb3f`, multiplier `0x12b9b0a1` and the supplied
seed. Updates retain uint32 wrap and the original arithmetic right shifts. The
sample is the low 16 bits of the new state; range scaling multiplies modulo
2^32 and then arithmetic-shifts by 16. Consequently very large ranges can give
negative samples. This implementation preserves that behavior rather than
substituting a standard C++ distribution. The `RandomStreams` API exposes both
states for differential testing.

Weighted choices sum signed 16-bit weights, draw once from the selected stream,
then subtract weights until the remainder is negative. This step supports
nonnegative weights with a positive total. If scale overflow makes a draw
negative, entry zero is selected just as in the original, even with zero weight.
Empty tables, all-zero weights and negative weights fault explicitly: the
original can scan past its table on such inputs. There is a 65,535-choice cap.

NoRepeat and Permute ignore table weights and use stream one. NoRepeat's first
visit draws an initial last value, then draws a candidate and retries up to 100
times when equal. After that it permits the repeat; a one-choice table therefore
still completes, consuming 102 draws on its first visit. Uint16 choices outside
the table fault instead of reading outside storage.

Permute initializes the order 0..N-1 and performs N*10 swaps, with two random
draws per swap. It increments the cursor **before** choosing: the first visit
returns the second element of the initial order. At wrap, a new shuffle avoids
putting the previous last element first, and that new first element is selected.
Subsequent complete cycles contain all choices exactly once. The supported
count is 2..66, the original inline order capacity. Single-choice and oversized
permutations are rejected because the original accesses invalid/uninitialized
order slots in these cases.

## Ownership and bounds

The interpreter keeps at most 100 stored records, newest insertion first;
creation at capacity evicts the oldest insertion. Lookups do not change age.
Records use compiled script checksum, token offset, opcode, count and execution
owner as native keys. Separate active tasks have separate histories at the
same site. A task retains its history through calls, loops and waits, including
while its terminal result remains retained. Cancel/destruction releases its
records for reuse by later executions. A synchronous run releases ownership
when it ends. A free compatible record is reused before creating another one,
following the original stored-list lookup policy. Script/offset identity is a
portable policy, not the original code-pointer/cache lifetime model.

All executions share the interpreter's RNG streams, so interleaving tasks
changes their draws. Statement table reads, history searches, order creation,
weight scans and every random draw charge lifetime execution fuel. Random
operations cannot replenish fuel. Initialization faults remove the new record;
RNG draws, existing-history mutations and evictions are not rolled back on a
terminal fault. Construct a fresh interpreter for a clean reproduction.

## Current integration boundary

Random tables/jumps nested inside command arguments, assignments, defaults,
expressions or switch argument lines still fault through the bounded value
parser. This step does not implement inline-value control flow, RandomRange,
random host commands, engine script spawning or cache-dependent owner reuse.
The static value evaluator remains deterministic and does not consume RNG.
Statement jumps can chain through random tables and ordinary jumps; every
step costs fuel, including cycles. Selected branches can call QB, return,
change parameters or yield at Wait using the existing runtime.

## Validation

10 CTest suites pass in Release and ASan/UBSan builds. Random fixtures cover
stream independence, seeds, zero-weight alternatives, no-repeat retries,
permutation cycles/boundaries, task ownership and reuse, pool capacity,
interrupted initialization, malformed tables, jump cycles, repeat loops and
permutation histories across Wait. CLI checks validate seeded reproduction
and reject out-of-range seeds.

```sh
/path/to/project/.venv/bin/python tools/verify_native_random.py \
  --exe /path/to/Game/THUG2PM.exe --report work/random-oracle.json
```

The optional oracle matches **1,800 stream draws and resulting states** plus
**13,800 choice decisions**, with zero mismatches. Original Rnd/Rnd2,
ResolveRandomJumps and CStoredRandom::Shuffle execute unchanged; stored-record
lookup/allocation are controlled stubs for one table/owner. Tests cover six
seeds, both streams, signed/overflowing range scaling, weighted tables and
100 successive choices per stored sequence. This does not validate engine
allocators, ownership lookup, inline-value parsing or the full interpreter.

With seed 0, empty inputs and 500 fuel per entry, the 15,728-entry no-host
catalog gives 937 finished, 10 limited; faults: 14,008 missing, 144 type and
629 unsupported. With only Wait registered and up to four 60 Hz updates per
entry: 938 finished, 85 waiting, 10 limited; faults: 13,915 missing, 144 type,
636 unsupported. Finished counts are unchanged by this step; more entries now
reach a missing engine command. These are bounded catalog probes, not gameplay
validation. Release and ASan/UBSan Wait-only reports match exactly.

Local reports: `work/random-oracle.json`, `random-no-host.json`,
`random-wait.json`, `random-wait-sanitize.json`, `random-cli.json`. Game assets,
original EXE and reports stay out of Git.
