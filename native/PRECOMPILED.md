# Precompiled QB images

`thug2/precompiled.hpp` reads the packed `.qb.xbx` symbol images in the supplied
game data. `thug2-qb` detects their magic (`0xb075ef6f`) and supports the same
`summary`, `symbols` and `dump` commands as for token QB. It uses `AssetStore`
and works natively without Windows, the game EXE or game memory.

```sh
cmake --build work/native-build
work/native-build/thug2-qb summary levels/default/default_Scripts.qb.xbx \
  --dir work/english-game/Game/Data
work/native-build/thug2-qb symbols models/UI_bg/UI_bg.qb.xbx \
  --dir work/english-game/Game/Data
work/native-build/thug2-qb dump levels/default/default_Scripts.qb.xbx \
  --dir work/english-game/Game/Data
```

`dump` prints values and child indices for structures/arrays, and token dumps
for stored script bodies. Script token offsets shown after `script N+0x...`
and jump targets are relative to the decoded body, not the image.
Names are retained as CRCs: these images do not provide the token files'
checksum-name dictionaries.

## Portable representation

`PrecompiledImage` owns immutable input bytes. Its public vectors contain:

- `symbols()`: names/CRCs, serialized symbol spans and value indices.
- `values()`: type, name, original raw word, field/data offsets, children,
  string bytes, pair/vector IEEE-754 words, and optional script index.
- `references()`: each stored offset field and its target offset, including
  structure heads, component links, array tables and pointer-valued elements.
- `scripts()`: raw or decoded code, checksum, stored size, token count and
  whether it came from a symbol block or a component.

Integers, floats, names and function-reference words remain values. Known
pointer-valued types are followed as offsets. Unknown types retain their raw
word as opaque scalar data, as the original relocator's default case does.
Named references are not resolved against an engine symbol table. No stored
offset is turned into a host pointer, and no on-disk struct is reinterpreted
as a C++ object. Pair/vector words preserve their exact bits. Text dumps escape
non-ASCII/control bytes without assuming UTF-8.

## Serialized layout

All 95 supplied images have the same 20-byte layout fingerprint at bytes 8..27:

```text
1c0802041004080c0c0802041402040c10100c00
```

This reader explicitly supports that layout and rejects other fingerprints.
The file header is 28 bytes; symbols are 20 bytes, components 16, structures 8
and arrays 12. **The serialized type fields differ from the reconstructed PC
structs**: symbol/array types are at byte +2; component type/flags at +1 (type
is that byte shifted right one). The PC routines expect +1 and +0 respectively.
The fingerprint is therefore checked rather than assuming the PC C++ ABI.

Data is packed after its header/table. Structures contain linked components;
arrays with more than one element have an offset/value table, singletons use
the inline slot, and empty arrays have no payload. String-array texts are
packed without individual alignment, with padding only at the array end.
Individual strings, pairs, vectors and containers are aligned as stored.

The parser checks the declared file size, alignment, header/payload bounds,
table counts, strings, exact inline offsets and component links. Backward,
cyclic, shared or non-packed object layouts are rejected; they are not part of
the supported packed variant. Recursion is limited to 128 levels. Arrays of
scripts are explicitly unsupported (none occur in this corpus).

Symbol scripts have a `{checksum, decoded size, stored size}` header, followed
by code; compressed bodies use the same valid-stream LZSS convention already
used by the archive decoder. Component scripts have raw tokens without that
header. Decoded bodies are validated by the token reader using a synthetic
script header/EOF; symbol script checksums must match the stored checksum.
No expression evaluation, function calls or script execution occurs.

## Validation on 2026-10-28

```sh
ctest --test-dir work/native-build --output-on-failure
work/native-build/precompiled-corpus work/english-game/Game/Data work/native-validation/precompiled.json
.venv/bin/python tools/verify_native_precompiled.py
```

All **95 images** (319,100 bytes) parse: **963 symbols, 15,939 values and 19,061
references**. Their 257 scripts include 3 compressed bodies, with 7,802 decoded
script bytes in total. Release and AddressSanitizer/UndefinedBehaviorSanitizer
tests pass, including bad layouts/sizes/offsets, truncated deep payloads, cycles,
array counts, unaligned string-array texts, script checksums/components and
excessive nesting. CLI checks cover both formats, value trees and script dumps.

The optional differential test first compares all symbol spans and reference
fields against an independent traversal. For an additional x86 comparison, it
**adapts enum-byte positions in test copies only** to the PC ABI, then runs
original `RelocatePrecompiledSymbols` (`0x0046ca50`) with a null callback. The
entire relocated memory image matches the predicted pointer changes for every
file and all 19,061 references. This is an adapted-layout comparison, not proof
that the PC relocator accepts the unmodified serialized files.

Original `CalculateScriptChecksum` confirms all 257 function checksums;
original `Lzss::Decode` (`0x004014f0`) confirms all 3 compressed bodies byte for
byte. The native reader leaves the enum fields and stored offsets unchanged.
Only this optional test needs the supported EXE, existing project `.venv`
modules, and Unicorn outside the macOS execution sandbox. The EXE hash is
checked before mapping it into emulated memory; no game allocators, callbacks
or symbol-table registration are used.

Reports: `work/native-validation/precompiled.json`, `precompiled-sanitize.json`,
`precompiled-oracle.json` and `precompiled-cli.json`. No game data or working
DLL is modified. [Shared symbol linking](SYMBOLS.md) handles global definitions
and name aliases across both formats. The [value evaluator](VALUES.md) provides
common immutable values and static expression evaluation. QB execution remains
separate work.
