// NOT ORIGINAL - read-only, bounded candidate metadata; never exports game bodies/IR/JS.
import { createHash } from "node:crypto";
import { existsSync, opendirSync, realpathSync, writeFileSync } from "node:fs";
import { basename, dirname, extname, join, relative, resolve, sep } from "node:path";
import { pathToFileURL } from "node:url";
import { emitIR, requiresRandom } from "./emitter.ts";
import type { Definition } from "./ir.ts";
import { readIR, readQBFile } from "./reader.ts";
import { checksum } from "./translate.ts";

const maxFiles = 4096,
    maxEntries = 100000,
    maxBytes = 256 * 1024 * 1024,
    maxReportBytes = 32 * 1024 * 1024;
const services = ["ArrayContains", "GetArraySize", "GetNextArrayElement", "GetUpperCaseString", "StringToCharArray"];
const serviceNames = new Map(services.map((name) => [checksum(name), name]));

export function inventoryModule(bytes: Uint8Array, source: string) {
    const metadata = { module: source, bytes: bytes.length, sha256: createHash("sha256").update(bytes).digest("hex") };
    try {
        const ir = readIR(bytes, source);
        // Check the complete module, not a conveniently extracted script.
        emitIR(ir);
        const scripts = ir.definitions.filter((d) => d.kind === "script");
        const localScripts = new Set(scripts.map((d) => d.name));
        const inspect = (definition: Definition) => {
            const calls = new Set<number>(),
                globals = new Set<number>(),
                parameters = new Set<number>(),
                literalTypes = new Set<number>(),
                kinds = new Set<string>();
            let nodes = 0;
            const stack: unknown[] = [definition];
            while (stack.length) {
                const value = stack.pop();
                if (!value || typeof value !== "object") continue;
                if (++nodes > 250000) throw Error("Inventory traversal node limit");
                if (Array.isArray(value)) {
                    for (const item of value) stack.push(item);
                    continue;
                }
                const record = value as Record<string, unknown>;
                if (typeof record.kind === "string") kinds.add(record.kind);
                if (record.kind === "call") calls.add(record.name as number);
                if (record.kind === "global" && value !== definition) globals.add(record.name as number);
                if (record.kind === "parameter") parameters.add(record.name as number);
                if (record.kind === "literal") literalTypes.add((record.value as { type: number }).type);
                if (record.kind !== "literal") stack.push(...Object.values(record));
            }
            return {
                crc: definition.name,
                offset: definition.offset,
                nodes,
                calls: [...calls]
                    .sort((a, b) => a - b)
                    .map((crc) => ({
                        crc,
                        localScript: localScripts.has(crc),
                        serviceHint: serviceNames.get(crc) ?? null,
                    })),
                globals: [...globals].sort((a, b) => a - b),
                parameters: [...parameters].sort((a, b) => a - b),
                literalTypes: [...literalTypes].sort((a, b) => a - b),
                requiresRandom: requiresRandom({ ...ir, definitions: [definition] }),
                kinds: [...kinds].sort(),
            };
        };
        return {
            ...metadata,
            status: "supported" as const,
            definitions: ir.definitions.length,
            requiresRandom: requiresRandom(ir),
            globalDefinitions: ir.definitions.filter((d) => d.kind === "global").map((d) => d.name),
            scripts: scripts.map(inspect),
        };
    } catch (error) {
        return { ...metadata, status: "unsupported" as const, diagnostic: String(error) };
    }
}

export function inventoryDirectory(directory: string) {
    const root = realpathSync(directory),
        pending = [{ path: root, depth: 0 }],
        files: string[] = [];
    let entries = 0,
        skippedSymlinks = 0;
    while (pending.length) {
        const current = pending.pop()!,
            dir = opendirSync(current.path);
        try {
            for (let entry = dir.readSync(); entry !== null; entry = dir.readSync()) {
                if (++entries > maxEntries) throw Error("Inventory directory entry limit");
                if (entry.isSymbolicLink()) {
                    ++skippedSymlinks;
                    continue;
                }
                const path = join(current.path, entry.name);
                if (entry.isDirectory()) {
                    if (current.depth >= 16) throw Error("Inventory directory depth limit");
                    pending.push({ path, depth: current.depth + 1 });
                } else if (entry.isFile() && extname(entry.name).toLowerCase() === ".qb") {
                    if (files.length >= maxFiles) throw Error("Inventory QB file limit");
                    files.push(path);
                }
            }
        } finally {
            dir.closeSync();
        }
    }
    let totalBytes = 0,
        reportBytes = 0;
    const chargeReport = <T>(metadata: T): T => {
        reportBytes += Buffer.byteLength(JSON.stringify(metadata, null, 2));
        if (reportBytes > maxReportBytes) throw Error("Inventory report exceeds 32 MiB");
        return metadata;
    };
    const modules = files.sort().map((path) => {
        const source = relative(root, path).split(sep).join("/");
        let bytes: Uint8Array;
        try {
            bytes = readQBFile(path);
        } catch (error) {
            return chargeReport({ module: source, status: "unreadable" as const, diagnostic: String(error) });
        }
        totalBytes += bytes.length;
        if (totalBytes > maxBytes) throw Error("Inventory total byte limit");
        return chargeReport(inventoryModule(bytes, source));
    });
    return { format: 1, files: files.length, totalBytes, skippedSymlinks, modules };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const [, , data, output] = process.argv;
    if (!data || !output || process.argv.length !== 4)
        throw Error("Usage: node tools/qbjs/rich-helper-inventory.ts DATA_DIRECTORY NEW_PRIVATE_REPORT.json");
    const root = realpathSync(data),
        target = join(realpathSync(dirname(resolve(output))), basename(output)),
        rel = relative(root, target);
    if (rel === "" || (!rel.startsWith(`..${sep}`) && rel !== ".."))
        throw Error("Report must be outside the input directory");
    if (extname(target) !== ".json" || existsSync(target)) throw Error("Report must be a new .json file");
    const inventory = inventoryDirectory(root),
        report = JSON.stringify(inventory, null, 2) + "\n";
    if (Buffer.byteLength(report) > maxReportBytes) throw Error("Inventory report exceeds 32 MiB");
    // Exclusive creation also refuses existing files/hardlinks/symlinks after scanning.
    writeFileSync(target, report, { flag: "wx" });
    process.stdout.write(JSON.stringify({ files: inventory.files, report: target }) + "\n");
}
