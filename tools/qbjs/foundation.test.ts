// NOT ORIGINAL - these helper assertions also execute in the native CTest bundle.
import { test } from "node:test";
import { runInNewContext } from "node:vm";
import { foundationBundle } from "./foundation-fixtures.ts";

test("foundation helpers: bounded precision, typed fields and empty/script values", () => {
    runInNewContext(foundationBundle(), {}, { timeout: 5000 });
});
