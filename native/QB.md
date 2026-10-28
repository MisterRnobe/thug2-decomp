# QB inspector: token scripts

`thug2/qb.hpp` reads THUG2 token scripts using portable C++20 and owned bytes.
`thug2-qb` loads them through `AssetStore`, from PRX entries or loose files, with
sources in explicit priority order. It also detects and reads
[precompiled images](PRECOMPILED.md). It does not load an EXE, call game functions,
relocate pointers or execute scripts.

```sh
cmake --build work/native-build

work/native-build/thug2-qb summary scripts/game/cas_skater_shared.qb \
  --prx work/english-game/Game/Data/pre/qb_scripts.prx

work/native-build/thug2-qb symbols scripts/aaaaloopchecker.qb \
  --dir work/english-game/Game/Data

work/native-build/thug2-qb dump scripts/aaaaloopchecker.qb \
  --dir work/english-game/Game/Data
```

`summary` reports the source, bytes/tokens, function definitions, globals,
checksum-name records and jump targets. `symbols` lists functions and globals
with names/CRCs, offsets and function checksums. `dump` prints token offsets,
mnemonics and literal values. This is an annotated token dump, not reconstructed
source. Uncertain semantic labels remain `token_XX`. Text is escaped bytewise
rather than assuming UTF-8; unresolved names remain CRCs. Errors include the
byte offset. Exit codes: 0 success, 2 missing resource, 1 malformed data/error.

The library exposes `Document::tokens()`, `scripts()`, `symbols()`, `names()` and
`jumps()`. Metadata uses offsets into owned bytes. Global values are indexed as
byte ranges without evaluating expressions, following aliases or materializing
game structures. Functions inside global structures are also indexed, with
their container depth. Repeated definitions and checksum-name records remain
in source order; display-name lookup uses the first spelling. This is not a
runtime symbol table.

A separate [shared symbol table](SYMBOLS.md) now indexes globals and top-level
functions across modules, handling explicit name aliases and load ownership.

Checks include bounded token payloads, terminated strings, random-table counts,
balanced braces/brackets/parentheses, named script headers, matching endscript,
EOF with no trailing bytes, and relative targets landing on token boundaries.
Random tables contain separate weight and offset arrays. Long jumps support
x86 wrapped/signed displacements. Script CRCs omit line endings/line numbers,
the definition header and endscript, with seed `0xffffffff` and no final XOR.
Numeric reads are explicitly little-endian; float display uses IEEE-754 bits.

These are **framing and structural checks, not complete grammar or runtime
validation**. If/switch/loop semantics, expression evaluation, random selection,
symbol resolution and engine calls remain unimplemented. Jump checks do not
prove correct control flow or termination. The general reader consumes EOF as
one byte; original `SkipToken` stays at EOF, which its caller handles separately.

## Two QB formats

Ordinary `.qb` files contain tokens. The corpus's `.qb.xbx` files are precompiled
symbol images with magic `0xb075ef6f`, containing structures, arrays and stored
pointer offsets. The CLI detects this magic and selects the
[precompiled parser](PRECOMPILED.md). The token-only `Document` API still rejects
images, and the 95 images are excluded from the token-corpus counts below.

## Validation on 2026-10-28

```sh
ctest --test-dir work/native-build --output-on-failure
work/native-build/qb-corpus work/english-game/Game/Data work/native-validation/qb.json
.venv/bin/python tools/verify_native_qb.py
```

The corpus command mounts 115 sorted PRX files, then Data. The selected **470
token assets** (380 archived, 90 loose) contain 3,674,766 tokens, 16,551 function
definitions, 10,351 indexed globals and 24,507 jump targets. Release and
AddressSanitizer/UndefinedBehaviorSanitizer runs pass. Tests cover truncated
payloads, delimiters/scripts, embedded functions, jump tables, signed jumps,
literals, names and line-independent CRCs.

The optional differential test includes shadowed versions: **869 fixtures**
(470 loose files plus 399 archive entries), 6,349,670 token boundaries and 32,840
function checksums match an independent Python framing/CRC reference. Original
x86 `SkipToken` (`0x0046f690`) additionally matches 54,884 sampled token steps,
covering each observed opcode per fixture plus random tokens. Original
`CalculateScriptChecksum` (`0x0046f960`) matches 1,352 sampled functions. The x86
checks are samples, not execution of every function or every token step.

Only the optional differential test needs the supported user-owned EXE, existing
project `.venv` dependencies, and Unicorn outside the macOS execution sandbox.
It checks the EXE SHA-256 before mapping sections into emulated x86 memory.
Test-only `core-oracle` exports supply native offsets/checksums. `--library`
selects its platform-specific shared library. The reader has no such dependency.

Reports in `work/native-validation/`: `qb.json`, `qb-sanitize.json`,
`qb-oracle.json`, `qb-cli.json`. CLI checks cover archive reads, named globals,
dumps and missing assets. See the precompiled page for the image CLI tests. The working
game build and its data are unchanged.
