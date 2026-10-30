# Conditional NOT and object-qualified commands

Implementation of #31 in the format-2 pipeline (`tools/qbjs/reader.ts`,
`tools/qbjs/ir.ts`, `tools/qbjs/emitter.ts`). Both constructs come from
[CScript::ExecuteIf / ExecuteCommand](../src/game/script/cscript_exec.cpp)
(`0x00475050` / `0x00474B40`); this documents the translated semantics and the
remaining original-data blockers found by re-reading the whole module.

## `if not <condition>` (token 0x39)

The original `ExecuteIf` reads the 2-byte jump target first, then consumes an
optional `0x39` NOT token *before* the condition command:

```
++m_pc; target = m_pc + Read2Bytes(m_pc); m_pc += 2;
if (*m_pc == 0x39) { ++m_pc; negate = true; }
condition = ExecuteCommand();          // runs exactly once
if (!m_pc) return;                     // a fault aborts the if: no jump
if (negate) condition = !condition;
if (!condition) m_pc = target;         // jump only on a false result
```

The reader stores the prefix as `negate: true` on the `if` IR node
([ir.ts](../tools/qbjs/ir.ts)). The emitter keeps the original evaluation
order exactly:

- The condition is evaluated **once**, into the `if (...)` test itself;
  effects (script calls, host outputs) happen before the negation.
- A QB-script condition counts as `true` before negation
  ([runtime.js](../scripting/runtime.js) `qb.call` returns `true` for script
  callees), so `if not <script-call>` always takes the else part while the
  callee's effects are still applied.
- A faulting condition aborts the whole `if` — the throw propagates out of
  the test expression, mirroring `if (!m_pc) return;` — with the original
  source/script/offset site attached.
- A qualified condition or plain call still yields a generator, so
  suspension (`yield`) inside the condition keeps working under `not`.

## Object-qualified commands (token 0x42)

`name:command args` compiles to the object name followed by `0x42`, then the
command name. `ExecuteCommand` resolves the object with `FindObject` and, when
it is missing, returns `true` with `m_pc` left at the `:` — the next pass then
runs the `:` as a statement looking for the object named `0`.

The reader preserves the full identity in IR: a `qualified` node with the
object checksum (`object`), the command checksum (`name`), the statement
offset and the separate `objectOffset` / `commandOffset` original positions.
It appears both as a statement and as an `if` condition (the original
dispatches both through the same `ExecuteCommand`).

Object dispatch is **not implemented**. Until an object contract exists, the
emitter generates an explicit fault at the original site:

```
throw Object.assign(Error(`QB object-qualified command 0x<obj>:0x<cmd>
    requires object dispatch, which is not implemented`), {site: {...}});
```

This is deliberately not a global `qb.call`, not a success stub, and not the
original's missing-object fallback (which would mis-execute the `:`). Loading
a module that only *contains* qualified commands still succeeds; running one
faults with the original offsets.

## Re-reading the whole original module

`scripts/game/menu/view_goals_menu.qb` (SHA-256
`a23f36949053d5408ce78f3e8cceadecb3ef0361b584be6896ff1df1ee9728e9`, 33,796
bytes) was re-read definition by definition after the NOT/qualified fixes —
no script extracted, no definition skipped. The two previously reported
blockers are gone:

- `0xb1`: `unsupported condition command token` — the helper's `if not`
  prefix (26 negated `if`s across the module).
- `0x36a`: `unsupported value token 0x42` — object-qualified statements
  (23 statements and 16 `if` conditions).

Two further constructs surfaced, in this order:

1. `0x474`: `case exit does not target endswitch`. The original compiler
   targets the end-of-line token *after* the `0x3d` endswitch
   (`ResolveRandomJumps` computes `49 = +1 + delta`), which is semantically
   the same landing point. The reader now accepts the endswitch itself and
   the EOL right after it.
2. `0x1385`: `dynamic command name from parameter` — **still unsupported, and
   now rejected explicitly**. `ExecuteCommand`'s `ReadName` accepts not only
   `16 <crc>` but also `2d 16 <param>` (a checksum read from a parameter) and
   `2c` (the unnamed spread), both as the command name and as the object of a
   `name:command` call. The module contains 18 such sites (15 statements,
   3 `if` conditions; 11 of the statements are qualified calls whose object
   name comes from a parameter). Dynamic dispatch has no static IR name, so
   the reader rejects it with a specific diagnostic instead of misparsing.

With only the dynamic-name sites tolerated by a throwaway probe (not part of
this change), the remaining 57 definitions (55 scripts, 2 globals) read and
emit cleanly — 484 KB of generated JS. This is a grammar statement only; no
original module body, IR or generated JS is stored in the repository. The
public regression suites continue to run on synthetic fixtures.

## Checks

```sh
npx tsc -p tools/qbjs/tsconfig.json        # types
npx biome check tools/qbjs                 # style
node --test "tools/qbjs/*.test.ts"          # reader/emitter/runtime regressions
cmake -S scripting -B build-scripting -DCMAKE_BUILD_TYPE=Release
cmake --build build-scripting --parallel 4
ctest --test-dir build-scripting --output-on-failure   # real QuickJS suites
```

The emitter fixtures add `notHost2` / `notScript2` / `notExpr2` /
`notExprTrue2` / `notFault2` (conditions, effects, script-call-true,
transitions, fault propagation) and `qualified2` / `qualifiedCond2`
(preserved identity, explicit fault, original site), executed both on the
Node VM and in the embedded QuickJS `qb-js-*` test drivers.
