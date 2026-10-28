# Shared QB symbol table

`thug2/symbols.hpp` combines token QB globals/functions and precompiled image
symbols into a portable table. It owns parsed modules through immutable shared
references and resolves global name aliases by CRC. It neither registers game
C functions nor evaluates expressions or executes scripts.

```cpp
#include "thug2/symbols.hpp"

thug2::AssetStore assets;
assets.mount_archive("Game/Data/pre/qb_scripts.prx");
assets.mount_directory("Game/Data");
thug2::qb::SymbolTable symbols;
auto module = symbols.load(assets, "scripts/aaaaloopchecker.qb");
auto result = symbols.resolve(thug2::name_crc("BlockingFunctions"));
// result.status, path, target and last_alias
symbols.unload(module);
```

`load_bytes(logical_name, bytes)` supports supplied buffers. `load_batch(assets,
names)` parses all files and constructs candidate maps before publishing any
changes; bad/missing input leaves the table untouched. IDs are stable and not
reused after unload/clear. Modules retain the actual `AssetInfo` source when
loaded through `AssetStore`. `find` returns an owning definition snapshot;
`resolve` returns an owning target snapshot on success. Snapshots and their
bytes remain valid after replacement, unload, table clear or destruction.
Mutation must be externally synchronized with readers.

## Order and ownership

File-source priority and symbol-definition priority are distinct:

- AssetStore selects the first mounted source containing a file.
- QB modules are processed in the caller's load order, and definitions within
  each file in byte-offset order. A later definition replaces the current one.
- As in original `ParseQB`, a **token-script** definition with the same name
  and checksum as the current script reuses that script and its original file
  ownership. This compares checksums, not byte-by-byte code equality. Image
  definitions replace normally, including image scripts with unchanged code.
- Unload removes only current definitions owned by that module. Overwritten
  definitions do not reappear. Unloading an older module cannot remove its
  replacement. Reused unchanged scripts are still owned by the original module.

This models permanent-load replacement/reuse, not temporary loads, pending
symbol replacement, interpreter restart or cache invalidation. Module IDs
distinguish repeated loads of the same logical filename; they are not the game's
filename-CRC ownership key. Runtime callers must explicitly manage those loads.

Token definitions remain source ranges (`DefinitionStorage::TokenValue`) or
script indices; image definitions index `PrecompiledImage::symbols()`. Embedded
functions in structures are values, **not global function definitions**. Token
value types are inferred hints, with `None` for unevaluated expressions. Values
can now be materialized by the separate [value evaluator](VALUES.md).

## Alias lookup

Only a whole-token global value `A = B` becomes a token alias; an expression
beginning with B does not. Precompiled global `Name` values are aliases directly.
Name-valued structure/array members are retained as data and are not global
aliases. Aliases follow the current table, so later replacements affect lookup.

`resolve(crc, max_alias_hops=256)` reports:

| Status | Meaning |
| --- | --- |
| Resolved | A non-alias definition was reached; `target` owns it. |
| Missing | The root or an alias target is absent; no successful target. |
| Cycle | A CRC was encountered again; the trace includes that repeated endpoint. |
| Limit | Following another alias would exceed the configured edge limit. |

The trace includes the requested CRC and the missing/repeated/limited endpoint.
A zero hop limit still permits a direct non-alias lookup. Original
`LookUpSymbol` returns the **last existing alias** when its target is missing,
rather than null; `last_alias` preserves that fallback while `Missing` makes the
diagnostic explicit. Original cycles can loop forever; this API is bounded.
An absent target is not a module-load failure: it can be defined by another
module, an engine registration, or a later loading phase.

Checksum-name records provide escaped display labels across loaded token files,
also usable for image symbols. The latest loaded module's first spelling wins;
unloading its labels does not restore older spellings. This scoped diagnostic
dictionary differs from the game's permanent debug-name cache and has no effect
on CRC lookup or alias resolution.

## CLI

```sh
work/native-build/thug2-symbols list \
  --load scripts/aaaaloopchecker.qb --dir work/english-game/Game/Data

work/native-build/thug2-symbols resolve BlockingFunctions \
  --load scripts/aaaaloopchecker.qb --dir work/english-game/Game/Data

work/native-build/thug2-symbols audit \
  --load scripts/aaaaloopchecker.qb \
  --load models/UI_bg/UI_bg.qb.xbx --dir work/english-game/Game/Data
```

Repeat `--load` in the required module order. Repeat `--prx`/`--dir` in source
priority order. All sources are mounted before files are loaded. `resolve`
accepts a name or `0xCRC`, printing the chain and each definition's owner/type/
offset. Exit codes: 0 resolved/success, 1 error, 2 missing, 3 cycle, 4 hop limit.
`audit` summarizes alias statuses and succeeds even with missing references.

## Validation on 2026-10-28

```sh
ctest --test-dir work/native-build --output-on-failure
work/native-build/symbols-corpus work/english-game/Game/Data \
  work/native-validation/symbols.json work/native-validation/symbols-topology.json
.venv/bin/python tools/verify_native_symbols.py
```

The catalog test mounts 115 sorted archives, then Data; modules are loaded in
normalized pathname order. **This is an inspection order, not game startup.**
It loads 565 modules (470 token, 95 image), with 27,573 definition events:
24,275 active definitions, 3,043 replacements and 255 unchanged script reuses.
Of 187 active aliases, 149 resolve and 38 have absent targets; no cycles or hop
limits occur in this catalog. Missing references are not evidence of broken
game data. An independent source-order model checks winners, ownership, aliases,
traces and unload behavior. Unit tests cover mixed formats, missing targets,
cycles, limits, snapshots, expressions, embedded functions and atomic batches.
Release and AddressSanitizer/UndefinedBehaviorSanitizer unit and full-catalog
runs pass.

The optional oracle builds a synthetic PC-format table from the exported native
topology and calls original `FindSymbol` (`0x00478cc0`) and `LookUpSymbol`
(`0x00478cf0`) in Unicorn. All 24,275 present-name lookups match, including all
38 dangling-alias fallbacks; an absent root returns null. This validates lookup
**routing**, not expression evaluation, actual game table loading or startup
order. Original cycle execution is deliberately skipped; native unit fixtures
check cycle diagnostics. Only this test needs the supported EXE, existing
project `.venv`, and Unicorn outside the macOS execution sandbox.

Reports are under `work/native-validation/`: `symbols.json`,
`symbols-sanitize.json`, `symbols-topology.json`, `symbols-oracle.json` and
`symbols-cli.json`. All work is separate from the playable game DLL and assets.
