# Native data hosts and the first real QB scenario

`Interpreter::install_data_hosts()` explicitly registers **GetArraySize**,
**StructureContains** and **GotParam**. CLI opts in with `--hosts data`; unknown groups are
rejected. These commands do not mutate globals, arrays or structures. Rendering,
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
included global structures are traversed. The requested checksum uses the original GetChecksum rules described below. An absent/wrong structure returns
false. Included-structure cycles and excessive depth/work fault.

`GotParam <name>` checks the **current QB frame's parameters**, not the
command's argument list or global symbol existence. It sees named fields of
any type, including zero/false values, unnamed flags and unnamed included
global structures. A callee sees its own defaults/arguments; caller values
arrive only when explicitly passed/spread. It returns a condition without
changing parameters. Recursive inclusions use the same cycle/work bounds.

Checksum queries keep the last valid unnamed name. Missing or wrong-type
queries default to checksum zero, so any unnamed caller component then counts
as present. **An unnamed name referring to a global structure is skipped**, as
CStruct::GetChecksum does; it neither replaces an earlier valid query nor
expands the structure for this unnamed lookup. Names referring to other types
remain raw checksums. This rule now also applies to StructureContains and was
verified against the complete original getter and CFunction.

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
the two supporting data commands (the group additionally registers GotParam). No script bodies/assets are copied into
Git and no original EXE, Windows DLL or allocator executes. The test allows
100,000 fuel per call. Release and ASan/UBSan reports match exactly.

This validates base profile-menu logic on the inspection snapshot. It does not
render a menu, emulate runtime unlock/progression state, or connect a skater
object. Unknown-name lookup reaches `script_assert`, which remains unbound;
that error path is not claimed as implemented. Dynamic loading/mutable engine
globals and original cache identity/lifetimes remain separate work.

## Validation

15 CTest suites pass in Release and ASan/UBSan builds. Data-host tests cover
opt-in binding, typed searches, global aliases, all three nested indices,
ignored missing earlier indices, immutable input arrays, missing/wrong/out-of-
bounds values, named fields and flags, included globals, raw checksum names,
cycle bounds, definition snapshot lifetime and reference-assignment targets.
CLI smoke checks cover `--hosts data`, missing bindings and unknown groups.
These host implementations are checked against source conventions and real
QB/reference scans; GetArraySize/StructureContains are not claimed as complete x86-oracled
CFunctions. GotParam has the independent full-function check below.

Reports: `work/profile-corpus.json`, `profile-sanitize.json`,
`range-data-cli.json`. See [RANDOM.md](RANDOM.md) for RandomRange's independent
x86 differential checks and [RUNTIME.md](RUNTIME.md) for VM limitations.

## Original menu callback scenario

```sh
work/native-build/menu-callback-corpus /path/to/Game/Data work/menu-callback-corpus.json
```

This runs unmodified `generic_menu_pad_choose` from
`scripts/game/menu/gamemenu_helpers.qb`. GotParam decides whether a `callback`
parameter exists. A supplied checksum is dynamically called as QB and receives
`<...>` parameters. Return values merge into the calling frame.

All **33 cases** pass: absent callback finishes without callback output; a
profile-count callback returns 18; and callbacks for all 31 profile-name
lookups return their correct indices while retaining a marker parameter.
The corpus shares the 565-module inspection snapshot and 100,000 fuel per
call. Release and ASan/UBSan reports match. These are original handler/callee
control paths with valid QB callbacks, not a rendered menu or input-device
integration. Arbitrary object/native callbacks remain outside this test.
The original direct 31-profile/18-visible corpus still passes.

## GotParam x86 check and data-enabled audits

```sh
/path/to/project/.venv/bin/python tools/verify_native_gotparam.py \
  --exe /path/to/Game/THUG2PM.exe --report work/gotparam-oracle.json
work/native-build/runtime-corpus /path/to/Game/Data work/gotparam-data.json --data
work/native-build/runtime-corpus /path/to/Game/Data work/gotparam-data-wait.json --data-wait
```

**490 checks match** the complete original GotParam (`0x0044bd30`), specialized
GetChecksum (`0x00476950`), FindComponent/ContainsComponentNamed and symbol
lookup. Synthetic PC components/global tables represent the same QB values
used by the native runner. No original helper is stubbed and no allocator or
full engine runs. Tests cover absent/zero values, flags, raw names, aliases,
included globals, named nested structures, fallback query zero and query
selection. Native unit tests separately cover cyclic inclusions, fuel,
callee isolation, defaults, parameter forwarding and immutable inputs.

The data modes enable three hosts, optionally Wait as a fourth. Otherwise
the audit conditions stay fixed: 15,728 definitions in CRC order, seed 0,
empty inputs, 500 lifetime fuel and up to four simulated updates with Wait.

| Mode | Finished | Waiting | Limited | Missing | Type | Unsupported |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `--data` | 1,051 | 0 | 557 | 13,646 | 39 | 435 |
| `--data-wait` | 1,053 | 102 | 557 | 13,535 | 39 | 442 |

These counts measure the complete data group, not GotParam alone. The small
fuel budget also limits heavy searches/arrays. Empty inputs are insufficient
for many valid scripts, and finishing is not complete gameplay validation.
The Data+Wait catalog reports match under Release and ASan/UBSan.

Additional local reports: `work/gotparam-oracle.json`, `gotparam-data.json`,
`gotparam-data-wait.json`, `gotparam-data-wait-sanitize.json`,
`menu-callback-corpus.json`, `menu-callback-sanitize.json`,
`gotparam-profiles.json` and `gotparam-cli.json`. No game scripts/assets are
committed and the original EXE is opened read-only by the optional oracle.
