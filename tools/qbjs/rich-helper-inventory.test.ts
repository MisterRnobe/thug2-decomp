// NOT ORIGINAL - generic synthetic inventory fixtures, not a game helper reconstruction.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { linkSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { emitIR } from "./emitter.ts";
import { command, field, integer, name, param, script, word } from "./fixtures.ts";
import { readIR } from "./reader.ts";
import { inventoryDirectory, inventoryModule } from "./rich-helper-inventory.ts";
import { checksum } from "./translate.ts";

const fixture = () =>
    Uint8Array.from([
        ...name("inventoryGlobal"),
        7,
        ...integer(9),
        1,
        ...script(
            "inventoryRoot",
            [
                ...command("inventoryChild", field("payload", param("input"))),
                ...command("GetArraySize", param("input")),
            ],
            field("fallback", [0x1b, ...word(4), 65, 66, 67, 0]),
        ),
        ...script("inventoryChild", command("UnknownInventoryHost", [14, ...name("inventoryGlobal"), 15])),
        0,
    ]);

test("inventory reports dependencies/default types without bodies, payloads or generated code", () => {
    const bytes = fixture(),
        before = Uint8Array.from(bytes),
        report = inventoryModule(bytes, "synthetic.qb");
    assert.equal(report.status, "supported");
    if (report.status !== "supported") throw Error("Expected supported module");
    assert.equal(report.definitions, 3);
    assert.equal(report.sha256, createHash("sha256").update(bytes).digest("hex"));
    assert.deepEqual(report.globalDefinitions, [checksum("inventoryGlobal")]);
    const root = report.scripts.find((s) => s.crc === checksum("inventoryRoot"))!;
    assert.ok(root.calls.find((c) => c.crc === checksum("inventoryChild"))?.localScript);
    assert.equal(root.calls.find((c) => c.crc === checksum("GetArraySize"))?.serviceHint, "GetArraySize");
    assert.deepEqual(root.parameters, [checksum("input")]);
    assert.deepEqual(root.literalTypes, [3]);
    assert.deepEqual(report.scripts[1]!.globals, [checksum("inventoryGlobal")]);
    assert.equal(root.requiresRandom, false);
    assert.doesNotMatch(JSON.stringify(report), /ABC|members|javascript|"body"|"bits"/);
    assert.deepEqual(bytes, before);
    assert.deepEqual(inventoryModule(bytes, "synthetic.qb"), report);
});

test("unsupported later definition rejects the whole module rather than extracting a useful first script", () => {
    const bytes = Uint8Array.from([...script("goodInventory", command("GetArraySize", param("input"))), 0x43, 0]);
    const report = inventoryModule(bytes, "whole.qb");
    assert.equal(report.status, "unsupported");
    if (report.status !== "unsupported") throw Error("Expected unsupported");
    assert.match(report.diagnostic, /whole.qb: QB offset .*unsupported definition token/);
    assert.equal("scripts" in report, false);
    assert.equal(inventoryModule(Uint8Array.of(0x23), "truncated.qb").status, "unsupported");
});

test("directory inventory sorts relative paths, skips symlinks and diagnoses oversized inputs", () => {
    const root = mkdtempSync(join(tmpdir(), "qb-inventory-"));
    try {
        mkdirSync(join(root, "sub"));
        writeFileSync(join(root, "z.QB"), fixture());
        writeFileSync(join(root, "sub", "a.qb"), Buffer.from([0x23]));
        writeFileSync(join(root, "ignored.txt"), "not QB");
        writeFileSync(join(root, "large.qb"), Buffer.alloc(4 * 1024 * 1024 + 1));
        symlinkSync(root, join(root, "loop"));
        symlinkSync(join(root, "z.QB"), join(root, "alias.qb"));
        const report = inventoryDirectory(root);
        assert.equal(report.files, 3);
        assert.equal(report.skippedSymlinks, 2);
        assert.deepEqual(
            report.modules.map((m) => m.module),
            ["large.qb", "sub/a.qb", "z.QB"],
        );
        assert.deepEqual(
            report.modules.map((m) => m.status),
            ["unreadable", "unsupported", "supported"],
        );
        assert.deepEqual(readFileSync(join(root, "z.QB")), Buffer.from(fixture()));
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
});

test("inventory refuses excessive directory depth", () => {
    const root = mkdtempSync(join(tmpdir(), "qb-inventory-depth-"));
    try {
        mkdirSync(join(root, ...Array<string>(17).fill("sub")), { recursive: true });
        assert.throws(() => inventoryDirectory(root), /directory depth limit/);
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
});

test("CLI creates metadata only and preserves inputs/old reports including hardlinks and symlinks", () => {
    const root = mkdtempSync(join(tmpdir(), "qb-inventory-cli-"));
    try {
        const data = join(root, "data"),
            output = join(root, "private.json"),
            cli = fileURLToPath(new URL("./rich-helper-inventory.ts", import.meta.url));
        mkdirSync(data);
        const input = join(data, "source.qb");
        writeFileSync(input, fixture());
        const run = (path: string) => execFileSync(process.execPath, [cli, data, path], { stdio: "pipe" });
        run(output);
        const report = readFileSync(output);
        assert.equal(JSON.parse(report.toString()).files, 1);
        assert.throws(() => run(output), /new .json/);
        assert.deepEqual(readFileSync(output), report);
        assert.throws(() => run(join(data, "new.json")), /outside the input/);
        const hardlink = join(root, "hardlink.json");
        linkSync(input, hardlink);
        assert.throws(() => run(hardlink), /new .json/);
        const symlink = join(root, "symlink.json");
        symlinkSync(input, symlink);
        assert.throws(() => run(symlink), /new .json/);
        assert.deepEqual(readFileSync(input), Buffer.from(fixture()));
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
});

test("inventoried synthetic module executes on embedded QuickJS", { skip: !process.env.QB_QUICKJS_DRIVER }, () => {
    const root = mkdtempSync(join(tmpdir(), "qb-inventory-quickjs-"));
    try {
        const bytes = fixture();
        assert.equal(inventoryModule(bytes, "synthetic.qb").status, "supported");
        const source = ["values.js", "runtime.js"]
            .map((name) => readFileSync(new URL(`../../scripting/${name}`, import.meta.url), "utf8"))
            .join("\n");
        const body = `
            function requireInventory(x) { if (!x) throw Error('Synthetic inventory assertion'); }
            let calls = 0;
            globalThis.__qbHost = (name, args, caller) => {
                ++calls;
                if (name === ${checksum("GetArraySize")}) {
                    requireInventory(args[0][1].type === 12 && args[0][1].elements.length === 3);
                    return {condition:true, outputs:[[${checksum("count")}, {type:1,bits:3}]]};
                }
                requireInventory(name === ${checksum("UnknownInventoryHost")});
                requireInventory(args[0][1].bits === 9);
                requireInventory(caller.find(([key, value]) => key === ${checksum("payload")} && value.type === 12)[1].elements.length === 3);
                return {condition:true, outputs:[]};
            };
            const data = {type:12, elementType:1, elements:[1,2,3].map(bits => ({type:1,bits}))};
            const id = qb.start(${checksum("inventoryRoot")}, [[${checksum("input")}, data]], 10000);
            const result = qb.update(id, 0);
            requireInventory(result.status === 'finished' && calls === 2);
            requireInventory(result.parameters.find(([key, value]) => key === ${checksum("count")} && value.type === 1)[1].bits === 3);
            globalThis.qbIntegrationTestResult = {completed:true};
        `;
        const output = join(root, "synthetic.js");
        writeFileSync(output, source + "\n" + emitIR(readIR(bytes, "synthetic.qb")) + "\n" + body);
        execFileSync(process.env.QB_QUICKJS_DRIVER!, [output], { stdio: "pipe" });
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
});
