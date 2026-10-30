# Original-x86 evidence for expression completion

Companion to #29 and [the native reference design](qb-native-references.md).
At phase-2 base `12ea70c`, the new optional
[oracle](../tools/verify_js_expression_result.py) passes **15 synthetic cases**
against the exact locally supplied THUG2PM.exe with SHA-256
`7eb982acfa06ee0cb6884c4334d4c049258903ceaa287fd1d2e700a731b91632`.
No EXE bytes, extracted game scripts or generated reports are committed.

## What actually executes

Unicorn executes the original x86 `CExpressionEvaluator::GetResult`
(`0x0046EC40`) and `CopyComponent` (`0x004773E0`), including the original
reference/owning retag helpers, `CopyArray`, `AppendStructure`, container/component
initializers and component cleanup reached by these cases. CopyComponent and
cleanup hooks **observe** arguments; they do not replace either function.

The controlled replacements are allocation entry points `0x00408F90` (array),
`0x00409030` (structure), `0x005BA3D0` (component pool allocation), and object-free
entry points `0x00408FD0` / `0x00409070`. They return zeroed, separately identified
arena objects or record frees. An artificial component-pool selector routes
AppendStructure to the controlled allocator. There is no real game heap, object
initialization or Windows process. Each original invocation has a 50,000
instruction / one-second execution cap; the arena is bounded. Return PC and
calling convention are checked, as well as observable results.

Arrays contain one element to exercise original copy/cleanup without substituting
the game's element-block allocator and heap contexts. Structs are empty or have
one initial component. Nested owning arrays and existing nested references are
included. GetResult starts with one ready value and no pending operators.

## Observations

| Cases | Observed original behavior |
| --- | --- |
| Integer -1, Float -0, Name payload | Type and raw bits retained in result; scratch cleared |
| Owning/ref Array 12/21 completed result | Result is type 12 with a different array identity and copied contents |
| Owning/ref empty Structure 10/20 completed result | Result is type 10 with a different structure identity |
| Ordinary `CopyComponent(ref20/21, reference=0)` | Retains the reference tag and exact target identity; no allocation |
| `CopyComponent(owner10/12, reference=1)` | Produces reference20/21 to the same target; no allocation |
| Ref Structure containing ref20 or ref21 | Outer structure is detached, existing nested reference keeps its target |
| Ref Array containing an owning Array | Outer and child arrays are both detached; later source-child mutation is invisible |
| Ref Structure completed before later field changes | Result keeps the old owned scalar value and does not acquire the later field |

For GetResult, the first CopyComponent call receives owning type 10/12 even
when the evaluator's starting value is ref20/21, and `reference=0`. Before scratch
cleanup the source reference tag is restored. Reference inputs' targets are not
freed; owning inputs' targets are freed after copying. The harness does not
read or mutate a target after a recorded free.

These observations corroborate the reconstructed
[GetResult](../src/game/script/eval.cpp) and
[CopyComponent](../src/game/script/struct.cpp). Completing an expression and
ordinary component copying are distinct operations. Completion materializes
the outer referenced target, but does not recursively erase pre-existing nested
reference semantics.

## Current JS gap and required fix

The behavior below was reproduced at `12ea70c`. #29 implements an explicit
`expressionResult` IR boundary and an iterative owning copy helper, and replaces
the conflicting live-result expectation. It adds independent grouping/random,
snapshot/wait, nested-reference and depth/error tests to the public QuickJS
integration bundle. The original-parser exclusions in this evidence still apply.

A separate synthetic production reader/emitter → embedded QuickJS probe at this
base saves the all-parameters expression, then adds a new parameter. Its saved
value remains type 20 and observes the later field. This reproduces the behavior
also asserted by the existing `math2` fixture's “all-parameters live reference”
check in [emitter-fixtures.ts](../tools/qbjs/emitter-fixtures.ts). This result is
evidence of the current behavior, not an approved expected QB result.

The x86 probe does not run original ParseExpression or that complete QB script.
Combining its GetResult evidence with reconstructed ParseExpression's final
GetResult call supports #29's required completion boundary; it does not determine
the reader/IR boundary for every nested expression or random branch.

After reader/IR/emitter handoff from #22, #29 must:

- Represent final expression completion distinctly from individual operands and
  grouping. Keep intermediate borrow behavior for member/index operations.
- Materialize the top-level ref target as an owning copy at that boundary, while
  preserving nested reference semantics verified here.
- Replace the conflicting fixture expectation with independent completion tests,
  including values changed or fields added after completion, nested owners/refs,
  nested grouping and random expression branches.
- Preserve work/depth limits and error propagation. Continue rejecting 20/21 in
  the native by-value codec; codec unwrapping is not the expression fix.

The oracle is not a JS differential harness or a complete QB-VM validation.
It does not cover multi-element allocation, strings/coordinates, pending
operators, parser evaluation ordering, malformed expressions, game allocation
failures, graph cycles, long-term pointer safety or QuickJS GC/rooting. The
existing source claims and unsupported cases remain bounded accordingly.

## Reproduce locally

Use the project's Python venv with pefile and unicorn, keeping any missing
dependencies inside that venv. From this checkout:

```sh
mkdir -p work
/path/to/project/.venv/bin/python tools/verify_js_expression_result.py \
  --exe /path/to/installed/Game/THUG2PM.exe \
  --report work/expression-result-oracle.json
```

The report is optional, must be a new `.json` file, and should remain in ignored
`work/`. Existing files are never overwritten. A wrong EXE fingerprint fails
before emulation. On hosts where Unicorn cannot execute inside the sandbox,
run this explicitly authorized local probe outside that sandbox. This does not
require UTM or running the game executable as a Windows application.
