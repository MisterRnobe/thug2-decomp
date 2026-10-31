// NOT ORIGINAL - synthetic format-2 execution fixtures and TEST-ONLY dispatcher.
// This adapter exercises generated generators; it is not the product scheduler
// or C++ bridge. values.js is the real compatibility implementation.

import { readFileSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { emitIR } from "./emitter.ts";
import {
    command,
    conditional,
    field,
    fixture,
    floating,
    integer,
    name,
    not_,
    param,
    qualified,
    script,
} from "./fixtures.ts";
import { readIR } from "./reader.ts";
import { checksum, translate } from "./translate.ts";

const expr = (...parts: number[][]) => [14, ...parts.flat(), 15];
const assign = (key: string, rhs: number[]) => [...field(key, rhs), 1];
const repeat = (body: number[], count: number[] = []) => [0x20, 1, ...body, 0x21, ...count, 1];
const ret = (key: string, rhs: number[]) => [0x29, ...field(key, rhs), 1];
const conditionalNot = (condition: number[], yes: number[], no: number[]): number[] =>
    conditional([...not_, ...condition], yes, no);
const choose = (selector: number[], cases: [number[], number[]][], fallback?: number[]) => {
    const b = [0x3c, ...selector, 1],
        exits: number[] = [];
    let previous: number | undefined;
    const jump = (at: number, target: number) => {
        b[at] = (target - at) & 255;
        b[at + 1] = (target - at) >>> 8;
    };
    for (const [label, body] of cases) {
        const start = b.length;
        if (previous !== undefined) jump(previous, start);
        b.push(0x3e, 0x49, 0, 0, ...label, 1, ...body, 0x49, 0, 0);
        previous = start + 2;
        exits.push(b.length - 2);
    }
    if (previous !== undefined) jump(previous, b.length);
    if (fallback !== undefined) {
        b.push(0x3f, 0x49, 0, 0);
        exits.push(b.length - 2);
        b.push(...fallback);
    }
    const end = b.length;
    b.push(0x3d, 1);
    for (const at of exits) jump(at, end);
    return b;
};
export const emittedFixture = () => {
    const data = [
        ...assign("global", integer(5)),
        ...assign("alias", name("global")),
        ...assign("arr", [5, ...integer(3), ...floating(0x40800000), 6]),
        ...script("math2", [
            ...assign("result", expr(integer(8), [12], integer(4), [13], integer(2))),
            ...assign("globalResult", expr(name("alias"), [11], integer(2))),
            ...assign("arrayResult", expr(name("arr"), [5], integer(1), [6])),
            ...assign("s", [3, ...field("member", integer(9)), 4]),
            ...assign("memberResult", expr(param("s"), [8], name("member"))),
            ...assign("all", expr([0x2c])),
            ...assign("late", integer(77)),
            ...assign("live", expr(param("all"), [8], name("late"))),
        ]),
        ...script(
            "count2",
            repeat(assign("n", expr(param("n"), [11], integer(1))), integer(3)),
            field("n", integer(0)),
        ),
        // Count is read after the first body visit only. It must stay at 3
        // even as 'limit' grows on subsequent iterations.
        ...script(
            "once2",
            repeat(
                [
                    ...assign("limit", expr(param("limit"), [11], integer(1))),
                    ...assign("n", expr(param("n"), [11], integer(1))),
                ],
                param("limit"),
            ),
            [...field("limit", integer(2)), ...field("n", integer(0))],
        ),
        ...script(
            "wrap2",
            repeat(
                [
                    ...assign("n", expr(param("n"), [11], integer(1))),
                    ...conditional(expr(param("n"), [7], integer(2)), [0x22, 1], []),
                ],
                integer(0),
            ),
            field("n", integer(0)),
        ),
        ...script(
            "unbounded2",
            repeat(
                [
                    ...assign("n", expr(param("n"), [11], integer(1))),
                    ...conditional(expr(param("n"), [7], integer(2)), [0x22, 1], []),
                ],
                [...integer(1), ...floating(0x40400000)],
            ),
            field("n", integer(0)),
        ),
        ...script("breakParams2", repeat([0x22, 1], expr(param("missing"), [11], integer(1)))),
        ...script("fuel2", repeat([], integer(-1))),
        ...script(
            "switch2",
            choose(
                param("x"),
                [
                    [integer(1), []],
                    [param("label"), []],
                    [integer(3), assign("result", integer(9))],
                    [integer(4), assign("result", integer(10))],
                ],
                assign("result", integer(99)),
            ),
        ),
        ...script(
            "groupFault2",
            choose(integer(1), [
                [integer(1), []],
                [expr(param("missing"), [11], integer(1)), assign("result", integer(9))],
            ]),
        ),
        ...script("switchBreak2", repeat(choose(integer(1), [[integer(1), [0x22, 1]]], assign("wrong", integer(1))))),
        ...script("callee2", ret("result", expr(param("x"), [11], integer(2))), field("x", integer(7))),
        ...script("caller2", [...command("callee2"), ...command("Pause2"), ...assign("done", integer(1))]),
        ...script("missing2", assign("result", expr(param("missing"), [11], integer(1)))),
        ...script("shortCircuit2", assign("result", expr(integer(0), [51], expr(param("missing"), [11], integer(1))))),
        ...script("untaken2", [
            ...conditional(
                expr(integer(0)),
                assign("bad", expr(param("missing"), [11], integer(1))),
                assign("result", integer(11)),
            ),
            ...choose(integer(1), [
                [integer(1), assign("selected", integer(1))],
                [expr(param("missing"), [11], integer(1)), assign("bad", integer(1))],
            ]),
        ]),
        ...script("literals2", [
            ...assign("bytes", [0x1b, 4, 0, 0, 0, 0xff, 0x80, 0, 65]),
            ...assign("local", [0x1c, 2, 0, 0, 0, 65, 0]),
            ...assign("pair", [0x1f, 0, 0, 0, 0x80, 0, 0, 0x80, 0x3f]),
            ...assign("vector", [0x1e, 0, 0, 0, 0, 0, 0, 0x80, 0x3f, 0, 0, 0, 0x40]),
        ]),
        // `if not`: the condition is evaluated exactly once, then negated.
        // A QB-script condition counts as true before negation, so the else
        // part runs while the callee's effects still happen.
        ...script("notHost2", [
            ...conditionalNot(name("Flip2"), assign("taken", integer(1)), assign("taken", integer(0))),
        ]),
        ...script("notScript2", [
            ...conditionalNot(name("callee2"), assign("taken", integer(1)), assign("taken", integer(0))),
        ]),
        ...script("notExpr2", [
            // `not (0)`: zero raw bits are false, so negation takes the yes
            // part. A separate script checks a nonzero condition under `not`.
            ...conditionalNot(expr(integer(0)), assign("taken", integer(1)), assign("taken", integer(0))),
        ]),
        ...script("notExprTrue2", [
            ...conditionalNot(expr(integer(5)), assign("taken", integer(1)), assign("taken", integer(0))),
        ]),
        // A faulting condition (absent parameter operand) aborts the if (no
        // jump) even under `not`, mirroring ExecuteIf's `if (!m_pc) return`.
        ...script("notFault2", [
            ...conditionalNot(
                expr(param("missing"), [11], integer(1)),
                [...assign("bad", integer(1))],
                [...assign("worse", integer(1))],
            ),
        ]),
        // Object-qualified statements and if conditions are preserved in IR and
        // fault at execution instead of dispatching a global call.
        ...script("qualified2", [...qualified("obj", "Do", field("x", integer(1))), ...assign("after", integer(2))]),
        ...script("qualifiedCond2", [
            ...conditional([...name("obj"), 0x42, ...name("Check")], assign("taken", integer(1)), []),
        ]),
        0,
    ];
    return emitIR(readIR(Uint8Array.from(data), "emitter.qb"));
};

export function executionBundle(valuesSource: string): string {
    const key = (s: string) => checksum(s);
    return `(() => {\nconst saved = globalThis.qb;\ntry {\n${valuesSource}\n
const v = qbValues, scripts = new Map(), symbols = new Map();
let fuel = 0, latest, checks = 0, flipCalls = 0;
const assert = (ok, why) => { ++checks; if (!ok) throw Error('Emitter test: ' + why); };
const charge = n => { if ((fuel -= n) < 0) throw Error('test fuel exhausted'); };
globalThis.qb = {
 module(m) { assert(m.format === 2 && m.requires.values === 2, 'manifest'); },
 context(site) { return {site, owner:1, symbol:n => symbols.get(n), charge}; },
 step(site) { latest = site; charge(1); },
 define(n, fn) { scripts.set(n, fn); symbols.set(n, {kind:'callable', type:7, name:n}); },
 defineGlobal(n, value) { symbols.set(n, value); }, frame() {},
 *call(n, args, p, site) {
   if (n === ${key("Flip2")}) { ++flipCalls; return false; }
   if (n === ${key("Pause2")}) { yield {frames:1}; return true; }
   const fn = scripts.get(n); if (!fn) throw Error('unsupported host');
   const out = yield* fn(args, false); v.append(p, out, this.context(site)); return true;
 }
};
fuel = 1000000;
${emittedFixture()}
const integer = n => v.integer(n >>> 0);
const get = (p, key) => p.findLast(f => f[0] === key)?.[1];
const run = (n, args = [], budget = 100000) => {
 fuel = budget; const task = scripts.get(n)(args, true); let r;
 do { r = task.next(); } while (!r.done); return r.value;
};
const failure = (n, text) => { let error; try { run(n); } catch (e) { error=e; }
 assert(String(error).includes(text), 'expected fault ' + text + ', got ' + error); return error; };
let p = run(${key("math2")});
assert(get(p, ${key("result")}).bits === 1, 'original division/multiplication priority');
assert(get(p, ${key("globalResult")}).bits === 7, 'global alias');
assert(get(p, ${key("arrayResult")}).type === 2 && get(p, ${key("arrayResult")}).bits === 0x40800000, 'promoted float array');
assert(get(p, ${key("memberResult")}).bits === 9, 'member selection');
assert(get(p, ${key("all")}).type === 10, 'GetResult materializes all parameters');
assert(get(p, ${key("live")}) === undefined && get(get(p, ${key("all")}).members, ${key("late")}) === undefined, 'completed all-parameters result omits later field');
assert(get(run(${key("count2")}), ${key("n")}).bits === 3, 'counted loop');
p = run(${key("once2")});
assert(get(p, ${key("n")}).bits === 3 && get(p, ${key("limit")}).bits === 5, 'repeat count read once after first iteration');
assert(get(run(${key("wrap2")}), ${key("n")}).bits === 2, 'repeat zero wraps until break');
assert(get(run(${key("unbounded2")}), ${key("n")}).bits === 2, 'typed merging removes Integer count, loop runs until break');
failure(${key("breakParams2")}, 'Absent expression operand');
failure(${key("fuel2")}, 'fuel exhausted');
for (const [input, expected] of [[1,9],[2,9],[3,9],[4,10],[8,99]]) {
 p=run(${key("switch2")}, [[${key("x")},integer(input)],[${key("label")},integer(2)]]);
 assert(get(p, ${key("result")}).bits === expected, 'grouped switch ' + input);
}

assert(get(run(${key("switch2")}), ${key("result")}).bits === 99, 'absent selector defaults');
failure(${key("groupFault2")}, 'Absent expression operand');
assert(get(run(${key("switchBreak2")}), ${key("wrong")}) === undefined, 'break inside switch targets loop');
fuel=100000; const task=scripts.get(${key("caller2")})([],true); let state=task.next();
assert(!state.done && state.value.frames===1, 'generator yields at call'); state=task.next();
assert(state.done && get(state.value,${key("result")}).bits===9 && get(state.value,${key("done")}).bits===1, 'callee defaults/returns and resume');
const error=failure(${key("missing2")}, 'Absent expression operand');
assert(error.site?.source==='emitter.qb' && error.site.script===${key("missing2")}, 'original source/CRC in error');
failure(${key("shortCircuit2")}, 'Absent expression operand');
// \`if not\` with a host condition: exactly one evaluation, then negation.
let np = run(${key("notHost2")});
assert(flipCalls === 1, 'not condition evaluated exactly once');
assert(get(np, ${key("taken")}).bits === 1, 'negated false host condition takes the yes part');
np = run(${key("notHost2")});
assert(flipCalls === 2 && get(np, ${key("taken")}).bits === 1, 'each run evaluates once more');
// \`if not <script call>\`: the call counts as true before negation, so the
// else part runs while the callee's effects are still applied.
np = run(${key("notScript2")});
assert(get(np, ${key("taken")}).bits === 0, 'negated script-call condition jumps to else');
assert(get(np, ${key("result")}).bits === 9, 'callee effects preserved under not');
// \`if not <expression>\`: zero raw bits are false, so negation takes the yes
// part; a nonzero condition is negated into the else part.
np = run(${key("notExpr2")});
assert(get(np, ${key("taken")}).bits === 1, 'negated zero expression is true');
np = run(${key("notExprTrue2")});
assert(get(np, ${key("taken")}).bits === 0, 'negated nonzero expression is false');
// A faulting condition aborts the if under \`not\` (CScript::ExecuteIf's
// \`if (!m_pc) return;\`): no jump, no else part.
const notError = failure(${key("notFault2")}, 'Absent expression operand');
assert(notError.site?.script === ${key("notFault2")}, 'not fault keeps original site');
// Object-qualified statements fault explicitly; they never dispatch a global
// call and never return a stub success.
const qualifiedError = failure(${key("qualified2")}, 'object-qualified command');
assert(String(qualifiedError).includes('0x' + (${key("obj")}).toString(16)), 'fault names the object CRC');
assert(qualifiedError.site?.script === ${key("qualified2")}, 'qualified fault keeps original site');
const condError = failure(${key("qualifiedCond2")}, 'object-qualified command');
assert(condError.site?.source === 'emitter.qb', 'qualified condition fault keeps source');
assert(checks >= 23, 'execution assertions ran');
} finally { globalThis.qb = saved; }\n})();\n`;
}

// Run with qb-js-tests to execute assertions in actual QuickJS, then leave the
// original format-1 regression fixtures registered for its C++ bridge checks.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const output = process.argv[2];
    if (!output || process.argv.length !== 3) throw Error("Usage: node tools/qbjs/emitter-fixtures.ts OUTPUT.js");
    writeFileSync(
        output,
        executionBundle(readFileSync(new URL("../../scripting/values.js", import.meta.url), "utf8")) +
            translate(fixture(), "synthetic.qb"),
    );
}
