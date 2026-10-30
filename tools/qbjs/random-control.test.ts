// NOT ORIGINAL - statement-level random control-flow and jump validation tests.
import assert from "node:assert/strict";
import { test } from "node:test";
import { runInNewContext } from "node:vm";
import { emitIR, requiresRandom } from "./emitter.ts";
import { command, conditional, integer, name, script, word } from "./fixtures.ts";
import {
    assignLine,
    loopBlock,
    randomBlock,
    randomControlBundle,
    randomControlFixture,
    switchBlock,
} from "./random-control-fixtures.ts";
import { readIR } from "./reader.ts";

const qbModule = (...defs: number[][]) => Uint8Array.from([...defs.flat(), 0]);
const wrapScript = (body: number[], nameStr = "test") => qbModule(script(nameStr, body));

test("structured random statement blocks execute on product scheduler with independent RNG traces", () => {
    const context = runInNewContext(`${randomControlBundle()}globalThis`, {}, { timeout: 5000 });
    assert.equal(context.qbRandomControlTestResult?.completed, true);
    assert.ok(context.qbRandomControlTestResult.assertions >= 100);
    assert.equal(context.qbIntegrationTestResult?.completed, true);
});

test("statement random IR and emitted JS are deterministic and set requires.random", () => {
    const first = randomControlFixture();
    const second = randomControlFixture();
    assert.deepEqual(first.ir, second.ir);
    assert.equal(first.javascript, second.javascript);
    assert.equal(requiresRandom(first.ir), true);
    assert.match(first.javascript, /"random":1/);

    // A module with only a statement-level random block (no value randomRange/randomChoice) still requires random:1.
    const stmtOnlyIR = readIR(wrapScript(randomBlock(0x2f, [command("A"), command("B")], [1, 1])), "stmt-only.qb");
    assert.equal(requiresRandom(stmtOnlyIR), true);
    assert.match(emitIR(stmtOnlyIR), /"random":1/);
});

test("every truncation offset of random control-flow modules fails with source and QB offset", () => {
    const mainBytes = randomControlFixture().input;
    for (let i = 0; i < mainBytes.length; i++) {
        assert.throws(() => readIR(mainBytes.subarray(0, i), "random-control.qb"), /random-control\.qb: QB offset 0x/);
    }

    // Numbered EOL (0x02) + inline (no-EOL before 0x2e) + compiler-style omitLastJump module truncations.
    const numberedInline = wrapScript([
        ...randomBlock(
            0x2f,
            [
                [...name("InlineA"), ...integer(1)],
                [...name("InlineB"), ...integer(2)],
            ],
            [1, 2],
            {
                leadingEols: [
                    [2, ...word(10)],
                    [2, ...word(20)],
                ],
                targetAfterLeadingEol: [true, true],
                joinEols: [2, ...word(30)],
            },
        ),
        ...command("AfterJoin"),
    ]);
    assert.equal(readIR(numberedInline, "numbered.qb").definitions.length, 1);
    for (let i = 0; i < numberedInline.length; i++) {
        assert.throws(() => readIR(numberedInline.subarray(0, i), "numbered.qb"), /numbered\.qb: QB offset 0x/);
    }
});

test("validates branch counts, weights, backward/out-of-bounds jumps and misaligned targets", () => {
    // Invalid branch counts and Permute bounds.
    assert.throws(() => readIR(wrapScript([0x2f, ...word(0)])), /random branch count/);
    assert.throws(() => readIR(wrapScript([0x2f, ...word(4097)])), /random branch count/);
    assert.throws(() => readIR(wrapScript(randomBlock(0x41, [command("OnlyOne")]))), /RandomPermute requires 2\.\.66/);
    assert.throws(
        () =>
            readIR(
                wrapScript(
                    randomBlock(
                        0x41,
                        Array.from({ length: 67 }, () => command("Step")),
                    ),
                ),
            ),
        /RandomPermute requires 2\.\.66/,
    );

    // Unsafe weights for 0x2f and 0x37 (negative int16 or all-zero total).
    for (const op of [0x2f, 0x37]) {
        assert.throws(
            () => readIR(wrapScript(randomBlock(op, [command("A"), command("B")], [0, 0]))),
            /unsafe weighted random table/,
        );
        assert.throws(
            () => readIR(wrapScript(randomBlock(op, [command("A"), command("B")], [1, 0x8000]))),
            /unsafe weighted random table/,
        );
        assert.throws(
            () => readIR(wrapScript(randomBlock(op, [command("A")], [0xffff]))),
            /unsafe weighted random table/,
        );
    }

    // Backward and out-of-bounds table targets.
    const validTwo = randomBlock(0x2f, [command("A"), command("B")], [1, 1]);
    {
        const backwardTarget = [...validTwo];
        backwardTarget.splice(9, 4, ...word(0xfffffff0));
        assert.throws(() => readIR(wrapScript(backwardTarget)), /random target cannot jump backwards/);

        const oobTarget = [...validTwo];
        oobTarget.splice(9, 4, ...word(0x10000));
        assert.throws(() => readIR(wrapScript(oobTarget)), /random target exceeds module bounds/);
    }

    // Backward and out-of-bounds 0x2e branch jumps.
    {
        const jumpIndex = validTwo.indexOf(0x2e);
        assert.ok(jumpIndex > 0);
        const backwardJump = [...validTwo];
        backwardJump.splice(jumpIndex + 1, 4, ...word(0xfffffffb));
        assert.throws(() => readIR(wrapScript(backwardJump)), /random branch jump cannot jump backwards/);

        const oobJump = [...validTwo];
        oobJump.splice(jumpIndex + 1, 4, ...word(0x10000));
        assert.throws(() => readIR(wrapScript(oobJump)), /random branch jump exceeds module bounds/);
    }

    // Misaligned target inside 0x02 line number payload or inside branch command payload.
    const withNumberedLeading = randomBlock(0x2f, [command("A", integer(1)), command("B", integer(2))], [1, 1], {
        leadingEols: [
            [2, ...word(100)],
            [2, ...word(200)],
        ],
        joinEols: [2, ...word(300)],
    });
    assert.equal(readIR(wrapScript(withNumberedLeading)).definitions.length, 1);
    for (const delta of [1, 2, 3, 4, 6, 7]) {
        const misalignedFirst = [...withNumberedLeading];
        misalignedFirst.splice(9, 4, ...word(4 + delta));
        assert.throws(() => readIR(wrapScript(misalignedFirst)), /random target does not match branch boundary/);
    }

    // Second branch target pointing to first branch start or mid-first-branch.
    {
        const duplicateTarget = [...validTwo];
        duplicateTarget.splice(13, 4, ...word(0));
        assert.throws(() => readIR(wrapScript(duplicateTarget)), /random target does not match branch boundary/);
    }
});

test("rejects mismatched joins, missing branch jumps, cross-boundary jumps and unstructured 0x2e", () => {
    // Different join targets across branches.
    const threeBranches = randomBlock(0x2f, [command("A"), command("B"), command("C")], [1, 1, 1], {
        omitLastJump: true,
        joinEols: [1],
    });
    assert.equal(readIR(wrapScript(threeBranches)).definitions.length, 1);
    {
        const first2e = threeBranches.indexOf(0x2e);
        const mismatchedJoin = [...threeBranches];
        const currentDelta = Buffer.from(mismatchedJoin.slice(first2e + 1, first2e + 5)).readUInt32LE(0);
        mismatchedJoin.splice(first2e + 1, 4, ...word(currentDelta + 1));
        assert.throws(() => readIR(wrapScript(mismatchedJoin)), /random branches do not share a common join target/);
    }

    // Join pointing to the start of the last branch (delta 0 on first branch's 0x2e when omitLastJump is used).
    {
        const twoOmitLast = randomBlock(0x2f, [command("A"), command("B")], [1, 1], {
            omitLastJump: true,
            joinEols: [1],
        });
        const first2e = twoOmitLast.indexOf(0x2e);
        const joinAtLastBranchStart = [...twoOmitLast];
        joinAtLastBranchStart.splice(first2e + 1, 4, ...word(0));
        assert.throws(
            () => readIR(wrapScript(joinAtLastBranchStart)),
            /random branch jump does not target join boundary/,
        );
    }

    // Join pointing inside a 0x02 line-number token at the join boundary.
    {
        const withNumberedJoin = randomBlock(0x2f, [command("A"), command("B")], [1, 1], {
            joinEols: [2, ...word(500)],
        });
        const first2e = withNumberedJoin.indexOf(0x2e);
        const second2e = withNumberedJoin.lastIndexOf(0x2e);
        const badJoin = [...withNumberedJoin];
        const d1 = Buffer.from(badJoin.slice(first2e + 1, first2e + 5)).readUInt32LE(0);
        const d2 = Buffer.from(badJoin.slice(second2e + 1, second2e + 5)).readUInt32LE(0);
        badJoin.splice(first2e + 1, 4, ...word(d1 + 2));
        badJoin.splice(second2e + 1, 4, ...word(d2 + 2));
        assert.throws(() => readIR(wrapScript(badJoin)), /random branch jump does not target join boundary/);
    }

    // Missing 0x2e on a single-branch block or non-final branch.
    assert.throws(
        () => readIR(wrapScript(randomBlock(0x2f, [command("Only")], [1], { omitLastJump: true }))),
        /random branch requires join jump/,
    );
    {
        const missingFirstJump = [
            0x2f,
            ...word(2),
            1,
            0,
            1,
            0,
            ...word(4),
            ...word(4 + command("A").length - 4),
            ...command("A"),
            ...command("B"),
            0x2e,
            ...word(0),
        ];
        assert.throws(() => readIR(wrapScript(missingFirstJump)), /random branch requires join jump/);
    }

    // Inline command without EOL before 0x2e where join has no EOL/endscript before next command.
    {
        const inlineNoJoinEol = [
            ...randomBlock(
                0x2f,
                [
                    [...name("CmdA"), ...integer(1)],
                    [...name("CmdB"), ...integer(2)],
                ],
                [1, 1],
            ),
            ...command("NextCmd"),
        ];
        assert.throws(() => readIR(wrapScript(inlineNoJoinEol)), /random branch jump does not target join boundary/);
    }

    // Unstructured 0x2e at script level or inside if/loop/switch.
    assert.throws(() => readIR(wrapScript([0x2e, ...word(0)])), /unexpected random jump token 0x2e/);
    assert.throws(() => readIR(wrapScript([0x2e, ...word(0xfffffffb)])), /unexpected random jump token 0x2e/);
    assert.throws(
        () =>
            readIR(
                wrapScript(
                    randomBlock(0x2f, [conditional(name("Cond"), [0x2e, ...word(0)], []), command("Other")], [1, 1]),
                ),
            ),
        /unexpected random jump token 0x2e/,
    );
    assert.throws(
        () =>
            readIR(
                wrapScript(randomBlock(0x2f, [loopBlock([0x2e, ...word(0)], integer(1)), command("Other")], [1, 1])),
            ),
        /unexpected random jump token 0x2e/,
    );
    assert.throws(
        () =>
            readIR(
                wrapScript(
                    randomBlock(
                        0x2f,
                        [switchBlock(integer(1), [[integer(1), [0x2e, ...word(0)]]]), command("Other")],
                        [1, 1],
                    ),
                ),
            ),
        /unexpected random jump token 0x2e/,
    );

    // Inner random jumping past enclosing random branch boundary.
    {
        const inner = randomBlock(0x2f, [command("InnerA"), command("InnerB")], [1, 1], {
            omitLastJump: true,
            joinEols: [1],
        });
        const inner2e = inner.indexOf(0x2e);
        const badInner = [...inner];
        const delta = Buffer.from(badInner.slice(inner2e + 1, inner2e + 5)).readUInt32LE(0);
        badInner.splice(inner2e + 1, 4, ...word(delta + 2));
        const outer = [
            ...randomBlock(0x2f, [command("OuterA"), badInner], [1, 1], {
                omitLastJump: true,
                joinEols: [1],
            }),
            ...command("AfterOuter"),
        ];
        assert.throws(() => readIR(wrapScript(outer)), /crosses enclosing block boundary/);
    }
});

test("enforces nesting, node, loop-depth, break and array-element limits on random blocks", () => {
    // Break inside a random branch without an enclosing loop fails.
    assert.throws(
        () => readIR(wrapScript(randomBlock(0x2f, [[0x22, 1], command("Other")], [1, 1]))),
        /break outside local loop/,
    );

    // Statement nesting limit (> 64) via nested random blocks.
    let nested = command("Leaf");
    for (let i = 0; i < 65; i++) nested = randomBlock(0x2f, [nested]);
    assert.throws(() => readIR(wrapScript(nested)), /statement nesting limit/);

    // Loop nesting limit (> 10) inside/around random blocks.
    let loops = command("Leaf");
    for (let i = 0; i < 11; i++) loops = randomBlock(0x2f, [loopBlock(loops, integer(1))]);
    assert.throws(() => readIR(wrapScript(loops)), /loop nesting exceeds original loop storage/);

    // IR node limit.
    const valid = wrapScript(randomBlock(0x2f, [command("A"), command("B")], [1, 1]));
    assert.throws(() => readIR(valid, "limit.qb", { nodes: 2 }), /IR node limit/);

    // Random inside array literals remains explicitly rejected for #23.
    assert.throws(
        () => readIR(wrapScript(assignLine("arr", [5, ...randomBlock(0x2f, [integer(1)]), 6]))),
        /unsupported literal-array element/,
    );
});
