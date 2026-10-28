# Portable native core

Independent C++20 library and CLIs for THUG2 PRE/PRX version 3 archives and
read-only resource lookup across archives and loose directories, plus a
[QB inspector](QB.md) for token scripts and [precompiled images](PRECOMPILED.md). The
shared [symbol table](SYMBOLS.md) combines their definitions and resolves aliases.
The [value evaluator](VALUES.md) materializes values and evaluates static QB expressions.
The bounded [QB executor](RUNTIME.md) runs a subset with explicit host bindings.
The macOS build is native ARM64 and uses only the standard library. Windows,
CrossOver, UTM, game DLLs and the original EXE are not needed to build or run it.

From the project root:

```sh
cmake -S native -B work/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/native-build
ctest --test-dir work/native-build --output-on-failure

work/native-build/thug2-prx list work/english-game/Game/Data/pre/APcol.prx
work/native-build/thug2-prx verify work/english-game/Game/Data/pre/*.prx
work/native-build/thug2-prx extract work/english-game/Game/Data/pre/APcol.prx work/APcol-extracted
work/native-build/thug2-prx crc 'levels/AP/AP.col.xbx'
```

`extract` requires a **new destination directory with an existing parent**.
It rejects absolute paths, drive prefixes and `.`/`..` components. It writes
regular files and refuses existing destinations, including symlinks. Parent
aliases such as macOS `/tmp` are resolved before extraction. A decode or I/O
error can leave a partial destination; use another new directory when retrying.

`list` prints archive, decoded size, compressed size (zero means raw), entry
status and filename, separated by tabs. `verify` decodes every entry, including
duplicates, checks exact sizes, and compares fresh and reused decoder contexts.

## Library boundaries

- Explicit little-endian parsing and bounds checks; no mapped game structs,
  pointer casts, EXE addresses, assembly or 32-bit pointer requirements.
- Game CRC: reflected polynomial `0xedb88320`, seed `0xffffffff`, no final XOR.
  Filename CRC folds ASCII case and `/` to `\`. Empty buffer CRC is zero;
  empty filename CRC is `0xffffffff`, matching the game.
- Archive-specific `Pre::Decode` LZSS uses a caller-owned 4096-byte window.
  Its first 4078 bytes reset to spaces per call; its last 18 retain state.
  New contexts start at zero. Failed decodes do not commit state. Bad streams
  raise errors instead of reproducing out-of-bounds reads or assertion loops.
- Entry name padding may contain garbage after the terminating NUL. The stored
  checksum is exposed but is not treated as a filename CRC. Filename hash
  collisions follow the game's first-wins rule for extraction;
  `Archive::read(index, decoder)` can inspect any entry.

This is a standalone resource layer. It does not yet replace the game's file manager,
allocator, QB interpreter, renderer or general LZSS encoder. It is not yet
integrated into the working DLL. See [NOTICE.md](NOTICE.md) for provenance.

## Validation on 2026-10-28

The supplied English game's corpus has 115 archives, 14,199 entries and
305,675,908 decoded bytes. Release and AddressSanitizer/UndefinedBehaviorSanitizer
builds both pass unit tests and full corpus verification. Tests cover
CRC, overlapping and wrapping references, retained window state, malformed
streams, PRX bounds, duplicates, paths, existing destinations and symlinks.

Unicorn executes the original x86 decoder at `0x005B7DF0`: **all 9,668 compressed
entries match byte for byte**, totaling 243,264,379 bytes. All 7,151 unique
filename/test CRC inputs match original code at `0x00401B90`. CLI extraction of
`AP_scripts.prx` produced 197 verified files and refused an existing destination.
The working game build is unchanged.

Optional differential test, using a project-local `.venv` and the supported
user-owned EXE (test dependencies only). Set up the Python environment in the
repository; it is not needed for C++ builds or unit tests:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r native/requirements-oracle.txt
```

The defaults expect private fixtures in `work/english-game/Game` (including
`THUG2PM.exe` and `Data/pre`). Alternatively, use the test script's `--exe`,
`--archives` or `--data` options for your local installation, as listed by
`--help`. Nothing downloads game files. Builds, fixtures, reports and the venv
are ignored by Git.

Run the optional decoder oracle:

```sh
.venv/bin/python tools/verify_native_prx.py
# Short decoder run; still checks all CRCs and sample CLI extraction:
.venv/bin/python tools/verify_native_prx.py --limit 5 --report work/native-validation/integration.json
```

On macOS, Unicorn may terminate with SIGILL under the execution sandbox; this
optional test needs to run outside it. The test maps PE sections into emulated
x86 memory without launching the game, after verifying the EXE SHA-256.
`--library` selects the test-only `core-oracle` shared library on other systems;
the default filename is macOS-specific. Reports stay in
`work/native-validation/`: `oracle.json` records the full decoder comparison;
`integration.json` records the CRC/extraction check. Sample extracted files are
temporary and removed automatically.

## Resource lookup

`thug2/assets.hpp` provides `AssetStore`, a read-only mount table. Sources have
**first-mounted-wins** priority. Mount archives before a Data directory for
archive-first lookup, or put an overlay directory first for local overrides.
The original game's archives are traversed in hash-table order; this API
deliberately requires a deterministic order instead of claiming to reproduce
that table's iteration. Within an archive, original name-CRC first-wins behavior
is preserved, including collisions. Loose files use full normalized names.

```cpp
#include "thug2/assets.hpp"

thug2::AssetStore assets;
auto level = assets.mount_archive("Game/Data/pre/APcol.prx");
assets.mount_directory("Game/Data");
auto file = assets.open("LEVELS/AP/AP.COL.XBX");
if (file) {
    auto data = file->bytes(); // Owned by file; no original game memory.
    // file->read(destination_span), seek(absolute_offset), tell(), eof()
}
assets.unmount(level); // Already opened files keep their data and cursor.
```

Names are relative to a mount root, ASCII case-insensitive, with either slash
style. Absolute paths, NULs and `.`/`..` components are rejected. `find` returns
source metadata without decompression; `open` returns an owning `AssetFile` with
an independent cursor. Missing names return `std::nullopt`; corrupt data, I/O
errors and invalid paths throw exceptions. A selected source's error never
falls through to lower-priority data. Mount IDs are stable and not reused.

Archive bytes are snapshotted at mount; entries decode on open, with a fresh
decoder per file. Directory names/sizes are indexed at mount, while contents
are read on open. Same-size edits are visible; size changes raise an error.
Remount to refresh names/sizes. Linked entries are skipped; a file replaced by
a symlink outside its root is rejected on open. Ambiguous loose names after
ASCII case/separator normalization reject the whole mount. Failed mounts do
not change the search table. Mount changes require external synchronization
with readers. Whole files are loaded into RAM on open; streaming large movies
and audio, async I/O and caching are future work.

CLI sources are specified **in priority order**:

```sh
work/native-build/thug2-assets locate 'LEVELS/AP/AP.COL.XBX' \
  --prx work/english-game/Game/Data/pre/APcol.prx \
  --dir work/english-game/Game/Data

# Raw bytes on stdout; metadata/errors never enter the binary stream:
work/native-build/thug2-assets cat 'levels/AP/AP.col.xbx' \
  --prx work/english-game/Game/Data/pre/APcol.prx > work/AP.col.xbx
```

`locate` prints source kind, source path, actual name and size separated by tabs.
Both commands return 2 for a missing asset, 1 for an error and 0 on success.
Neither command writes into game directories. There are no implicit Data
prefixes, language substitutions, `.pre` aliases or automatic archive loading.

Additional tests cover priority across archives/directories, unmount/remount,
loose fallback, case/separators, partial reads/seeks, independent cursors,
open-file lifetime, empty files, mount failures and source errors. The optional
local corpus test mounts 115 sorted PRX files followed by Data, then compares
each selected archive asset against a direct entry read. It checks all 14,199
entry lookups: 6,806 selected assets (183,402,846 bytes), 7,393 shadowed entries,
plus 3,426 loose fallback files of at most 64 KiB (37,301,104 bytes). It does not
claim to test all large loose files or to emulate original archive load order.
Release and AddressSanitizer/UndefinedBehaviorSanitizer corpus runs pass; reports
are `work/native-validation/assets.json` and `assets-sanitize.json`. CLI checks
also confirm the AP collision asset's SHA-256 against the earlier original-x86
oracle, loose-file byte equality, and missing/invalid-path exit codes; recorded
in `work/native-validation/assets-cli.json`.

```sh
work/native-build/assets-corpus work/english-game/Game/Data work/native-validation/assets.json
```

The [QB inspector](QB.md) now uses `AssetStore` to index functions/globals and
inspect token scripts and `.qb.xbx` values without `PreFile` or EXE globals.
The [symbol table](SYMBOLS.md) now links their global definitions and name aliases.
The [value evaluator](VALUES.md) handles data and expressions; the
[QB executor](RUNTIME.md) now runs a bounded subset of scripts. It does not
connect engine objects or the gameplay scheduler. Two explicit data hosts now
run [original profile-menu QB](HOSTS.md); no integration into the playable DLL
has been performed by these steps.

The [native QB executor](RUNTIME.md) supports opt-in Wait and explicit per-frame
continuations with retained call/loop stacks, plus compiled switch/case dispatch. `thug2-run --frames N` drives a
bounded simulated clock; game rendering, objects and events remain separate.

[Random statement jumps](RANDOM.md) now use the two original RNG streams,
weighted choices and bounded no-repeat/permutation histories; `--seed N`
makes fresh executions reproducible. Inline random values work in runtime arguments/expressions.
[Runtime arrays](ARRAYS.md) now preserve two-pass random selection, nested
values, sizing types and scalar zero tails, with checked capacity.

Hosted CI tests Linux x64, Windows x64 and macOS Intel (`macos-15-intel`). The
macOS label is explicit because ARM hosted runners failed to acquire capacity
on 2026-10-28; local macOS ARM Release and ASan/UBSan checks also pass. Runner
labels: https://docs.github.com/en/actions/reference/runners/github-hosted-runners

[RandomRange](RANDOM.md) now preserves original integer/float selection and
rounding. `--hosts data` connects GetArraySize and StructureContains; the
[profile corpus](HOSTS.md) validates original game-script name lookups for all
31 profiles and the 18-visible-profile count without the original EXE.
