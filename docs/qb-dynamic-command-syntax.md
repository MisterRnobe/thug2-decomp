# Dynamic command names (ReadName parameter forms)

Implementation of #42 in the format-2 pipeline (`tools/qbjs/reader.ts`,
`tools/qbjs/ir.ts`, `tools/qbjs/emitter.ts`, `tools/qbjs/rich-helper-inventory.ts`).
This lifts the last syntax loading blocker for the whole unchanged
`view_goals_menu.qb`: command names (and `name:command` objects) that come from
script parameters rather than literal checksums. Dynamic dispatch itself is
**not implemented** — the goal is to load the module and fault honestly when
such a call is reached.

This supersedes the "still unsupported" note for the `0x1385` blocker in
[qb-conditional-not-qualified-calls.md](qb-conditional-not-qualified-calls.md).

## The three name forms

[CScript::ReadName](../src/game/script/cscript_exec.cpp) (`0x004729E0`) accepts
three spellings wherever a command name is read, and
`ExecuteCommand` (`0x00474B40`) reads the object of a `name:command` call with
the same routine:

| Tokens | Meaning |
| --- | --- |
| `16 <crc>` | a literal checksum name (fully static) |
| `2d 16 <param-crc>` | a checksum read from the named script parameter |
| `2c` | a checksum read from the unnamed checksum parameter |

Because both slots go through `ReadName`, every combination of static/dynamic
object and command is grammatically possible. The reader now parses all of
them, in statement position and as `if` conditions (plain or under a `NOT`
prefix), with full argument parsing — no byte skipping and no assumed fixed
widths. Malformed or truncated forms fail with the usual bounded
`<source>: QB offset 0x...` diagnostic.

## IR shape

The distinction a name came from is part of its identity, so the dynamic forms
get their own node kind rather than piggybacking on existing expression kinds
([ir.ts](../tools/qbjs/ir.ts)):

- `NameSource` — `nameLiteral` / `nameParameter` / `nameUnnamed`, each with its
  original offset. `nameParameter` keeps the CRC of the parameter it reads
  *from*; the resolved checksum never exists statically.
- `DynamicCall` (`kind: "dynamic"`) — the command `NameSource`, its
  `commandOffset`, the parsed argument fields, and (for qualified calls) the
  object `NameSource`. A static object inside a dynamic call stays a
  `nameLiteral`, so the fault keeps the full object/command identity instead of
  a bare "dynamic" flag.
- Static names keep the pre-existing `call` / `qualified` shapes unchanged.

Two forms stay rejected explicitly, because they are not part of `ReadName`
dispatch: the unnamed `2c =` dynamic assignment, and any dynamic name in
condition position followed by `=`. The `2d 16 <crc> = value` statement is *not*
dynamic — it is the static assignment to the script parameter named `<crc>`
(the pre-existing reader branch) and keeps working.

## Execution policy

The emitter defines every function at load time; only reaching a dynamic call
faults ([emitter.ts](../tools/qbjs/emitter.ts)):

```
throw Object.assign(Error(`QB dynamic command <object-descriptor>:<command-descriptor>
    requires dynamic command dispatch, which is not implemented`), {site: {...}});
```

- Descriptors mirror the three forms: `0x<crc>`, `parameter 0x<crc>`, `unnamed
  parameter`, joined with `:` for qualified calls.
- The fault fires **before** argument evaluation and before any host dispatch:
  an argument that would itself fault (a missing parameter) is never touched.
- There is no `v.param` checksum resolution, no global `qb.call` fallback, no
  success stub, and no arbitrary dispatch target — even when the parameter
  actually holds a correct `Name` checksum at run time.
- A dynamic `if` condition faults on its single evaluation, under `NOT` too,
  with the same site semantics as any other faulting condition.
- Dynamic calls in untaken branches or uninvoked bodies never fault; static
  behavior (plain and qualified static calls) is unchanged.

## Inventory honesty

`rich-helper-inventory.ts` distinguishes a syntax-readable module from the
fully executable subset: a module containing dynamic or qualified call sites
gets `status: "readable"` instead of `"supported"`, and each script reports
`execution: { objectDispatch, dynamicDispatch, dynamicCommandOffsets }` —
descriptor offsets only, never bodies, payloads or generated JS. Traversal
keeps the existing node budget and cannot silently lose dynamic nodes.

## Whole original module

The complete unchanged local
`scripts/game/menu/view_goals_menu.qb` (SHA-256
`a23f36949053d5408ce78f3e8cceadecb3ef0361b584be6896ff1df1ee9728e9`, 33,796
bytes) now reads and emits through the production pipeline — no extraction, no
rewriting, no skipped definitions:

- 57 definitions (55 scripts, 2 globals).
- 18 dynamic sites preserved: 15 statements (11 of them qualified calls whose
  object or command name comes from a parameter) and 3 `if` conditions.
- The emitted JS (480,097 bytes) defines every function and contains exactly
  18 dynamic fault throws, e.g. `QB dynamic command parameter 0x8e8858e5
  requires dynamic command dispatch, which is not implemented`.
- The inventory reports the module as `readable`: 7 scripts need dynamic
  dispatch, 20 need object dispatch.

No new loading blockers surfaced: this was the last construct the reader
rejected. Dynamic/object *dispatch* remains the execution boundary — the
module is readable, not yet runnable. No original module body, IR or generated
JS is stored in the repository; the public regression suites run on synthetic
fixtures only.

## Checks

```sh
npx tsc -p tools/qbjs/tsconfig.json        # types
npx biome check tools/qbjs                 # style
node --test "tools/qbjs/*.test.ts"          # reader/emitter/inventory regressions
QB_QUICKJS_DRIVER=build-scripting/qb-js-script-tests \
    node --test tools/qbjs/dynamic-command.test.ts   # real QuickJS
ctest --test-dir build-scripting --output-on-failure
```

`tools/qbjs/dynamic-command-fixtures.ts` builds the synthetic module (all
qualified combinations, conditions with `NOT`, untaken/uninvoked sites,
fault-before-argument-evaluation proof) and a self-contained test bundle with
its own TEST-ONLY dispatcher, so `emitter-fixtures.ts` stays untouched. The
bundle runs both in the Node VM and, via `QB_QUICKJS_DRIVER`, in the embedded
QuickJS interpreter. `reader.test.ts` keeps a regression for the new parsing
and for the rejections that remain true.
