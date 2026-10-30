// NOT ORIGINAL - synthetic multi-statement random control-flow fixtures. No game QB.
import { writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { emitIR } from "./emitter.ts";
import { command, conditional, field, integer, param, script, word } from "./fixtures.ts";
import type { Statement } from "./ir.ts";
import { randomChoice, randomRange, randomSource } from "./random-fixtures.ts";
import { readIR } from "./reader.ts";
import { nativeRuntimeBundle } from "./runtime-fixtures.ts";
import { checksum } from "./translate.ts";

export type RandomBlockOptions = {
    omitLastJump?: boolean;
    leadingEols?: number[][];
    joinEols?: number[];
    targetAfterLeadingEol?: boolean[];
};

export function randomBlock(
    op: number,
    branches: number[][],
    weights = branches.map(() => 1),
    options: RandomBlockOptions = {},
): number[] {
    const bytes = [op, ...word(branches.length), ...weights.flatMap((w) => [w & 255, w >>> 8])];
    const table = bytes.length,
        exits: number[] = [];
    for (const _ of branches) bytes.push(0, 0, 0, 0);
    const jump = (fieldOffset: number, target: number) =>
        bytes.splice(fieldOffset, 4, ...word((target - fieldOffset - 4) >>> 0));
    branches.forEach((branch, i) => {
        const leading = options.leadingEols?.[i] ?? [];
        const beforeLeading = bytes.length;
        bytes.push(...leading);
        const afterLeading = bytes.length;
        const target = options.targetAfterLeadingEol?.[i] ? afterLeading : beforeLeading;
        jump(table + 4 * i, target);
        bytes.push(...branch);
        if (i < branches.length - 1 || !options.omitLastJump) {
            bytes.push(0x2e, 0, 0, 0, 0);
            exits.push(bytes.length - 4);
        }
    });
    const joinOffset = bytes.length;
    if (options.joinEols) bytes.push(...options.joinEols);
    for (const fieldOffset of exits) jump(fieldOffset, joinOffset);
    return bytes;
}

export function switchBlock(selector: number[], cases: [number[], number[]][], fallback?: number[]): number[] {
    const bytes = [0x3c, ...selector, 1],
        exits: number[] = [];
    let previous: number | undefined;
    const jump = (fieldOffset: number, target: number) => {
        bytes[fieldOffset] = (target - fieldOffset) & 255;
        bytes[fieldOffset + 1] = (target - fieldOffset) >>> 8;
    };
    for (const [fields, body] of cases) {
        const start = bytes.length;
        if (previous !== undefined) jump(previous, start);
        bytes.push(0x3e, 0x49, 0, 0, ...fields, 1, ...body, 0x49, 0, 0);
        previous = start + 2;
        exits.push(bytes.length - 2);
    }
    if (previous !== undefined) jump(previous, bytes.length);
    if (fallback !== undefined) {
        bytes.push(0x3f, 0x49, 0, 0, ...fallback);
        exits.push(bytes.length - fallback.length - 2);
    }
    const end = bytes.length;
    bytes.push(0x3d, 1);
    for (const fieldOffset of exits) jump(fieldOffset, end);
    return bytes;
}

export const loopBlock = (body: number[], repeatFields: number[] = []) => [0x20, 1, ...body, 0x21, ...repeatFields, 1];
export const assignLine = (key: string, value: number[]) => [...field(key, value), 1];
export const returnLine = (fields: number[] = []) => [0x29, ...fields, 1];
export const exprTokens = (...parts: number[][]) => [14, ...parts.flat(), 15];

export function randomControlFixture() {
    const emitTag = (tag: number) => command("RecordStep", field("tag", integer(tag)));
    const pauseOne = command("WaitRandomStep", field("frames", integer(1)));

    // 1. Shortest weighted block (1 branch with 0x2e, plus 2-branch compiler-style omitLastJump with untaken fault).
    const shortestSingle = randomBlock(0x2f, [emitTag(101)], [3]);
    const shortestWeighted = randomBlock(
        0x2f,
        [
            [...assignLine("picked", integer(1)), ...emitTag(111)],
            [
                ...command("NeverCalledHost"),
                ...assignLine("picked", exprTokens(param("missingParam"), [11], integer(1))),
            ],
        ],
        [1, 0],
        { omitLastJump: true, leadingEols: [[1], [1]], targetAfterLeadingEol: [true, true], joinEols: [1] },
    );

    // 2. Second stream (0x37) mixed with first-stream randomChoice value and second-stream randomRange2.
    const secondStreamBlock = randomBlock(
        0x37,
        [
            [
                ...assignLine("choiceVal", randomChoice(0x2f, [integer(10), integer(20)], [1, 1])),
                ...assignLine("rangeVal", randomRange(0x3f800000, 0x40800000, true)),
                ...emitTag(201),
            ],
            [
                ...assignLine("choiceVal", randomChoice(0x2f, [integer(30), integer(40)], [1, 1])),
                ...assignLine("rangeVal", randomRange(0x40a00000, 0x41000000, true)),
                ...emitTag(202),
            ],
        ],
        [2, 3],
        { omitLastJump: true, joinEols: [1] },
    );

    // 3. RandomNoRepeat (0x40) with multiple statements inside a repeat loop.
    const noRepeatLoop = loopBlock(
        randomBlock(
            0x40,
            [
                [...assignLine("lastBranch", integer(0)), ...emitTag(301), ...emitTag(302)],
                [...assignLine("lastBranch", integer(1)), ...emitTag(311), ...emitTag(312)],
                [...assignLine("lastBranch", integer(2)), ...emitTag(321), ...emitTag(322)],
            ],
            [1, 1, 1],
            { omitLastJump: true, joinEols: [1] },
        ),
        integer(6),
    );

    // 4. RandomPermute (0x41) yielding and resuming mid-branch across frames.
    const permuteWaitResume = loopBlock(
        randomBlock(
            0x41,
            [
                [...emitTag(401), ...pauseOne, ...emitTag(402), ...assignLine("afterResume", integer(1))],
                [...emitTag(411), ...pauseOne, ...emitTag(412), ...assignLine("afterResume", integer(2))],
                [...emitTag(421), ...pauseOne, ...emitTag(422), ...assignLine("afterResume", integer(3))],
            ],
            [1, 1, 1],
        ),
        integer(6),
    );

    // 5. Nested random blocks with if/else, switch/case/default, and inner loop/break.
    const nestedControl = randomBlock(
        0x2f,
        [
            [
                ...emitTag(500),
                ...randomBlock(
                    0x37,
                    [
                        conditional(
                            exprTokens(param("flag"), [7], integer(1)),
                            [...emitTag(511), ...assignLine("nestedOut", integer(11))],
                            [...emitTag(512), ...assignLine("nestedOut", integer(12))],
                        ),
                        switchBlock(
                            param("mode"),
                            [
                                [integer(1), [...emitTag(521), ...assignLine("nestedOut", integer(21))]],
                                [integer(2), [...emitTag(522), ...assignLine("nestedOut", integer(22))]],
                            ],
                            [...emitTag(529), ...assignLine("nestedOut", integer(29))],
                        ),
                    ],
                    [1, 1],
                    { omitLastJump: true, joinEols: [1] },
                ),
                ...emitTag(509),
            ],
            [
                ...emitTag(550),
                ...loopBlock(
                    [
                        ...emitTag(551),
                        ...conditional(exprTokens(param("stopInner"), [7], integer(1)), [0x22, 1], []),
                        ...emitTag(552),
                    ],
                    integer(2),
                ),
                ...assignLine("nestedOut", integer(55)),
            ],
        ],
        [1, 1],
        { omitLastJump: true, joinEols: [1] },
    );

    // 6. Break targeting the enclosing loop from inside a random branch, with repeat argument evaluation check.
    const breakOuterLoop = [
        ...assignLine("iterations", integer(0)),
        ...loopBlock(
            [
                ...assignLine("iterations", exprTokens(param("iterations"), [11], integer(1))),
                ...randomBlock(
                    0x40,
                    [
                        [...emitTag(601), 0x22, 1, ...emitTag(699)],
                        [...emitTag(602), 0x22, 1, ...emitTag(698)],
                    ],
                    [1, 1],
                ),
                ...emitTag(650),
            ],
            field("repeatArg", randomChoice(0x2f, [integer(5), integer(5)], [1, 1])),
        ),
        ...emitTag(690),
    ];

    // 7. Call and return inside selected random branches (callee return vs root return).
    const randomCallee = randomBlock(
        0x2f,
        [
            [...emitTag(701), ...returnLine([...field("fromCallee", integer(71)), ...field("early", integer(1))])],
            [...emitTag(702), ...assignLine("fromCallee", integer(72))],
        ],
        [1, 1],
        { omitLastJump: true, joinEols: [1] },
    );
    const randomCaller = [
        ...command("randomCallee"),
        ...emitTag(710),
        ...randomBlock(
            0x37,
            [
                [...emitTag(721), ...returnLine(field("rootReturned", integer(100)))],
                [...emitTag(722), ...assignLine("rootReturned", integer(200))],
            ],
            [1, 1],
        ),
        ...emitTag(730),
    ];

    // 8. Unknown host inside the selected branch faults after advancing RNG and executing preceding statements.
    const selectedUnknownHost = randomBlock(
        0x2f,
        [[...emitTag(801), ...command("UnknownSelectedHost")], [...emitTag(802)]],
        [1, 0],
    );

    const input = Uint8Array.from([
        ...script("shortestSingle", shortestSingle),
        ...script("shortestWeighted", shortestWeighted),
        ...script("secondStreamBlock", secondStreamBlock),
        ...script("noRepeatLoop", noRepeatLoop),
        ...script("permuteWaitResume", permuteWaitResume),
        ...script("nestedControl", nestedControl, [
            ...field("flag", integer(1)),
            ...field("mode", integer(2)),
            ...field("stopInner", integer(1)),
        ]),
        ...script("breakOuterLoop", breakOuterLoop),
        ...script("randomCallee", [...randomCallee, ...emitTag(709)]),
        ...script("randomCaller", randomCaller),
        ...script("selectedUnknownHost", selectedUnknownHost),
        0,
    ]);

    const ir = readIR(input, "random-control.qb");
    const sites: Record<string, { source: string; script: number; offset: number }[]> = {};
    const collectSites = (
        scriptName: number,
        xs: Statement[],
        out: { source: string; script: number; offset: number }[],
    ) => {
        for (const st of xs) {
            if (st.kind === "random") {
                out.push({ source: ir.source, script: scriptName, offset: st.offset });
                for (const b of st.branches) collectSites(scriptName, b, out);
            } else if (st.kind === "loop") {
                collectSites(scriptName, st.body, out);
            } else if (st.kind === "if") {
                collectSites(scriptName, st.yes, out);
                collectSites(scriptName, st.no, out);
            } else if (st.kind === "switch") {
                for (const c of st.cases) collectSites(scriptName, c.body, out);
                if (st.fallback) collectSites(scriptName, st.fallback, out);
            }
        }
    };
    for (const def of ir.definitions) {
        if (def.kind === "script") {
            const list: { source: string; script: number; offset: number }[] = [];
            collectSites(def.name, def.body, list);
            sites[String(def.name)] = list;
        }
    }
    const choiceSites: Record<string, { source: string; script: number; offset: number }[]> = {};
    for (const def of ir.definitions) {
        if (def.kind === "script" && def.name === checksum("secondStreamBlock")) {
            const b = def.body[0];
            if (b?.kind === "random") {
                choiceSites.secondStream = b.branches.map((branch) => {
                    const first = branch[0];
                    if (first?.kind !== "assign") throw Error("Expected assign in secondStreamBlock");
                    return { source: ir.source, script: def.name, offset: first.value.offset };
                });
            }
        }
        if (def.kind === "script" && def.name === checksum("breakOuterLoop")) {
            const loopSt = def.body[1];
            if (loopSt?.kind === "loop" && loopSt.repeatFields[0] && !("spread" in loopSt.repeatFields[0])) {
                choiceSites.breakRepeat = [
                    { source: ir.source, script: def.name, offset: loopSt.repeatFields[0].value.offset },
                ];
            }
        }
    }
    return { input, ir, javascript: emitIR(ir), sites, choiceSites };
}

export function randomControlAssertions(): string {
    const key = checksum,
        fixture = randomControlFixture(),
        sites = JSON.stringify(fixture.sites),
        choiceSites = JSON.stringify(fixture.choiceSites);
    return `(() => {
const savedHost = globalThis.__qbHost;
try {
let assertions = 0;
const assert = (ok, why) => { ++assertions; if (!ok) throw Error('Random control test: ' + why); };
const events = [];
globalThis.__qbHost = (name, args) => {
    if (name === ${key("RecordStep")}) {
        const tag = args.find(f => f[0] === ${key("tag")})?.[1]?.bits;
        events.push(tag);
        return { condition: true, outputs: [] };
    }
    if (name === ${key("WaitRandomStep")}) {
        const frames = args.find(f => f[0] === ${key("frames")})?.[1]?.bits ?? 1;
        return { condition: true, outputs: [], frames };
    }
    throw Error('Unknown host command 0x' + name.toString(16));
};
const rng = qb.random(), ref = qbRandom.create(0), sites = ${sites}, choiceSites = ${choiceSites};
const get = (p, n) => p.findLast(f => f[0] === n)?.[1];
const sameStreams = (label) => {
    for (const s of [0, 1]) {
        assert(JSON.stringify(rng.state(s)) === JSON.stringify(ref.state(s)), 'RNG stream ' + s + ' mismatch: ' + label);
    }
};
const reset = (seed) => {
    rng.seed(seed); rng.clearStored();
    ref.seed(seed); ref.clearStored();
    events.length = 0;
};
let frameCounter = 1000;
const runSync = (scriptName, args = [], fuel = 100000) => {
    const id = qb.start(scriptName, args, fuel);
    try {
        return qb.update(id, ++frameCounter);
    } finally {
        qb.cancel(id);
    }
};

// 1. Shortest weighted blocks & untaken branch isolation.
reset(0);
let r = runSync(${key("shortestSingle")});
assert(r.status === 'finished' && events.join(',') === '101', 'shortestSingle executes');
ref.choose(0x2f, sites[${key("shortestSingle")}][0], [3], 1);
sameStreams('shortestSingle');

events.length = 0;
r = runSync(${key("shortestWeighted")});
assert(r.status === 'finished' && events.join(',') === '111' && get(r.parameters, ${key("picked")})?.bits === 1, 'shortestWeighted selected branch only');
ref.choose(0x2f, sites[${key("shortestWeighted")}][0], [1, 0], 1);
sameStreams('shortestWeighted');

// 2. Second stream (0x37) + inner stream-0 randomChoice + stream-1 randomRange2 across multiple seeds.
for (const seed of [0, 1, 77, 321, 0xffffffff]) {
    reset(seed);
    const id = qb.start(${key("secondStreamBlock")}, [], 100000);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    const branch = ref.choose(0x37, sites[${key("secondStreamBlock")}][0], [2, 3], id);
    const valPick = ref.choose(0x2f, choiceSites.secondStream[branch], [1, 1], id);
    const expectedChoice = branch === 0 ? [10, 20][valPick] : [30, 40][valPick];
    const expectedRange = branch === 0
        ? ref.range(0x3f800000, 0x40800000, true).bits
        : ref.range(0x40a00000, 0x41000000, true).bits;
    const expectedEvent = branch === 0 ? 201 : 202;
    assert(r.status === 'finished', r.message);
    assert(events.length === 1 && events[0] === expectedEvent, 'secondStreamBlock event trace');
    assert(get(r.parameters, ${key("choiceVal")})?.bits === expectedChoice, 'secondStreamBlock choiceVal');
    assert(get(r.parameters, ${key("rangeVal")})?.bits === expectedRange, 'secondStreamBlock rangeVal');
    sameStreams('secondStreamBlock seed ' + seed);
}

// 3. RandomNoRepeat (0x40) multi-statement loop & concurrent owner isolation.
for (const seed of [0, 42, 999]) {
    reset(seed);
    const id = qb.start(${key("noRepeatLoop")}, [], 100000);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    const expectedTrace = [];
    let lastBranch = -1;
    for (let i = 0; i < 6; i++) {
        const b = ref.choose(0x40, sites[${key("noRepeatLoop")}][0], [1, 1, 1], id);
        if (i > 0) assert(b !== lastBranch, 'NoRepeat never repeats consecutive branch');
        lastBranch = b;
        expectedTrace.push(301 + b * 10, 302 + b * 10);
    }
    ref.forget(id);
    assert(r.status === 'finished', r.message);
    assert(events.join(',') === expectedTrace.join(','), 'noRepeatLoop multi-statement trace');
    assert(get(r.parameters, ${key("lastBranch")})?.bits === lastBranch, 'noRepeatLoop final state');
    sameStreams('noRepeatLoop seed ' + seed);
}

// 4. RandomPermute (0x41) with yield/resume in the middle of the selected branch + concurrent task histories.
reset(123);
const permuteSite = sites[${key("permuteWaitResume")}][0];
const taskA = qb.start(${key("permuteWaitResume")}, [], 100000);
const taskB = qb.start(${key("permuteWaitResume")}, [], 100000);
const expectedEvents = [];
const picksA = [], picksB = [];
for (let step = 0; step < 6; step++) {
    const pickA = ref.choose(0x41, permuteSite, [1, 1, 1], taskA);
    picksA.push(pickA);
    expectedEvents.push(401 + pickA * 10);
    const snapA = qb.update(taskA, ++frameCounter);
    assert(snapA.status === 'waiting', 'taskA waits mid-branch at step ' + step);

    const pickB = ref.choose(0x41, permuteSite, [1, 1, 1], taskB);
    picksB.push(pickB);
    expectedEvents.push(401 + pickB * 10);
    const snapB = qb.update(taskB, ++frameCounter);
    assert(snapB.status === 'waiting', 'taskB waits mid-branch at step ' + step);

    sameStreams('permuteWaitResume mid-wait step ' + step);

    // Resume taskA and taskB on the next frames: each finishes the current branch and enters the next iteration (or finishes on step 5).
    if (step === 5) {
        expectedEvents.push(402 + pickA * 10);
        const doneA = qb.update(taskA, ++frameCounter);
        ref.forget(taskA);
        assert(doneA.status === 'finished' && get(doneA.parameters, ${key("afterResume")})?.bits === pickA + 1, 'taskA finishes after resume');

        expectedEvents.push(402 + pickB * 10);
        const doneB = qb.update(taskB, ++frameCounter);
        ref.forget(taskB);
        assert(doneB.status === 'finished' && get(doneB.parameters, ${key("afterResume")})?.bits === pickB + 1, 'taskB finishes after resume');
    } else {
        // Resuming advances past pauseOne, emits 402 + pick*10, loops around, chooses the next branch, emits 401 + nextPick*10, and pauses again!
        break;
    }
}
qb.cancel(taskA); ref.forget(taskA);
qb.cancel(taskB); ref.forget(taskB);

// Full 6-iteration frame-by-frame verification of permuteWaitResume against independent reference model.
reset(321);
const taskP = qb.start(${key("permuteWaitResume")}, [], 100000);
const traceP = [];
const chosenP = [];
for (let frame = 0; frame < 7; frame++) {
    if (frame < 6) {
        const pick = ref.choose(0x41, permuteSite, [1, 1, 1], taskP);
        chosenP.push(pick);
        traceP.push(401 + pick * 10);
    }
    const snap = qb.update(taskP, ++frameCounter);
    assert(snap.status === (frame < 6 ? 'waiting' : 'finished'), 'permuteWaitResume frame ' + frame);
    sameStreams('permuteWaitResume frame ' + frame);
    if (frame < 6) {
        traceP.push(402 + chosenP[frame] * 10);
    }
}
ref.forget(taskP);
qb.cancel(taskP);
assert(events.join(',') === traceP.join(','), 'permuteWaitResume complete pre/post-yield event trace');
assert(new Set(chosenP.slice(2, 5)).size === 3, 'permuteWaitResume full cycle across waits');

// 5. Nested random blocks with if/else, switch/case/default and inner loop/break.
for (const seed of [0, 1, 2, 3, 4, 5, 10, 25]) {
    for (const [flagVal, modeVal, stopVal] of [[1, 1, 1], [0, 2, 0], [1, 9, 1]]) {
        reset(seed);
        const args = [
            [${key("flag")}, { type: 1, bits: flagVal }],
            [${key("mode")}, { type: 1, bits: modeVal }],
            [${key("stopInner")}, { type: 1, bits: stopVal }],
        ];
        const id = qb.start(${key("nestedControl")}, args, 100000);
        r = qb.update(id, ++frameCounter);
        qb.cancel(id);
        const outerSite = sites[${key("nestedControl")}][0];
        const innerSite = sites[${key("nestedControl")}][1];
        const outerPick = ref.choose(0x2f, outerSite, [1, 1], id);
        const expected = [];
        let expectedOut = 0;
        if (outerPick === 0) {
            expected.push(500);
            const innerPick = ref.choose(0x37, innerSite, [1, 1], id);
            if (innerPick === 0) {
                if (flagVal === 1) { expected.push(511); expectedOut = 11; }
                else { expected.push(512); expectedOut = 12; }
            } else {
                if (modeVal === 1) { expected.push(521); expectedOut = 21; }
                else if (modeVal === 2) { expected.push(522); expectedOut = 22; }
                else { expected.push(529); expectedOut = 29; }
            }
            expected.push(509);
        } else {
            expected.push(550);
            if (stopVal === 1) {
                expected.push(551);
            } else {
                expected.push(551, 552, 551, 552);
            }
            expectedOut = 55;
        }
        assert(r.status === 'finished', r.message);
        assert(events.join(',') === expected.join(','), 'nestedControl trace seed ' + seed);
        assert(get(r.parameters, ${key("nestedOut")})?.bits === expectedOut, 'nestedControl output');
        sameStreams('nestedControl seed ' + seed);
    }
}

// 6. Break from inside a random branch exits the enclosing loop and evaluates repeat arguments with empty scope.
reset(7);
{
    const id = qb.start(${key("breakOuterLoop")}, [], 100000);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    const branch = ref.choose(0x40, sites[${key("breakOuterLoop")}][0], [1, 1], id);
    ref.choose(0x2f, choiceSites.breakRepeat[0], [1, 1], id);
    ref.forget(id);
    assert(r.status === 'finished', r.message);
    assert(events.join(',') === (branch === 0 ? '601,690' : '602,690'), 'breakOuterLoop exits loop and skips tail');
    assert(get(r.parameters, ${key("iterations")})?.bits === 1, 'breakOuterLoop ran one iteration');
    sameStreams('breakOuterLoop');
}

// 7. Call and return inside random branches: callee return vs root return.
for (const seed of [0, 1, 2, 3, 7, 15]) {
    reset(seed);
    const id = qb.start(${key("randomCaller")}, [], 100000);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    const calleePick = ref.choose(0x2f, sites[${key("randomCallee")}][0], [1, 1], id);
    const callerPick = ref.choose(0x37, sites[${key("randomCaller")}][0], [1, 1], id);
    const expected = [];
    if (calleePick === 0) expected.push(701);
    else expected.push(702, 709);
    expected.push(710);
    if (callerPick === 0) expected.push(721);
    else expected.push(722, 730);
    assert(r.status === 'finished', r.message);
    assert(events.join(',') === expected.join(','), 'randomCaller/randomCallee event trace seed ' + seed);
    assert(calleePick === 0 ? get(r.parameters, ${key("fromCallee")})?.bits === 71 : get(r.parameters, ${key("fromCallee")}) === undefined, 'callee return merges only on explicit return');
    assert(get(r.parameters, ${key("rootReturned")})?.bits === (callerPick === 0 ? 100 : 200), 'root return merges into task parameters');
    sameStreams('randomCaller seed ' + seed);
}

// 8. Unknown host in selected branch faults with exact site and preserves prior RNG draw + event.
reset(19);
{
    const id = qb.start(${key("selectedUnknownHost")}, [], 100000);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    ref.choose(0x2f, sites[${key("selectedUnknownHost")}][0], [1, 0], id);
    assert(r.status === 'fault' && r.message.includes('Unknown host command') && r.message.includes('random-control.qb'), 'selectedUnknownHost faults');
    assert(events.join(',') === '801', 'selectedUnknownHost ran statements before fault');
    sameStreams('selectedUnknownHost');
}

// 9. Fuel exhaustion before random statement draw leaves RNG untouched.
reset(55);
{
    const before0 = JSON.stringify(rng.state(0)), before1 = JSON.stringify(rng.state(1));
    const id = qb.start(${key("noRepeatLoop")}, [], 2);
    r = qb.update(id, ++frameCounter);
    qb.cancel(id);
    assert(r.status === 'fault' && r.message.includes('fuel'), 'fuel exhausted before random draw');
    assert(JSON.stringify(rng.state(0)) === before0 && JSON.stringify(rng.state(1)) === before1, 'fuel failure does not advance RNG');
}

globalThis.qbRandomControlTestResult = { completed: true, assertions };
} finally { globalThis.__qbHost = savedHost; }
})();\n`;
}

export function randomControlBundle(): string {
    return (
        randomSource() +
        nativeRuntimeBundle() +
        randomControlFixture().javascript +
        randomControlAssertions() +
        '\nif (globalThis.qbRandomControlTestResult?.completed !== true) throw Error("Random control assertions did not complete");\n' +
        "globalThis.qbIntegrationTestResult = { completed: true, randomControl: globalThis.qbRandomControlTestResult.assertions };\n"
    );
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const output = process.argv[2];
    if (!output || process.argv.length !== 3)
        throw Error("Usage: node tools/qbjs/random-control-fixtures.ts OUTPUT.js");
    writeFileSync(output, randomControlBundle());
}
