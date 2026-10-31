// NOT ORIGINAL - helper-level foundation regressions. Emitted field-builder
// integration belongs to the separate emitter handoff; do not imply it ran here.
import { readFileSync } from "node:fs";
import { helperSource } from "./helper-sources.ts";
export function foundationBundle(): string {
    return (
        helperSource("random") +
        helperSource("values") +
        readFileSync(new URL("../../scripting/tests/foundation.test.js", import.meta.url), "utf8")
    );
}
