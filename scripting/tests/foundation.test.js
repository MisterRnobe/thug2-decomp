// NOT ORIGINAL - independent expectations for foundation helper regressions.
(() => {
    const check = (ok, why) => {
        if (!ok) throw Error("Foundation regression: " + why);
    };
    const v = qbValues,
        n = qbNumeric;
    let work = 0;
    const ctx = {
        site: { source: "review.qb", script: 1, offset: 7 },
        owner: 1,
        symbol: () => undefined,
        charge: (k) => {
            work += k;
        },
    };
    const large = v.fields(
        [],
        Array.from({ length: 4096 }, (_, i) => () => [i + 1, v.integer(i)]),
        ctx,
    );
    check(large.length === 4096 && large[4095][1].bits === 4095, "documented field cap is reachable");
    check(work < 50000, "field construction has linear work cost");
    let limited = false;
    try {
        v.fields(
            [],
            Array.from({ length: 4097 }, (_, i) => () => [i + 1, v.integer(i)]),
            ctx,
        );
    } catch {
        limited = true;
    }
    check(limited, "field cap still enforced");
    check(v.binary(11, undefined, v.integer(1), ctx) === undefined, "empty operand propagates");
    check(!v.statementTruth(undefined, ctx), "empty condition is false");
    check(v.fields([], [() => [1, undefined]], ctx).length === 0, "empty expression omits argument");
    check(
        n.store32(n.round((1n << 80n) + (1n << 56n) + 1n, 1n, -80, 24)) === 0x3f800001,
        "precision24 avoids double rounding",
    );
    for (const precision of [53, 64])
        check(
            n.store32(n.round((1n << 80n) + (1n << 56n) + 1n, 1n, -80, precision)) === 0x3f800000,
            "offline double rounding profile",
        );
    check(n.store32({ m: 1n, e: -149 }) === 1, "minimum subnormal");
    check(n.store32({ m: 1n, e: -150 }) === 0, "subnormal tie rounds even");
    check(n.store32({ m: 3n, e: -150 }) === 2, "odd subnormal tie");
    check(n.store32({ m: 0n, e: 0 }, true) === 0x80000000, "negative zero");
    check(n.store32({ m: 1n, e: 128 }) === 0x7f800000, "overflow");
    for (const fn of [
        () => n.round(1n, 0n, 0),
        () => n.round(1n, 1n, 0, 25),
        () => n.round(1n << 4096n, 1n, 0),
        () => n.store32({ m: 1n, e: 99999 }),
    ]) {
        let failed = false;
        try {
            fn();
        } catch {
            failed = true;
        }
        check(failed, "numeric input limits");
    }
    check(v.fields([], [()=>[0x9d2d0915,v.vector([0,0x80000000,0]),false,true]],ctx).length===0,'literal angles marker omits near-zero vector');
    check(v.fields([], [()=>[0x9d2d0915,v.vector([0,0,0])]],ctx).length===1,'unmarked expression vector remains');
    const qctx={...ctx,symbol:key=>key===10?{kind:'callable',type:7,name:10}:key===11?{kind:'callable',type:7,name:11}:undefined};
    check(v.global(10,qctx).type===13&&v.global(10,qctx).bits===10,'script expression remains Name');
    check(v.binary(7,v.name(10),v.name(10),qctx).bits===1,'script identity equality');
    check(v.binary(7,v.name(10),v.name(11),qctx).bits===0,'different script identity');
    globalThis.qbFoundationTestResult={completed:true,emitterAdapterPending:true};
})();
