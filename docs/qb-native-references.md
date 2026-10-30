# Proposed native reference contract

Design for phase 2, issue #26, based on `c4d93e3`. This document specifies a
future opt-in interface; native references are **not implemented**. The existing
scalar ABI is unchanged. The acyclic rich by-value codec (#21), and its Runtime
integration (#24), must reject types 20/21 at any depth. They do not depend on
implementing this proposal.

## Evidence and compatibility boundary

The following is reconstructed source evidence, not an original-executable
oracle result. Addresses identify the reconstructed functions. No binary
experiment was run for this document.

| Operation | Source evidence | Consequence |
| --- | --- | --- |
| `CopyComponent`, `0x004773E0` | [struct.cpp](../src/game/script/struct.cpp): owning 10/12 deep-copy with `reference=0`, become 20/21 with `reference=1`; existing 20/21 copy their pointer | Ordinary copying of an existing reference does not detach it |
| `CleanUpComponent`, `0x004772A0` | Same file: owned structures/arrays are freed; reference tags fall through | Borrowing does not transfer deletion rights |
| `MakeReference`, `0x0046D180`; `MakeOwning`, `0x0046D1A0` | [eval.cpp](../src/game/script/eval.cpp): change the tag only | These are evaluator internals, not safe public ownership-transfer APIs |
| `ResolveNameComponent`, `0x00479730` | [parse.cpp](../src/game/script/parse.cpp): replace a resolved Name with symbol type/payload | Name resolution establishes no independent lifetime guarantee |
| Parameter, member and index operands | Same parser and evaluator: expression parameters are resolved then borrowed; container selection propagates the reference mode of its base | Intermediate operations can share a mutable target |
| `GetResult`, `0x0046EC40` | Evaluator: temporarily `MakeOwning`, `CopyComponent(..., 0)`, restore reference, clean scratch | A final reference result is copied as its target's owning value |
| `ComponentsEqual`, `0x004791A0` | `struct.cpp`: owning 10/12 compare target identity; reference 20/21 fall through to false | Native identity inspection must not replace QB equality |

[values.js](../scripting/values.js) models owning containers as JS objects and
references as wrappers with a strong `target` edge. Its value/reference copies
preserve existing reference targets; owning copies recursively copy owned
children. Its equality preserves reference-default-false behavior. Strong JS
edges are a port lifetime policy; the original raw-pointer code does not prove
that a target survives deletion of its owner.

### Known gap: completion of expressions

At this base, the emitter composes reference-producing value helpers without an
explicit outer `GetResult` materialization step. Therefore helper-level reference
behavior does not prove completed-expression compatibility. In particular, the
synthetic all-parameters case which saves `(<...>)`, adds a later parameter, and
expects that saved result to observe it needs correction or stronger evidence.
The reconstructed `GetResult` instead requests an owning copy at completion.

Implement and test this boundary separately before native live references. Do
not unwrap references in the codec to hide the mismatch. Do not deep-copy every
operand or nested grouping: those operations still need intermediate aliases.
Existing contract wording about references across suspension describes the JS
representation, not proof that every QB expression may export such a reference.
The exact reader/IR boundary, nested expressions and random expression branches
need focused tests; an original-x86 witness would strengthen this evidence.

## Selected interface policy

The rules below are new port decisions, not claims about the original ABI.
First implement callback-scoped live views only. Persistent C++ retention,
cross-callback handles, cross-task transfer, and asynchronous callbacks are
explicitly unsupported. A host that needs an independent value uses the owning
data API; attempting to retain a live view must fail rather than silently copy.
This narrow first version avoids making task lifetime an implicit host lease.

| Boundary operation | Ownership / identity | Valid lifetime and failure |
| --- | --- | --- |
| Existing scalar transport | Copy type and bits | Unchanged |
| Rich by-value input/output or snapshot | Detached owned data; no 20/21, including nested references | Independent of Runtime after successful conversion; cycles rejected |
| Future explicit live callback argument | Borrowed view of an existing target; no ownership transfer | Only during that synchronous callback invocation |
| Two live views of the same target | Same target identity in one Runtime; permissions and scopes remain separate | Each view must independently be valid |
| Read child container through a live view | Another scoped view of the existing child | Does not extend the parent scope |
| Read scalar/string/coordinate through a live view | Detached payload copy | Safe after callback return |
| Mutate through a live view | Changes that target; valid aliases observe changes | Prevalidate operation and value; reject expired/read-only views |
| Export live view as callback output/start parameter | Unsupported in the first live version | Explicit boundary error; no pointer or token in scalar `bits` |
| Retain or use between callbacks/tasks | Unsupported | Explicit expired/unsupported result; never touch QuickJS |
| Request a historical snapshot | Detached data only | Reference-containing state fails explicitly; never quietly returns a live handle |
| Inspect live task state outside a callback | Deferred separate API | No live behavior added to `snapshot()` |

No JS object address, original PC address or C++ pointer crosses this interface.
Do not serialize a handle as a public QB value. Proposed C++ views are move-only
opaque objects, constructible only by the bridge, with private Runtime identity,
scope identity, registry slot, generation and target kind. Moving a view does
not extend its scope. Borrowed reads are const; mutation requires an explicitly
writable view. Handle identity inspection is diagnostic/host functionality:
two references to one array can have the same target identity while QB equality
between their type-21 wrappers is still false.

Runtime identity is unique for the process lifetime; generation and scope
counters never wrap into a previously valid identity. Exhaustion is an explicit
error. Reusing a slot increments its generation. Address reuse after destruction
does not make an old handle valid. Passing a view to another Runtime is rejected
before a QuickJS call, even if its target kind and slot happen to match.

## Roots, invalidation and execution

The native registry roots each exposed target with `JS_DupValue`; roots are
released exactly once with the originating live context. Target identity is
deduplicated within a scope using actual JS object identity, not structural
equality. Multiple aliases can share a registry entry. Do not infer ownership
from the type of a wrapper or expose `MakeOwning` as a mutation.

The callback scope has a native RAII cleanup guard. Every exit invalidates its
views and frees its roots, including a thrown host exception, a caught JS error,
output conversion failure and an uncatchable engine failure. JS `finally` alone
is insufficient. Invalidation precedes releasing roots. An escaped C++ view
contains no owning `JSValue` or raw `JSContext*`; it can only consult a shared
inert validity control block after the Runtime dies.

On task completion, fault or cancellation there are no surviving callback
views. Cancellation of a suspended task cannot be followed by observation through
an earlier callback's view. Global replacement/unload outside a callback likewise
has no valid outstanding views. During a callback the normal Runtime entry guard
rejects cancellation, unload, nested update/start and other reentrant Runtime
operations. No asynchronous host suspension is introduced. A later callback may
receive the same underlying global target through a *new* scope, but an old view
stays invalid.

The Runtime is confined to its owning thread. Read/write view operations are
allowed only inside the active bridge callback, using internal checked bridge
operations; they do not recursively enter the public Runtime API. A callback
must not destroy its executing Runtime. Destruction occurs after the active call
unwinds. Teardown invalidates all control blocks and drains roots before
`JS_FreeContext`, then frees the QuickJS runtime. Wrong-thread access fails before
QuickJS access; C++ object races themselves are not supported.

Persistent retention would require a separate reviewed lease API with explicit
task/global/runtime ownership, revocation, rooting quotas and owning-thread
access scheduling. Storing a callback view in a C++ member is not that API.

## Graphs, mutation and resource limits

Owning deep-copy cycles are rejected. Repeated owned subgraphs in a by-value
conversion are copied independently; do not promise preserved graph identity.
References must never be replaced with a hidden deep copy. A live traversal
obtains child views and preserves target identity without recursively exporting
the graph. Existing JS reference cycles may remain strongly reachable in JS;
they are not recursively validated by merely registering their root.

The first live API may inspect existing cycles, but may not create reference
edges, replace a target or import a native graph. Consequently a read of a
back-edge returns the same target identity, not an endlessly expanded tree.
Any requested detached export rejects reference tags; an owning-only recursive
copy uses an active-path cycle check. Shared nodes are not mistaken for cycles.

Reuse value-helper limits (depth 128 and total work 250,000 per operation),
including traversal and validation work, and impose proposed initial limits of
4,096 distinct rooted targets and 4,096 issued views per callback. Deduplicated
targets still charge each issued view. These are conservative new limits, not
measured engine maxima. Native handle allocation is also charged to the Runtime
memory budget; JS memory accounting alone does not bound native vectors.
Budgets reset only at a new top-level operation/scope, not at recursive helpers.

For mutation, build/validate the replacement before modifying the destination;
each field/element operation either commits fully or leaves that destination
unchanged. Earlier successful mutations and external host side effects are not
rolled back when a later operation fails. No raw mutable target/payload escapes.
Appending/removing children cannot invalidate the root view, but fresh child
lookups observe the current container. Already-issued child views keep that
particular child rooted until scope exit, even if removed from the parent.

## Failure behavior

| Failure | Required outcome |
| --- | --- |
| Invalid tag/kind, stale scope/generation, wrong Runtime or thread | Reject before dereferencing a target; no mutation |
| Partial conversion/root registration or quota failure | Discard partial result, release acquired roots, preserve existing data |
| Host exception or ordinary task fault | End callback scope, invalidate views, apply existing task error policy |
| Uncatchable interrupt, host allocation failure, fatal output marshaling failure | Native scope cleanup still runs; preserve existing terminal Runtime policy |
| Runtime teardown | Invalidate first, release roots while context lives; later view destruction/access never calls QuickJS |
| Mutation validation failure | No partial destination mutation; previously committed operations remain |

Do not translate unsupported references, expired views or allocation failures
into success, empty containers or missing parameters. Error strings may identify
the operation and path; they must not include raw pointers. Error categories and
their C++ surface remain implementation decisions, consistent with the existing
Runtime fatal/recoverable distinction.

## Independent acceptance scenarios

These are future executable requirements, not tests already passing. Use
synthetic data and the embedded QuickJS runner; include native allocation/error
injection and sanitizer checks for lifetime paths.

1. **Two aliases:** array A contains Integer 7. Two type-21 wrappers target A.
   In one live callback both views report the same target identity. Set element
   zero through one to 9; the other reads 9. QB `ComponentsEqual` on the wrappers
   remains false. After return both views reject access.
2. **Owning copy:** owning array A contains 7. `CopyComponent(reference=0)`
   produces B with different identity. Mutating B to 9 leaves A at 7. Completed
   expression finalization of a reference to A likewise produces owning B; an
   intermediate borrowed operand must still target A.
3. **Nested reference:** owning structure S contains a type-21 reference to A.
   Ordinary component copy of S preserves that nested reference target. The
   by-value codec rejects the exact nested path, with no partial native result.
   The explicit live interface can navigate S to A without detaching it.
4. **Across wait/cancel:** a callback saves a view, returns, then the task waits
   and is cancelled. Access rejects already at callback return and still rejects
   after cancel. A rooted JS target elsewhere does not restore native rights.
5. **Across callbacks/tasks:** callback/task B receives the same global array
   that callback A previously observed. B reads current data with a fresh view;
   A's saved view remains expired. No cross-task retention is inferred.
6. **Runtime destruction/address reuse:** keep an expired view, destroy Runtime
   R, construct R2, then destroy/use the saved view. No use-after-free and no
   QuickJS call; R2 never accepts R's view.
7. **Wrong Runtime:** pass an otherwise active view from R to R2's checked view
   operation. Reject before reading R2's registry; neither target changes.
8. **Cycles and aliases:** an existing JS reference graph A to B to A can be
   walked by bounded live reads, returning A's original identity at the back-edge.
   Detached export rejects references. An owning cycle fails the path check;
   an acyclic repeated child copies successfully as independent owned children.
9. **Failure cleanup:** inject failure at every root allocation and replacement
   construction, throw from the host, and trigger an uncatchable interrupt.
   Scope root count returns to baseline, views expire, and teardown frees no
   value after its context. Repeated iterations do not accumulate native roots.
10. **Removed child and quotas:** obtain child C, remove C from its parent,
    then read C within the callback successfully. After scope exit it expires.
    The 4,097th issued view fails even when all views alias one target; the
    failing operation leaks no root and does not invalidate earlier valid views.

## Implementation gates

Review this design before implementing live views. First resolve expression
finalization with focused reader/IR/emitter and QuickJS fixtures; treat original
binary evidence as optional corroboration, not a prerequisite to recording the
source mismatch. Integrate and accept the acyclic codec/API independently.
Then a separate blocked implementation issue can cover the explicit live API,
native registry, error injection and scenarios above. Persistent retention,
live snapshots and native reference outputs remain out of scope until their own
contract is reviewed. This document does not expand the advertised supported
QB subset or certify the game's complete reference behavior.
