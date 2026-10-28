The archive format, checksum conventions and PRE LZSS decoder behavior were
derived from https://github.com/CanuckAussieKev/thug2-decomp at commit
2d82ed2 (2026-10-28), notably
src/game/file/pre.cpp and src/game/core/crc.cpp, and checked against the user's
game data. Original decoder address: 0x005B7DF0; name CRC: 0x00401B90.

Token QB layouts and checksums also use src/game/script/qb.cpp, load.cpp,
parse.cpp and precompiled.h at that commit. The bounded inspector does not
reuse game pointer layouts or execute the interpreter. Optional tests emulate
original SkipToken (0x0046F690) and CalculateScriptChecksum (0x0046F960).

Precompiled image traversal also follows script/precompiled.cpp. The actual
serialized enum-field positions were verified from the supplied files rather
than adopting PC runtime structs. Test copies alone adapt those enum fields
for comparison with original PC RelocatePrecompiledSymbols (0x0046CA50); stored
script decoding is checked with original Lzss::Decode (0x004014F0).

Symbol-table alias lookup, permanent ownership and unchanged-script reuse are
informed by script/struct.cpp and load.cpp. An optional synthetic-PC-table test
checks FindSymbol (0x00478CC0) and LookUpSymbol (0x00478CF0). Portable ownership,
atomic publication, bounded cycle detection and diagnostics are new code.

Static value parsing and operation conventions additionally follow script/parse.cpp,
eval.cpp and struct.cpp. The immutable model, bounded static evaluation and
binary64 error-free transforms for 64-bit significand rounding are new portable
code. Tests compare scalar operations with original Execute (0x0046D450), using
synthetic components and no game allocations or callbacks.

The bounded QB executor's control/parameter rules follow cscript_exec.cpp,
cscript_core.cpp and cscript_wait.cpp. Its owning frames, host boundary, fuel and
diagnostics are new code. Optional tests compare pre-initialized ExecuteRepeat
(0x00472AB0) and ExecuteIf (0x00475050) with a test boolean condition stub; they
do not execute the original engine or full interpreter. Wait tests also compare
Wait/WaitOnePerFrame/WaitTime/WaitRealTime setters and UpdateWait (0x00473050),
with controlled clock-provider stubs. The task registry and explicit frame
policy are new portable code, not the original gameplay scheduler.
Switch dispatch also follows ExecuteSwitch/SkipToEndOfSwitch in cscript_exec.cpp
and ComponentsEqual in struct.cpp. Optional tests execute the original switch
scanner and equality, with controlled literal-parser and component-lifetime
stubs, rather than running the original interpreter or game allocators.

Portable random streams and statement table choices follow mth/random.cpp,
script/parse.cpp (ResolveRandomJumps) and script/qb.cpp (CStoredRandom::Shuffle).
Native owner/site keys, storage and validation are new. Optional tests compare
original RNG, selection and shuffle code with controlled stored-record lookup
and allocation; they do not run the engine. Inline-value flow additionally
follows ParseArguments/ParseStructure/ParseExpression control points. An
optional test runs original ParseArguments and random transfers with a
controlled scalar AddParameter stub, comparing results and continuation state.
The explicit runtime value callback and immutable parser state are new code;
array control flow is rejected until its original two-pass behavior is modeled.

Upstream distinguishes reconstructed game code from its MIT-licensed original
tooling. No MIT license is asserted here for reconstructed game routines. See
../LICENSE and ../NOTICE.md for upstream's notices. Game assets
and executable are not included in this module.

The portable parser, CLI, bounds checks, explicit state and tests are new work
for this project. No EXE addresses, Windows APIs, inline assembly, or original
game memory are used at runtime.
