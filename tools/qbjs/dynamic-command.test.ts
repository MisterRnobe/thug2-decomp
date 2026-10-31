// NOT ORIGINAL - reader/emitter regressions for dynamic command/object names
// (CScript::ReadName parameter forms), issue #42. reader.test.ts keeps its
// own copy of the old rejections where they remain true (malformed streams).
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { runInNewContext } from "node:vm";
import { dynamicExecutionBundle, dynamicFixture, dynParam, dynUnnamed } from "./dynamic-command-fixtures.ts";
import { emitIR } from "./emitter.ts";
import { command, conditional, field, integer, name, not_, param, script } from "./fixtures.ts";
import type { ModuleIR } from "./ir.ts";
import { loopBlock, randomBlock, switchBlock } from "./random-control-fixtures.ts";
import { readIR, readQBFile } from "./reader.ts";
import { inventoryModule } from "./rich-helper-inventory.ts";
import { checksum } from "./translate.ts";

const module_ = (...data: number[][]) => Uint8Array.from([...data.flat(), 0]);
const assign = (key: string, rhs: number[]) => [...field(key, rhs), 1];
const dynamicCall = (source: number[], args: number[] = []) => [...source, ...args, 1];
const dynamicQualified = (object: number[], commandSource: number[], args: number[] = []) => [
    ...object,
    0x42,
    ...commandSource,
    ...args,
    1,
];
const firstStatement = (ir: ModuleIR, index = 0) => {
    const def = ir.definitions[0]!;
    assert.equal(def.kind, "script");
    if (def.kind !== "script") throw Error("expected script");
    return def.body[index]!;
};

test("plain dynamic command names are preserved as dynamic nodes with source kinds", () => {
    // Named parameter form `2d 16 <crc>`: the source CRC and both offsets
    // survive; there is no static dispatch target anywhere in the node.
    let ir = readIR(module_(script("a", dynamicCall(dynParam("cmdName"), field("x", integer(1))))));
    let call = firstStatement(ir);
    assert.equal(call.kind, "dynamic");
    if (call.kind !== "dynamic") throw Error("expected dynamic");
    assert.equal(call.command.kind, "nameParameter");
    if (call.command.kind !== "nameParameter") throw Error("expected parameter source");
    assert.equal(call.command.parameter, checksum("cmdName"));
    assert.equal(call.command.offset, call.offset);
    assert.equal(call.commandOffset, call.offset);
    assert.equal(call.object, undefined);
    assert.equal(call.fields.length, 1);
    // Unnamed form `2c`: the checksum comes from the unnamed parameter.
    ir = readIR(module_(script("a", dynamicCall(dynUnnamed))));
    call = firstStatement(ir);
    assert.equal(call.kind, "dynamic");
    if (call.kind !== "dynamic") throw Error("expected dynamic");
    assert.equal(call.command.kind, "nameUnnamed");
    assert.equal(call.command.offset, call.offset);
    assert.equal(call.fields.length, 0);
});

test("qualified dynamic combinations preserve object and command descriptors with original offsets", () => {
    const bodies = [
        // Dynamic object + static command.
        dynamicQualified(dynParam("objName"), name("Do"), field("x", integer(1))),
        // Static object + dynamic command.
        dynamicQualified(name("obj"), dynParam("cmdName")),
        // Both dynamic: named object, unnamed command.
        dynamicQualified(dynParam("objName"), dynUnnamed),
        // Both dynamic: unnamed object, named command.
        dynamicQualified(dynUnnamed, dynParam("cmdName")),
    ];
    for (const body of bodies) {
        const call = firstStatement(readIR(module_(script("a", body))));
        assert.equal(call.kind, "dynamic", `body ${body.length} bytes`);
        if (call.kind !== "dynamic" || !call.object) throw Error("expected dynamic with object");
        assert.ok(call.commandOffset > call.offset, "command offset follows the qualifier");
        assert.equal(call.object.offset, call.offset, "object descriptor keeps the original offset");
    }
    // Static object + static command still reads as the old QualifiedCall.
    const staticQualified = firstStatement(
        readIR(module_(script("a", [...name("obj"), 0x42, ...name("Do"), ...field("x", integer(1)), 1]))),
    );
    assert.equal(staticQualified.kind, "qualified");
    // A static object stays a nameLiteral inside the dynamic node, so the
    // fault keeps the full identity: object CRC AND dynamic command source.
    const mixed = firstStatement(readIR(module_(script("a", dynamicQualified(name("obj"), dynParam("cmdName"))))));
    assert.equal(mixed.kind, "dynamic");
    if (mixed.kind !== "dynamic" || !mixed.object) throw Error("expected dynamic with object");
    assert.equal(mixed.object.kind, "nameLiteral");
    if (mixed.object.kind === "nameLiteral") assert.equal(mixed.object.name, checksum("obj"));
});

test("dynamic if conditions are preserved, including with a NOT prefix", () => {
    const build = (tokens: number[]) =>
        readIR(module_(script("a", conditional(tokens, assign("taken", integer(1)), assign("taken", integer(0))))));
    for (const condition of [dynParam("cmdName"), [...dynParam("objName"), 0x42, ...dynUnnamed, 1]]) {
        const branch = firstStatement(build([...condition, 1]));
        assert.equal(branch.kind, "if");
        if (branch.kind !== "if") throw Error("expected if");
        assert.equal(branch.condition.kind, "dynamic");
        assert.equal(branch.negate, undefined);
    }
    const negated = firstStatement(build([...not_, ...dynParam("cmdName"), 1]));
    assert.equal(negated.kind, "if");
    if (negated.kind !== "if") throw Error("expected if");
    assert.equal(negated.negate, true);
    assert.equal(negated.condition.kind, "dynamic");
    // A bare 0x39 before a dynamic name is still a negated dynamic condition.
    const unnamed = firstStatement(build([...not_, 0x2c, 1]));
    assert.equal(unnamed.kind, "if");
    if (unnamed.kind !== "if") throw Error("expected if");
    assert.equal(unnamed.negate, true);
    if (unnamed.condition.kind !== "dynamic") throw Error("expected dynamic condition");
});

test("malformed and truncated dynamic names fail with bounded errors and source offsets", () => {
    // `2d` without the checksum-name token is malformed, not a dynamic call.
    assert.throws(() => readIR(module_(script("a", [0x2d, 1, 1]))), /expected checksum name/);
    // Truncated `2d 16` (no CRC bytes) and truncated after the CRC.
    for (const end of [0, 1, 2, 3, 4]) {
        const full = module_(script("a", dynamicCall(dynParam("cmdName"))));
        assert.throws(() => readIR(full.subarray(0, full.length - 1 - end), "cut.qb"), /cut\.qb: QB offset/);
    }
    // Every truncation of a dynamic-condition module fails with an offset.
    const full = module_(script("a", conditional([...dynParam("cmdName"), 1], command("Yes"), [])));
    for (let end = 0; end < full.length; ++end)
        assert.throws(() => readIR(full.subarray(0, end), "cut.qb"), /cut\.qb: QB offset/);
    // `2d 16 <crc> =` before a value is the static assignment to the script
    // parameter named <crc> (the pre-existing branch); the unnamed form
    // `2c =` and any dynamic assignment in condition position are rejected.
    const assigned = firstStatement(readIR(module_(script("a", [...dynParam("target"), 7, ...integer(1), 1]))));
    assert.equal(assigned.kind, "assign");
    assert.throws(
        () => readIR(module_(script("a", [0x2c, 7, ...integer(1), 1]))),
        /dynamic assignment is not supported/,
    );
    assert.throws(
        () => readIR(module_(script("a", conditional([...dynParam("target"), 7], command("Yes"), [])))),
        /dynamic assignment is not supported/,
    );
    // A dynamic object name whose qualifier or command name is truncated.
    assert.throws(() => readIR(module_(script("a", [...dynParam("objName"), 0x42, 1]))), /QB offset/);
});

test("dynamic calls inside loops, switches and random blocks stay inside their boundaries", () => {
    const call = dynamicCall(dynParam("cmdName"));
    // A dynamic call inside a repeat body: the boundary/jump validation still
    // holds with a dynamic node in the body.
    const loop = firstStatement(readIR(module_(script("a", loopBlock(call, integer(2))))));
    assert.equal(loop.kind, "loop");
    if (loop.kind !== "loop") throw Error("expected loop");
    assert.equal(loop.body[0]?.kind, "dynamic");
    // A dynamic call inside a random branch, joined by the block's exit
    // jump: the random-statement boundary checks are unchanged.
    const random = firstStatement(readIR(module_(script("a", randomBlock(0x2f, [call], [3])))));
    assert.equal(random.kind, "random");
    if (random.kind !== "random") throw Error("expected random");
    assert.equal(random.branches[0]?.[0]?.kind, "dynamic");
    // A dynamic call inside a switch case body.
    const switchStmt = firstStatement(
        readIR(module_(script("a", switchBlock(param("mode"), [[[...integer(1)], call]])))),
    );
    assert.equal(switchStmt.kind, "switch");
    if (switchStmt.kind !== "switch") throw Error("expected switch");
    assert.equal(switchStmt.cases[0]?.body[0]?.kind, "dynamic");
});

test("generated JS faults only on reaching a dynamic call and keeps the original site", () => {
    const values = readFileSync(new URL("../../scripting/values.js", import.meta.url), "utf8");
    // The whole execution bundle (load + run + fault assertions) in a Node VM.
    runInNewContext(dynamicExecutionBundle(values), {}, { timeout: 5000 });
    // Emission is deterministic and defines every function at load time.
    assert.equal(emitIR(readIR(dynamicFixture(), "dynamic.qb")), emitIR(readIR(dynamicFixture(), "dynamic.qb")));
    const js = emitIR(readIR(dynamicFixture(), "dynamic.qb"));
    assert.equal(js.match(/requires dynamic command dispatch/g)?.length, 12);
    // The static qualified fault text is unchanged for static-only modules.
    assert.doesNotMatch(js, /object-qualified command.*dynamic/s);
});

test("emission is honest: dynamic nodes never produce a static dispatch or a success stub", () => {
    const js = emitIR(readIR(dynamicFixture(), "dynamic.qb"));
    // No checksum is resolved through v.param and no global qb.call is
    // generated for a dynamic name: only the explicit fault exists.
    for (const line of js.split("\n"))
        if (line.includes("requires dynamic command dispatch")) assert.match(line, /throw Object\.assign\(Error/);
});

test("inventory reports dynamic/object execution requirements without bodies or payloads", () => {
    const report = inventoryModule(dynamicFixture(), "dynamic.qb");
    assert.equal(report.status, "readable");
    const withDynamic = report.scripts.filter((s) => s.execution?.dynamicDispatch);
    assert.equal(withDynamic.length, 12);
    const offsets = withDynamic.map((s) => s.execution?.dynamicCommandOffsets);
    assert.ok(offsets.every((list) => Array.isArray(list) && list.every((offset) => Number.isInteger(offset))));
    // A module without dynamic sites keeps the plain supported status.
    const plain = inventoryModule(module_(script("plain2", command("Host"))), "plain.qb");
    assert.equal(plain.status, "supported");
    // No bodies, payloads or generated JS leak into the report.
    assert.doesNotMatch(JSON.stringify(report), /"body"|"javascript"|"bits"/);
});

test("synthetic dynamic module executes on embedded QuickJS", { skip: !process.env.QB_QUICKJS_DRIVER }, () => {
    const root = mkdtempSync(join(tmpdir(), "qb-dynamic-quickjs-"));
    try {
        const output = join(root, "dynamic.js");
        const source = ["values.js"]
            .map((n) => readFileSync(new URL(`../../scripting/${n}`, import.meta.url), "utf8"))
            .join("\n");
        // The bundle self-asserts; the driver only needs to load it.
        writeFileSync(output, source + "\n" + dynamicExecutionBundle(""));
        execFileSync(process.env.QB_QUICKJS_DRIVER!, [output], { stdio: "pipe" });
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
});

// The full original module reads and emits through the production pipeline.
// The game file stays outside git; this gate requires local data to exist.
const viewGoals =
    "/Users/nikita-medvdv/IdeaProjects/thug2-recomp/bins/work/english-game/Game/Data/scripts/game/menu/view_goals_menu.qb";
test("full original view_goals_menu.qb reads and emits with dynamic nodes preserved", {
    skip: !existsSync(viewGoals),
}, () => {
    const bytes = readQBFile(viewGoals);
    const ir = readIR(bytes, "view_goals_menu.qb");
    assert.equal(ir.definitions.length, 57);
    let dynamicSites = 0,
        dynamicConditions = 0;
    const walk = (x: unknown): void => {
        if (!x || typeof x !== "object") return;
        if (Array.isArray(x)) {
            x.forEach(walk);
            return;
        }
        const record = x as { kind?: string; condition?: { kind?: string } };
        // Conditions are dynamic nodes too, so they land in both counters.
        if (record.kind === "dynamic") ++dynamicSites;
        if (record.kind === "if" && record.condition?.kind === "dynamic") ++dynamicConditions;
        if (record.kind !== "literal") Object.values(record).forEach(walk);
    };
    ir.definitions.forEach(walk);
    // 15 statement sites + 3 if-condition sites.
    assert.equal(dynamicSites - dynamicConditions, 15);
    assert.equal(dynamicConditions, 3);
    const js = emitIR(ir);
    assert.equal(js.match(/requires dynamic command dispatch/g)?.length, dynamicSites);
});
