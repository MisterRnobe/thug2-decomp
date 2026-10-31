// NOT ORIGINAL - synthetic dynamic command-name fixtures (CScript::ReadName
// parameter forms). No game QB. emitter-fixtures.ts stays untouched: this
// module ships its own TEST-ONLY dispatcher for the execution bundle.

import { readFileSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { emitIR } from "./emitter.ts";
import { command, conditional, field, integer, name, not_, param, script } from "./fixtures.ts";
import { readIR } from "./reader.ts";
import { checksum } from "./translate.ts";

// CScript::ReadName's three name forms, as raw statement tokens.
export const dynParam = (parameter: string): number[] => [...param(parameter)]; // `2d 16 <crc>`
export const dynUnnamed = [0x2c]; // the unnamed checksum parameter
// A dynamic command statement: <source> [args] EOL.
export const dynamicCall = (source: number[], args: number[] = []): number[] => [...source, ...args, 1];
// A `name:command` call with any static/dynamic object/command combination.
export const dynamicQualified = (object: number[], commandSource: number[], args: number[] = []): number[] => [
    ...object,
    0x42,
    ...commandSource,
    ...args,
    1,
];
const assign = (key: string, rhs: number[]) => [...field(key, rhs), 1];
const ret = (key: string, rhs: number[]) => [0x29, ...field(key, rhs), 1];
const expr = (tokens: number[]) => [0x0e, ...tokens, 0x0f];

export function dynamicFixture(): Uint8Array {
    const cmdName = dynParam("cmdName"),
        objName = dynParam("objName");
    return Uint8Array.from([
        // A static callee that would be a correct dispatch target.
        ...script("dynHost2", ret("result", param("x")), field("x", integer(7))),
        // Plain dynamic command names: a named parameter and the unnamed spread.
        ...script("dynPlain2", [...dynamicCall(cmdName, field("x", integer(1))), ...assign("after", integer(2))]),
        ...script("dynUnnamed2", dynamicCall(dynUnnamed, field("x", integer(1)))),
        // The fault must fire before argument evaluation: the argument reads a
        // missing parameter and must never be reached.
        ...script("dynArgs2", dynamicCall(cmdName, field("x", param("missing")))),
        // A parameter holding a correct Name target still gets the fault.
        ...script("dynTarget2", dynamicCall(cmdName)),
        // All qualified combinations: dynamic object + static command, static
        // object + dynamic command, both dynamic.
        ...script("dynObj2", dynamicQualified(objName, name("Do"), field("x", integer(1)))),
        ...script("dynCmd2", dynamicQualified(name("obj"), cmdName)),
        ...script("dynBoth2", dynamicQualified(objName, dynUnnamed)),
        // Dynamic if conditions, plain and under a NOT prefix.
        ...script("dynCond2", conditional([...cmdName, 1], assign("taken", integer(1)), assign("taken", integer(0)))),
        ...script(
            "dynCondNot2",
            conditional([...not_, ...cmdName, 1], assign("taken", integer(1)), assign("taken", integer(0))),
        ),
        ...script("dynQualifiedCond2", conditional([...objName, 0x42, ...dynUnnamed, 1], command("Yes"), [])),
        // Dynamic calls in untaken branches and uninvoked bodies must not stop
        // loading, nor fault while the taken path runs.
        ...script("dynUntaken2", conditional(expr(integer(0)), dynamicCall(cmdName), assign("ok", integer(1)))),
        ...script("dynUninvoked2", dynamicCall(dynParam("never"))),
        ...script("dynStaticOk2", [...command("dynHost2"), ...assign("fine", integer(3))]),
        0,
    ]);
}

export const dynamicEmitted = (): string => emitIR(readIR(dynamicFixture(), "dynamic.qb"));

// A self-contained TEST-ONLY execution bundle (the emitter-fixtures.ts
// dispatcher stays untouched). Asserts the explicit unsupported fault fires
// on reaching a dynamic call, before argument evaluation or host dispatch.
export function dynamicExecutionBundle(valuesSource: string): string {
    const key = (s: string) => checksum(s);
    return `(() => {\nconst saved = globalThis.qb;\ntry {\n${valuesSource}\n
	const v = qbValues, scripts = new Map(), symbols = new Map();
	let fuel = 0, checks = 0, hostCalls = 0;
	const assert = (ok, why) => { ++checks; if (!ok) throw Error('Dynamic command test: ' + why); };
	const charge = n => { if ((fuel -= n) < 0) throw Error('test fuel exhausted'); };
	globalThis.qb = {
	 module(m) { assert(m.format === 2 && m.requires.values === 2, 'manifest'); },
	 context(site) { return {site, owner:1, symbol:n => symbols.get(n), charge}; },
	 step(site) { charge(1); },
	 define(n, fn) { scripts.set(n, fn); symbols.set(n, {kind:'callable', type:7, name:n}); },
	 defineGlobal(n, value) { symbols.set(n, value); }, frame() {},
	 *call(n, args, p, site) {
	   ++hostCalls;
	   const fn = scripts.get(n); if (!fn) throw Error('unsupported host');
	   const out = yield* fn(args, false); v.append(p, out, this.context(site)); return true;
	 }
	};
	fuel = 1000000;
	${dynamicEmitted()}
	const integer = n => v.integer(n >>> 0);
	const get = (p, key) => p.findLast(f => f[0] === key)?.[1];
	const run = (n, args = [], budget = 100000) => {
	 fuel = budget; const task = scripts.get(n)(args, true); let r;
	 do { r = task.next(); } while (!r.done); return r.value;
	};
	const failure = (n, text, args) => { let error; try { run(n, args); } catch (e) { error = e; }
	 assert(String(error).includes(text), 'expected fault ' + text + ', got ' + error); return error; };
	const dynamicArgs = [[${key("cmdName")}, {type:13, bits:${key("dynHost2")}}]];
	// Loading defined every function; a reached dynamic call faults honestly
	// with the original site, even when the parameter holds a correct target.
	const e = failure(${key("dynPlain2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	assert(String(e).includes('parameter 0x' + (${key("cmdName")}).toString(16)), 'fault names the source parameter CRC');
	assert(e.site?.source === 'dynamic.qb' && e.site?.script === ${key("dynPlain2")}, 'fault keeps the original site');
	// A parameter holding a correct Name target still gets the honest fault,
	// which aborts before later statements, before argument evaluation and
	// before any host dispatch.
	failure(${key("dynTarget2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	const before = hostCalls;
	failure(${key("dynArgs2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	assert(hostCalls === before, 'no host dispatch for a dynamic name');
	failure(${key("dynUnnamed2")}, 'QB dynamic command unnamed parameter requires dynamic command dispatch, which is not implemented');
	// Qualified combinations keep the object/command descriptor identity.
	const qe = failure(${key("dynObj2")}, 'requires dynamic command dispatch, which is not implemented', [[${key("objName")}, {type:13, bits:1}]]);
	assert(String(qe).includes('parameter 0x' + (${key("objName")}).toString(16) + ':0x' + (${key("Do")}).toString(16)), 'dynamic object + static command descriptors');
	const ce = failure(${key("dynCmd2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	assert(String(ce).includes('0x' + (${key("obj")}).toString(16) + ':parameter 0x' + (${key("cmdName")}).toString(16)), 'static object + dynamic command descriptors');
	const be = failure(${key("dynBoth2")}, 'requires dynamic command dispatch, which is not implemented', [[${key("objName")}, {type:13, bits:1}]]);
	assert(String(be).includes('parameter 0x' + (${key("objName")}).toString(16) + ':unnamed parameter'), 'both-dynamic descriptors');
	// Dynamic if conditions fault on their single evaluation, with a NOT
	// prefix too: no jump, no else part, no condition-argument side effects.
	const condError = failure(${key("dynCond2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	assert(condError.site?.script === ${key("dynCond2")}, 'condition fault keeps the site');
	failure(${key("dynCondNot2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	failure(${key("dynQualifiedCond2")}, 'requires dynamic command dispatch, which is not implemented', [[${key("objName")}, {type:13, bits:1}]]);
	// Untaken branches and uninvoked bodies never fault; static calls unchanged.
	p = run(${key("dynUntaken2")}, dynamicArgs);
	assert(get(p, ${key("ok")}).bits === 1, 'untaken dynamic branch does not fault');
	p = run(${key("dynStaticOk2")});
	assert(get(p, ${key("result")}).bits === 7 && get(p, ${key("fine")}).bits === 3, 'static calls still dispatch');
	// Defining it was enough; invoking it faults the same honest way.
	failure(${key("dynUninvoked2")}, 'requires dynamic command dispatch, which is not implemented', dynamicArgs);
	assert(checks >= 18, 'execution assertions ran');
	globalThis.qbIntegrationTestResult = {completed:true};
	} finally { globalThis.qb = saved; }\n})();\n`;
}

// Run with a QuickJS driver (QB_QUICKJS_DRIVER) to execute the assertions in
// the embedded interpreter; the Node suite runs the same bundle in a VM.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const output = process.argv[2];
    if (!output || process.argv.length !== 3)
        throw Error("Usage: node tools/qbjs/dynamic-command-fixtures.ts OUTPUT.js");
    writeFileSync(
        output,
        dynamicExecutionBundle(readFileSync(new URL("../../scripting/values.js", import.meta.url), "utf8")),
    );
}
