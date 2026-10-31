# Phase2 foundation integration

This branch transfers runtime fixes from upstream-proposal `94e5bc7` onto
phase2 `677f5fa`. It retains phase2 NOT, static qualified-command diagnostics,
random statement/control blocks and research oracle tools.

Implemented: quickjs-ng v0.17.0, once-only embedded helpers, shared precision-24
numeric arithmetic, explicit owning rich transport, absent-expression semantics,
QScript identity values, per-update fuel, task-local interrupt recovery and
GCC/Clang/MSVC CI. Rich codec resource/error tests and helper-level field/angles
regressions run on Node and embedded QuickJS. No game path is enabled.

## Remaining integration gates

Issue #42 owns reader/IR/emitter changes. This foundation deliberately leaves
reader.ts, ir.ts, emitter.ts, rich-helper-inventory.ts and reader.test.ts unchanged.

- Replace the emitter-local field builder with `v.fields(p, entries, ctx)`.
- Mark literal argument/default vectors using entry slot 3 (slot 2 is flatten):
  `[name, value, false, true]`. Do not mark expression-result/assignment vectors.
- Switch the gated rich-helper inventory test to guarded helperSource loading;
  Runtime already embeds helpers. Do not enable its QuickJS driver globally yet.
- Parse dynamic command names and retain an explicit reached-only unsupported
  diagnostic, with no argument/host side effects, per #42.

Until that adapter is integrated, linear field cost and near-zero literal
`angles` omission are helper capabilities, not generated-module guarantees.
Full view_goals_menu loading and the gameplay scenario in #27 remain pending.

## Research evidence

Existing optional Python/Unicorn array/RNG probes remain on phase2, outside the
runtime dependency path. RNG uses gameplay FPCW 0x007f; the existing two-pass array
research keeps its explicitly recorded offline 0x037f profile. The array tool's
QuickJS step is a value-construction smoke test, not a two-pass JS differential.
Original files, executable images and generated reports stay local under work/.
These research tools are not carried unchanged into the upstream proposal.
