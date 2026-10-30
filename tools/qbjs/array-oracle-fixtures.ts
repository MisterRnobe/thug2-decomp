// NOT ORIGINAL - synthetic token fixtures and independent two-pass ParseArray
// reference model for issue #36 / #23. No extracted game QB bytes.
import { pathToFileURL } from "node:url";
import { runInNewContext } from "node:vm";
import { floating, integer, name, word } from "./fixtures.ts";
import { randomChoice, randomSource } from "./random-fixtures.ts";

export type StreamState = { add: number; mul: number; value: number };

export type SerializedArrayElement =
    | { type: 1 | 2 | 13; bits: number }
    | { type: 3 | 4; bytes: number[] }
    | { type: 5; coordinates: [number, number] }
    | { type: 6; coordinates: [number, number, number] }
    | { type: 10; members: [number, SerializedArrayElement][] }
    | { type: 12; elementType: number; size: number; elements: SerializedArrayElement[] }
    | { type: 0; bits: 0 };

export type TwoPassOracleExpectation = {
    seed: number;
    owner: number;
    endOffset: number;
    pass1: {
        count: number;
        elementType: number;
        stream0: StreamState;
        stream1: StreamState;
    };
    pass2Ran: boolean;
    pass2: {
        populatedCount: number;
        elementType: number;
        size: number;
        elements: SerializedArrayElement[];
        stream0: StreamState;
        stream1: StreamState;
    };
    subsequentProbes: number[];
    classification: "safe" | "empty_after_pass1" | "scalar_underfill";
};

export type ArrayOracleCase = {
    id: string;
    category:
        | "weighted_stream0"
        | "weighted_stream1"
        | "stored_norepeat"
        | "stored_permute"
        | "nested_array_skip"
        | "nested_structure_skip"
        | "numeric_promotion"
        | "cross_pass_divergence"
        | "separators_and_empty";
    description: string;
    tokens: number[];
    seeds: number[];
    owners: number[];
    repeatRuns?: number;
};

export type UnsafeDiagnosticCase = {
    id: string;
    hazard:
        | "population_overfill_single"
        | "population_overfill_heap"
        | "pointer_underfill_null"
        | "type_mismatch_desync";
    description: string;
    tokens: number[];
    seed: number;
    pass1Count: number;
    pass1Type: number;
    pass2AttemptedCount?: number;
    pass2EncounteredToken?: number;
};

export const stringToken = (ascii: string, local = false): number[] => {
    const data = [...Buffer.from(ascii, "utf8"), 0];
    return [local ? 0x1c : 0x1b, ...word(data.length), ...data];
};

export const pairToken = (x: number, y: number): number[] => {
    const b = Buffer.alloc(8);
    b.writeFloatLE(x, 0);
    b.writeFloatLE(y, 4);
    return [0x1f, ...b];
};

export const vectorToken = (x: number, y: number, z: number): number[] => {
    const b = Buffer.alloc(12);
    b.writeFloatLE(x, 0);
    b.writeFloatLE(y, 4);
    b.writeFloatLE(z, 8);
    return [0x1e, ...b];
};

export const arrayToken = (...parts: number[][]): number[] => [0x05, ...parts.flat(), 0x06];
export const structToken = (...parts: number[][]): number[] => [0x03, ...parts.flat(), 0x04];
export const namedFieldTokens = (key: number, value: number[]): number[] => [0x16, ...word(key), 0x07, ...value];
export const rawNameToken = (crc: number): number[] => [0x16, ...word(crc)];

function float32FromInt32Bits(intBits: number): number {
    const buf = Buffer.alloc(4);
    buf.writeFloatLE(intBits | 0, 0);
    return buf.readUInt32LE(0);
}

function skipToken(b: Buffer, p: number): number {
    const t = b[p]!;
    if (
        [
            0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x11, 0x12, 0x13, 0x14,
            0x15, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2c, 0x2d, 0x30, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x42, 0x45,
        ].includes(t)
    )
        return p + 1;
    if ([0x02, 0x16, 0x17, 0x18, 0x1a, 0x2e, 0x43, 0x44, 0x46].includes(t)) return p + 5;
    if (t === 0x1b || t === 0x1c) return p + 5 + b.readUInt32LE(p + 1);
    if (t === 0x1e) return p + 13;
    if (t === 0x1f) return p + 9;
    if (t === 0x2b) {
        let q = p + 5;
        while (b[q]) q++;
        return q + 1;
    }
    if ([0x2f, 0x37, 0x40, 0x41].includes(t)) return p + 5 + b.readUInt32LE(p + 1) * 6;
    if ([0x47, 0x48, 0x49].includes(t)) return p + 3;
    return p;
}

// Original SkipValue (0x0046FD60): nested 0x03 / 0x05 containers are skipped
// lexically via SkipToken without invoking ResolveRandomJumps.
function skipValue(b: Buffer, p: number): number {
    const t = b[p]!;
    if (t === 0x03 || t === 0x05) {
        const open = t,
            close = open + 1;
        let depth = 1;
        let cur = p;
        do {
            cur = skipToken(b, cur);
            if (b[cur] === open) ++depth;
            else if (b[cur] === close) --depth;
        } while (depth > 0);
        return cur + 1;
    }
    if ([0x16, 0x17, 0x18, 0x1a].includes(t)) return p + 5;
    if (t === 0x1b || t === 0x1c) return p + 5 + b.readUInt32LE(p + 1);
    if (t === 0x1e) return p + 13;
    if (t === 0x1f) return p + 9;
    return p;
}

function skipEndOfLinesIn(b: Buffer, p: number): number {
    let cur = p;
    for (;;) {
        if (b[cur] === 0x01) cur += 1;
        else if (b[cur] === 0x02) cur += 5;
        else return cur;
    }
}

type RngInstance = {
    seed: (s: number) => void;
    clearStored: () => void;
    draw: (n: number, stream?: number) => number;
    state: (stream?: number) => StreamState;
    choose: (
        op: number,
        site: { source: string; script: number; offset: number },
        weights: number[],
        owner: number,
    ) => number;
    forget: (owner: number) => void;
};

let cachedFactory: ((seed?: number) => RngInstance) | null = null;
function createRng(seed = 0): RngInstance {
    if (!cachedFactory) {
        const ctx = runInNewContext(`${randomSource()}\nglobalThis.qbRandom`);
        cachedFactory = (s = 0) => ctx.create(s);
    }
    return cachedFactory(seed);
}

// Evaluate original two-pass ParseArray (0x00471B00) using qbRandom.
export function evaluateTwoPassArray(
    tokens: number[],
    seed: number,
    owner = 1,
    existingRng?: RngInstance,
    scriptId = 1,
): { expectation: TwoPassOracleExpectation; rng: RngInstance } {
    const b = Buffer.from([...tokens, 0]);
    const rng = existingRng ?? createRng(seed);
    if (!existingRng) {
        rng.seed(seed);
        rng.clearStored();
    }
    const resolveRandomJumps = (start: number): number => {
        let p = start;
        for (;;) {
            const op = b[p]!;
            if (op === 0x2e) {
                p += 5 + b.readUInt32LE(p + 1);
                continue;
            }
            if (op === 0x49) {
                p += 1 + b.readUInt16LE(p + 1);
                continue;
            }
            if (op === 0x2f || op === 0x37 || op === 0x40 || op === 0x41) {
                const count = b.readUInt32LE(p + 1);
                const weights: number[] = [];
                for (let i = 0; i < count; i++) weights.push(b.readUInt16LE(p + 5 + 2 * i));
                const tableOffset = p + 5 + 2 * count;
                const choice = rng.choose(
                    op,
                    { source: "array-oracle.qb", script: scriptId, offset: p },
                    weights,
                    owner,
                );
                const fieldOffset = tableOffset + 4 * choice;
                p = fieldOffset + 4 + b.readUInt32LE(fieldOffset);
                continue;
            }
            return p;
        }
    };

    const addComponentToStruct = (
        members: [number, SerializedArrayElement][],
        fieldName: number,
        elem: SerializedArrayElement,
    ) => {
        for (let i = members.length - 1; i >= 0; --i) {
            const [existingName, existingVal] = members[i]!;
            if (existingName === fieldName) {
                const same =
                    existingVal.type === elem.type &&
                    (fieldName !== 0 || elem.type !== 13 || ("bits" in existingVal && existingVal.bits === elem.bits));
                const numeric = [1, 2].includes(existingVal.type) && [1, 2].includes(elem.type);
                if (same || numeric) members.splice(i, 1);
            }
        }
        members.push([fieldName, elem]);
    };

    const parseStructure = (tokenOffset: number): { nextOffset: number; value: SerializedArrayElement } => {
        const members: [number, SerializedArrayElement][] = [];
        const parseInto = (startToken: number): number => {
            let p = startToken + 1;
            for (;;) {
                p = skipEndOfLinesIn(b, p);
                p = resolveRandomJumps(p);
                const t = b[p]!;
                if (t === 0x04) return p + 1;
                if (t === 0x01 || t === 0x02) {
                    p = skipEndOfLinesIn(b, p);
                    continue;
                }
                if (t === 0x03) {
                    p = parseInto(p);
                    continue;
                }
                if (t === 0x09) {
                    ++p;
                    continue;
                }
                if (t === 0x16) {
                    const candidateName = b.readUInt32LE(p + 1);
                    let next = resolveRandomJumps(skipEndOfLinesIn(b, p + 5));
                    if (b[next] !== 0x07) {
                        addComponentToStruct(members, 0, { type: 13, bits: candidateName });
                        p = p + 5;
                        continue;
                    }
                    next = resolveRandomJumps(skipEndOfLinesIn(b, next + 1));
                    p = addParameter(next, candidateName, members);
                    continue;
                }
                p = addParameter(p, 0, members);
            }
        };
        const addParameter = (p: number, fieldName: number, dest: [number, SerializedArrayElement][]): number => {
            const t = b[p]!;
            if (t === 0x16) {
                addComponentToStruct(dest, fieldName, { type: 13, bits: b.readUInt32LE(p + 1) });
                return p + 5;
            }
            if (t === 0x17) {
                addComponentToStruct(dest, fieldName, { type: 1, bits: b.readUInt32LE(p + 1) });
                return p + 5;
            }
            if (t === 0x1a) {
                addComponentToStruct(dest, fieldName, { type: 2, bits: b.readUInt32LE(p + 1) });
                return p + 5;
            }
            if (t === 0x1b || t === 0x1c) {
                const len = b.readUInt32LE(p + 1);
                const zero = b.indexOf(0, p + 5);
                addComponentToStruct(dest, fieldName, {
                    type: t === 0x1b ? 3 : 4,
                    bytes: [...b.subarray(p + 5, zero)],
                });
                return p + 5 + len;
            }
            if (t === 0x1f) {
                addComponentToStruct(dest, fieldName, {
                    type: 5,
                    coordinates: [b.readUInt32LE(p + 1), b.readUInt32LE(p + 5)],
                });
                return p + 9;
            }
            if (t === 0x1e) {
                addComponentToStruct(dest, fieldName, {
                    type: 6,
                    coordinates: [b.readUInt32LE(p + 1), b.readUInt32LE(p + 5), b.readUInt32LE(p + 9)],
                });
                return p + 13;
            }
            if (t === 0x03) {
                const inner = parseStructure(p);
                addComponentToStruct(dest, fieldName, inner.value);
                return inner.nextOffset;
            }
            if (t === 0x05) {
                const inner = parseArrayAt(p, false);
                addComponentToStruct(dest, fieldName, inner.arrayValue);
                return inner.endOffset;
            }
            throw Error(`Unsupported token 0x${t.toString(16)} in synthetic AddParameter`);
        };
        const nextOffset = parseInto(tokenOffset);
        return { nextOffset, value: { type: 10, members } };
    };

    const parseArrayAt = (
        tokenOffset: number,
        isRoot: boolean,
    ): {
        endOffset: number;
        pass1: TwoPassOracleExpectation["pass1"];
        pass2Ran: boolean;
        pass2: TwoPassOracleExpectation["pass2"];
        arrayValue: Extract<SerializedArrayElement, { type: 12 }>;
        classification: TwoPassOracleExpectation["classification"];
    } => {
        const first = tokenOffset + 1;
        let p = resolveRandomJumps(first);
        let type = 0;
        let count = 0;
        if (b[p] === 0x06) {
            const s0 = rng.state(0),
                s1 = rng.state(1);
            const emptyArr: Extract<SerializedArrayElement, { type: 12 }> = {
                type: 12,
                elementType: 0,
                size: 0,
                elements: [],
            };
            return {
                endOffset: p + 1,
                pass1: { count: 0, elementType: 0, stream0: s0, stream1: s1 },
                pass2Ran: false,
                pass2: {
                    populatedCount: 0,
                    elementType: 0,
                    size: 0,
                    elements: [],
                    stream0: s0,
                    stream1: s1,
                },
                arrayValue: emptyArr,
                classification: "empty_after_pass1",
            };
        }
        do {
            const t = b[p]!;
            switch (t) {
                case 0x03:
                    type = 10;
                    break;
                case 0x05:
                    type = 12;
                    break;
                case 0x16:
                    type = 13;
                    break;
                case 0x17:
                case 0x18:
                    if (type !== 2) type = 1;
                    break;
                case 0x1a:
                    type = 2;
                    break;
                case 0x1b:
                    type = 3;
                    break;
                case 0x1c:
                    type = 4;
                    break;
                case 0x1e:
                    type = 6;
                    break;
                case 0x1f:
                    type = 5;
                    break;
            }
            if (t === 0x01 || t === 0x02 || t === 0x09) {
                p = skipToken(b, p);
            } else if ([0x03, 0x05, 0x16, 0x17, 0x18, 0x1a, 0x1b, 0x1c, 0x1e, 0x1f].includes(t)) {
                ++count;
                p = skipValue(b, p);
            } else {
                throw Error(`Unexpected token 0x${t.toString(16)} in ParseArray pass 1 at ${p}`);
            }
            p = resolveRandomJumps(p);
        } while (b[p] !== 0x06);

        const pass1Snap = {
            count,
            elementType: type,
            stream0: rng.state(0),
            stream1: rng.state(1),
        };

        if (type === 0) {
            const emptyArr: Extract<SerializedArrayElement, { type: 12 }> = {
                type: 12,
                elementType: 0,
                size: 0,
                elements: [],
            };
            return {
                endOffset: p + 1,
                pass1: pass1Snap,
                pass2Ran: false,
                pass2: {
                    populatedCount: 0,
                    elementType: 0,
                    size: 0,
                    elements: [],
                    stream0: pass1Snap.stream0,
                    stream1: pass1Snap.stream1,
                },
                arrayValue: emptyArr,
                classification: "empty_after_pass1",
            };
        }

        const elements: SerializedArrayElement[] = [];
        p = resolveRandomJumps(first);
        while (b[p] !== 0x06) {
            const t = b[p]!;
            switch (type) {
                case 10:
                    if (t === 0x01 || t === 0x09) ++p;
                    else if (t === 0x02) p += 5;
                    else if (t === 0x03) {
                        const st = parseStructure(p);
                        p = st.nextOffset;
                        elements.push(st.value);
                    } else throw Error(`Unexpected token 0x${t.toString(16)} in structure array pass 2`);
                    break;
                case 12:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x05) {
                        const nested = parseArrayAt(p - 1, false);
                        p = nested.endOffset;
                        elements.push(nested.arrayValue);
                    }
                    break;
                case 1:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x17 || t === 0x18) {
                        elements.push({ type: 1, bits: b.readUInt32LE(p) });
                        p += 4;
                    }
                    break;
                case 2:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x17 || t === 0x18) {
                        elements.push({ type: 2, bits: float32FromInt32Bits(b.readUInt32LE(p)) });
                        p += 4;
                    } else if (t === 0x1a) {
                        elements.push({ type: 2, bits: b.readUInt32LE(p) });
                        p += 4;
                    }
                    break;
                case 13:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x16) {
                        elements.push({ type: 13, bits: b.readUInt32LE(p) });
                        p += 4;
                    }
                    break;
                case 3:
                case 4:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === (type === 3 ? 0x1b : 0x1c)) {
                        const len = b.readUInt32LE(p);
                        p += 4;
                        const zero = b.indexOf(0, p);
                        elements.push({ type, bytes: [...b.subarray(p, zero)] });
                        p += len;
                    }
                    break;
                case 6:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x1e) {
                        elements.push({
                            type: 6,
                            coordinates: [b.readUInt32LE(p), b.readUInt32LE(p + 4), b.readUInt32LE(p + 8)],
                        });
                        p += 12;
                    }
                    break;
                case 5:
                    ++p;
                    if (t === 0x02) p += 4;
                    else if (t === 0x1f) {
                        elements.push({
                            type: 5,
                            coordinates: [b.readUInt32LE(p), b.readUInt32LE(p + 4)],
                        });
                        p += 8;
                    }
                    break;
            }
            p = resolveRandomJumps(p);
        }

        const populatedCount = elements.length;
        if (populatedCount > count) {
            throw Error(`Unsafe ParseArray population overflow (${populatedCount} > ${count})`);
        }
        let classification: TwoPassOracleExpectation["classification"] = "safe";
        while (elements.length < count) {
            classification = "scalar_underfill";
            if ([1, 2, 13].includes(type)) {
                elements.push({ type: type as 1 | 2 | 13, bits: 0 });
            } else {
                elements.push({ type: 0, bits: 0 });
            }
        }
        const arrayValue: Extract<SerializedArrayElement, { type: 12 }> = {
            type: 12,
            elementType: type,
            size: count,
            elements,
        };
        return {
            endOffset: p + 1,
            pass1: pass1Snap,
            pass2Ran: true,
            pass2: {
                populatedCount,
                elementType: type,
                size: count,
                elements,
                stream0: rng.state(0),
                stream1: rng.state(1),
            },
            arrayValue,
            classification: isRoot ? classification : "safe",
        };
    };

    const parsed = parseArrayAt(0, true);
    const subsequentProbes: number[] = [];
    for (let i = 0; i < 8; i++) {
        subsequentProbes.push(((rng.draw(2, i % 2) + 1) * 11) >>> 0);
    }
    return {
        expectation: {
            seed,
            owner,
            endOffset: parsed.endOffset,
            pass1: parsed.pass1,
            pass2Ran: parsed.pass2Ran,
            pass2: parsed.pass2,
            subsequentProbes,
            classification: parsed.classification,
        },
        rng,
    };
}

export function arrayOracleCases(): ArrayOracleCase[] {
    const fBits = (f: number) => {
        const b = Buffer.alloc(4);
        b.writeFloatLE(f, 0);
        return b.readUInt32LE(0);
    };
    const stdSeeds = [0, 1, 123, 0x7fffffff, 0x80000000, 0xffffffff];

    return [
        // 1. Weighted Random (0x2f, stream 0) across all supported element types
        {
            id: "weighted_0x2f_integers",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting integer elements in both passes",
            tokens: arrayToken(
                randomChoice(0x2f, [integer(11), integer(22), integer(33)], [1, 2, 3]),
                randomChoice(0x2f, [integer(100), integer(-5)], [2, 1]),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_floats",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting float32 elements including -0.0",
            tokens: arrayToken(
                randomChoice(0x2f, [floating(0x80000000), floating(fBits(1.5)), floating(fBits(-2.25))], [1, 1, 1]),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_names",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting raw Name checksum elements",
            tokens: arrayToken(randomChoice(0x2f, [name("alpha"), name("beta"), name("gamma")], [1, 1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_strings",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting String (0x1b) elements of different byte lengths",
            tokens: arrayToken(
                randomChoice(0x2f, [stringToken("a"), stringToken("bb"), stringToken("ccc")], [1, 1, 1]),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_localstrings",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting LocalString (0x1c) elements",
            tokens: arrayToken(randomChoice(0x2f, [stringToken("loc1", true), stringToken("loc2", true)], [1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_pairs_vectors",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting Pair (0x1f) and Vector (0x1e) elements",
            tokens: arrayToken(randomChoice(0x2f, [pairToken(1.0, 2.0), pairToken(3.5, -4.5)], [1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "weighted_0x2f_vectors",
            category: "weighted_stream0",
            description: "Weighted Random (0x2f) selecting Vector (0x1e) elements",
            tokens: arrayToken(randomChoice(0x2f, [vectorToken(1.0, 2.0, 3.0), vectorToken(4.0, 5.0, 6.0)], [1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },

        // 2. Weighted Random2 (0x37, stream 1) and dual-stream interleaving
        {
            id: "weighted_0x37_stream1_and_interleaved",
            category: "weighted_stream1",
            description: "Random2 (0x37) on stream 1 interleaved with Random (0x2f) on stream 0",
            tokens: arrayToken(
                randomChoice(0x37, [integer(10), integer(20), integer(30)], [1, 3, 2]),
                randomChoice(0x2f, [integer(40), integer(50)], [1, 1]),
                randomChoice(0x37, [integer(60), integer(70)], [2, 1]),
            ),
            seeds: stdSeeds,
            owners: [1],
        },

        // 3. Stored RandomNoRepeat (0x40) and RandomPermute (0x41) across passes, repeats, and owners
        {
            id: "stored_0x40_norepeat_two_pass_divergence",
            category: "stored_norepeat",
            description:
                "RandomNoRepeat (0x40) guarantees pass 2 selects a different branch than pass 1 on the same site/owner",
            tokens: arrayToken(
                randomChoice(0x40, [integer(11), integer(22)], [1, 1]),
                randomChoice(0x40, [integer(101), integer(202), integer(303)], [1, 1, 1]),
            ),
            seeds: stdSeeds,
            owners: [1, 2],
            repeatRuns: 3,
        },
        {
            id: "stored_0x41_permute_two_pass_progression",
            category: "stored_permute",
            description:
                "RandomPermute (0x41) advances permutation cursor in pass 1 (index 1) and again in pass 2 (index 2), wrapping with Shuffle(true) across repeated evaluations",
            tokens: arrayToken(randomChoice(0x41, [integer(11), integer(22), integer(33)], [1, 1, 1])),
            seeds: stdSeeds,
            owners: [1, 2],
            repeatRuns: 3,
        },

        // 4. Nested containers: SkipValue skip-path in pass 1 vs recursive evaluation in pass 2
        {
            id: "nested_array_inner_random_skipped_in_pass1",
            category: "nested_array_skip",
            description:
                "Outer pass 1 skips inner array via SkipValue (0 RNG draws in outer pass 1); outer pass 2 invokes recursive ParseArray (2 inner passes)",
            tokens: arrayToken(
                arrayToken(randomChoice(0x2f, [integer(11), integer(22)], [1, 1])),
                arrayToken(randomChoice(0x37, [integer(33), integer(44)], [1, 1])),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "outer_random_selecting_nested_arrays_with_inner_random",
            category: "nested_array_skip",
            description:
                "Outer random table chooses between two nested arrays; pass 1 draws only outer random, pass 2 draws outer random + 2 passes of the chosen inner array",
            tokens: arrayToken(
                randomChoice(
                    0x2f,
                    [
                        arrayToken(randomChoice(0x2f, [integer(11), integer(22)], [1, 1])),
                        arrayToken(randomChoice(0x37, [integer(33), integer(44)], [1, 1])),
                    ],
                    [1, 1],
                ),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "nested_structure_skipped_in_pass1_single_pass_in_pass2",
            category: "nested_structure_skip",
            description:
                "Outer pass 1 skips nested structures via SkipValue (0 RNG draws); pass 2 calls ParseStructure once per element (including bare-flag lookahead ResolveRandomJumps)",
            tokens: arrayToken(
                structToken(
                    namedFieldTokens(50, randomChoice(0x2f, [integer(11), integer(22)], [1, 1])),
                    rawNameToken(700),
                    randomChoice(0x37, [rawNameToken(801), rawNameToken(802)], [1, 1]),
                ),
            ),
            seeds: stdSeeds,
            owners: [1],
        },

        // 5. Numeric promotion (Integer + Float -> Float) and x87 int32->float32 rounding
        {
            id: "numeric_promotion_int_and_float_x87_rounding",
            category: "numeric_promotion",
            description:
                "Mixed Integer (16777217 = 2^24 + 1) and Float promotes array to Float (type 2); pass 2 converts chosen int32 via x87 float32 rounding (16777216.0f)",
            tokens: arrayToken(
                randomChoice(0x2f, [integer(16777217), floating(fBits(2.5))], [1, 1]),
                floating(fBits(7.5)),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "numeric_promotion_float_Sticky_before_int",
            category: "numeric_promotion",
            description:
                "Once pass 1 observes Float (type 2), subsequent Integer tokens keep type = Float (if (type != ESYMBOLTYPE_FLOAT) type = ESYMBOLTYPE_INTEGER)",
            tokens: arrayToken(floating(fBits(1.25)), randomChoice(0x2f, [integer(10), integer(-20)], [1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },

        // 6. Cross-pass multi-element branches with equal count/type, empty branches, and separators
        {
            id: "multi_element_random_branches_same_shape",
            category: "cross_pass_divergence",
            description:
                "Random branches each emitting 2 integers separated by EOL/comma tokens; pass 1 and pass 2 can select different pairs while keeping count = 3",
            tokens: arrayToken(
                [0x01, 0x09, 0x02, ...word(17)],
                randomChoice(
                    0x40,
                    [
                        [...integer(11), 0x09, ...integer(12)],
                        [...integer(21), 0x01, ...integer(22)],
                    ],
                    [1, 1],
                ),
                integer(99),
            ),
            seeds: stdSeeds,
            owners: [1],
        },
        {
            id: "empty_array_and_empty_branches",
            category: "separators_and_empty",
            description:
                "Empty array [] and array of empty random branches [ Random(@ @ ) ] terminate after pass 1 with type=0, size=0",
            tokens: arrayToken(randomChoice(0x2f, [[], []], [1, 1])),
            seeds: stdSeeds,
            owners: [1],
        },
    ];
}

// Unsafe count/type mismatch cases between pass 1 and pass 2 (documented & boundedly probed in Unicorn).
export function unsafeMismatchCases(): UnsafeDiagnosticCase[] {
    return [
        {
            id: "scalar_underfill_zero_tail",
            hazard: "pointer_underfill_null",
            description:
                "Pass 1 selects 2-element integer branch (count=2), pass 2 selects 1-element branch: slot 1 remains zero-initialized (0)",
            tokens: arrayToken(randomChoice(0x2f, [integer(11), [...integer(31), ...integer(32)]], [1, 1])),
            seed: 0, // Under seed 0: pass 1 draw(2) -> 1 (count=2), pass 2 draw(2) -> 0 (1 element written, slot[1]=0)
            pass1Count: 2,
            pass1Type: 1,
            pass2AttemptedCount: 1,
        },
        {
            id: "string_pointer_underfill_null_slot",
            hazard: "pointer_underfill_null",
            description:
                "Pass 1 selects 2-element String branch (count=2, type=3), pass 2 selects 1-element String branch: slot 1 remains a raw NULL pointer (0x00000000)",
            tokens: arrayToken(
                randomChoice(0x2f, [stringToken("one"), [...stringToken("twoA"), ...stringToken("twoB")]], [1, 1]),
            ),
            seed: 0, // Under seed 0: pass 1 draw(2) -> 1 (count=2), pass 2 draw(2) -> 0 (1 string written, slot[1]=NULL)
            pass1Count: 2,
            pass1Type: 3,
            pass2AttemptedCount: 1,
        },
        {
            id: "single_to_multi_overfill_corrupts_past_carray",
            hazard: "population_overfill_single",
            description:
                "Pass 1 selects 1-element branch (count=1, uses in-place CArray::m_single at dest+8 without heap buffer), pass 2 selects 2-element branch and writes the 2nd element out-of-bounds to dest+12",
            tokens: arrayToken(randomChoice(0x2f, [integer(11), [...integer(31), ...integer(32)]], [1, 1])),
            seed: 3, // Under seed 3: pass 1 draw(2) -> 0 (count=1), pass 2 draw(2) -> 1 (2 elements written -> OOB at dest+12)
            pass1Count: 1,
            pass1Type: 1,
            pass2AttemptedCount: 2,
        },
        {
            id: "heap_buffer_overfill_corrupts_past_elements",
            hazard: "population_overfill_heap",
            description:
                "Pass 1 selects 2-element branch (count=2, allocates 8-byte m_elements buffer), pass 2 selects 3-element branch and writes the 3rd element out-of-bounds to m_elements+8",
            tokens: arrayToken(
                randomChoice(
                    0x2f,
                    [
                        [...integer(11), ...integer(12)],
                        [...integer(31), ...integer(32), ...integer(33)],
                    ],
                    [1, 1],
                ),
            ),
            seed: 3, // Under seed 3: pass 1 draw(2) -> 0 (count=2), pass 2 draw(2) -> 1 (3 elements written -> OOB at m_elements+8)
            pass1Count: 2,
            pass1Type: 1,
            pass2AttemptedCount: 3,
        },
        {
            id: "type_mismatch_pass1_int_pass2_float_desync",
            hazard: "type_mismatch_desync",
            description:
                "Pass 1 selects Integer (type=1), pass 2 selects Float (0x1a with payload 0x3f060606): case ESYMBOLTYPE_INTEGER in pass 2 does ++p into the float payload, mistakes byte 0x06 for ']' and returns mid-token with an unpopulated slot",
            tokens: arrayToken(randomChoice(0x2f, [integer(11), floating(0x3f060606)], [1, 1])),
            seed: 3, // Under seed 3: pass 1 draw(2) -> 0 (Integer, type=1), pass 2 draw(2) -> 1 (Float 0x1a)
            pass1Count: 1,
            pass1Type: 1,
            pass2EncounteredToken: 0x1a,
        },
    ];
}

export function buildOracleDataset() {
    const cases = arrayOracleCases().map((c) => {
        const runs: TwoPassOracleExpectation[][] = [];
        for (const seed of c.seeds) {
            const rng = createRng(seed);
            const sequence: TwoPassOracleExpectation[] = [];
            const repeats = c.repeatRuns ?? 1;
            for (let r = 0; r < repeats; r++) {
                for (const owner of c.owners) {
                    const { expectation } = evaluateTwoPassArray(c.tokens, seed, owner, rng);
                    sequence.push(expectation);
                }
            }
            runs.push(sequence);
        }
        return { ...c, runs };
    });
    return {
        cases,
        unsafeCases: unsafeMismatchCases(),
    };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    process.stdout.write(`${JSON.stringify(buildOracleDataset())}\n`);
}
