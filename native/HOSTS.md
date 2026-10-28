# Native data hosts and the first real QB scenario

`Interpreter::install_data_hosts()` explicitly registers **GetArraySize** and
**StructureContains**. CLI opts in with `--hosts data`; unknown groups are
rejected. Neither command mutates globals, arrays or structures. Rendering,
engine objects, input, audio and gameplay services remain unconnected.

## Command contracts

`GetArraySize <array> [Index1=i [index2=j [index3=k]]]` writes integer
`array_size` into the current script's parameters and returns true. It selects
the last correctly typed unnamed array, resolving global names/aliases.
Subsequent non-array flags do not erase that match. An Index2 is used only
when Index1 exists; Index3 requires both preceding indices. Each index must
select a nested array. Missing/wrong arrays, negative/out-of-range indices and
non-array children fault explicitly instead of reproducing original invalid
pointer reads. Arrays and output size are bounded.

`StructureContains Structure={...} <name>` returns whether the structure has
a component of that name or an unnamed flag with that checksum. Presence of a
zero-valued field counts. Structures can be supplied through globals; unnamed
included global structures are traversed. The requested checksum stays a raw
name even if it also names a global value. An absent/wrong structure returns
false. Included-structure cycles and excessive depth/work fault.

Typed search follows CStruct::Search's last-match rule and ignores values of
other types. Lookups use the interpreter's fixed definition snapshot, not
external engine memory. Search costs are charged before host outputs merge;
host searches also cap their own work at 250,000 and depth at 128. Unknown
engine commands still fault; nothing is automatically stubbed as successful.

The real-script integration also fixes `<param> = value`: the target is the
written checksum before `=`, as ExecuteCommand re-reads it. The assignment
works when the old parameter is absent, numeric or names another symbol.
Dynamic command-name reads retain their existing separate behavior.

## Real skater-profile QB

```sh
cmake --build work/native-build
work/native-build/profile-corpus /path/to/Game/Data work/profile-corpus.json
```

The optional corpus executable uses the same inspection snapshot as the other
catalog tools: 115 sorted archives, then loose Data, with 565 selected QB
modules. It runs the actual unmodified game functions from
`scripts/game/skater/skater_profile.qb`:

- `get_skater_index_from_name`: **all 31 profile names** return their correct
  indices, checked against an independent traversal of `master_skater_list`.
- `get_num_visible_profiles`: returns **18 visible of 31 total**, checked
  against the profile structures' `not_in_frontend` presence/flags.

This runs the original QB loops, expressions, array indexing, field reads,
conditions, reference assignments and returns against decoded game data, with
two native command implementations. No script bodies/assets are copied into
Git and no original EXE, Windows DLL or allocator executes. The test allows
100,000 fuel per call. Release and ASan/UBSan reports match exactly.

This validates base profile-menu logic on the inspection snapshot. It does not
render a menu, emulate runtime unlock/progression state, or connect a skater
object. Unknown-name lookup reaches `script_assert`, which remains unbound;
that error path is not claimed as implemented. Dynamic loading/mutable engine
globals and original cache identity/lifetimes remain separate work.

## Validation

14 CTest suites pass in Release and ASan/UBSan builds. Data-host tests cover
opt-in binding, typed searches, global aliases, all three nested indices,
ignored missing earlier indices, immutable input arrays, missing/wrong/out-of-
bounds values, named fields and flags, included globals, raw checksum names,
cycle bounds, definition snapshot lifetime and reference-assignment targets.
CLI smoke checks cover `--hosts data`, missing bindings and unknown groups.
These host implementations are checked against source conventions and real
QB/reference scans; there is no claim of x86-oracling the complete CFunctions.

Reports: `work/profile-corpus.json`, `profile-sanitize.json`,
`range-data-cli.json`. See [RANDOM.md](RANDOM.md) for RandomRange's independent
x86 differential checks and [RUNTIME.md](RUNTIME.md) for VM limitations.
