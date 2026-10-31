// NOT ORIGINAL - format-2 translation IR. No runtime references or PC pointers.
export type ScalarLiteral = { type: 1 | 2 | 13; bits: number };
export type Literal = ScalarLiteral | { type: 3 | 4; bytes: number[] } | { type: 5 | 6; coordinates: number[] };
export type Expr =
    | { kind: "literal"; offset: number; value: Literal }
    | { kind: "expressionResult"; offset: number; value: Expr }
    | { kind: "parameter"; offset: number; name: number; mode: "value" | "expression" }
    | { kind: "global"; offset: number; name: number }
    | { kind: "allParameters"; offset: number }
    | { kind: "binary"; offset: number; op: number; left: Expr; right: Expr }
    | { kind: "randomRange"; offset: number; a: number; b: number; second: boolean }
    | { kind: "randomChoice"; offset: number; opcode: number; weights: number[]; choices: Expr[] }
    | { kind: "structure"; offset: number; fields: Field[] }
    | { kind: "array"; offset: number; items: Expr[] };
export type Field =
    | { name: number; value: Expr; offset: number; flattenStructure?: true }
    | { spread: true; offset: number };
// A `name:command` statement (token 0x42 after the command name). The
// reader preserves the object name and original offsets; the emitter
// reports an explicit fault at execution instead of a global call.
export type QualifiedCall = {
    kind: "qualified";
    offset: number;
    object: number;
    objectOffset: number;
    commandOffset: number;
    name: number;
    fields: Field[];
};
// CScript::ReadName (0x004729E0) accepts three name forms: a literal checksum
// (`16 <crc>`), a checksum read from a named parameter (`2d 16 <param-crc>`)
// and the unnamed checksum parameter (`2c`). The last two resolve only at
// run time, so their IR keeps the source distinction and the CRC that was
// read from, never a dispatch target.
export type NameSource =
    | { kind: "nameLiteral"; offset: number; name: number }
    | { kind: "nameParameter"; offset: number; parameter: number }
    | { kind: "nameUnnamed"; offset: number };
// A command whose name (or the object of its `name:command` qualifier) comes
// from a parameter, per CScript::ExecuteCommand's ReadName-based dispatch.
// Dynamic dispatch is not implemented: loading a module containing one
// succeeds, and the emitter faults with the original offsets when it runs.
export type DynamicCall = {
    kind: "dynamic";
    offset: number;
    object?: NameSource;
    command: NameSource;
    commandOffset: number;
    fields: Field[];
};
export type Call = { kind: "call"; offset: number; name: number; fields: Field[] };
export type CommandStatement = Call | QualifiedCall | DynamicCall;
export type SwitchLabel = { offset: number; fields: Field[] };
export type Statement =
    | Call
    | QualifiedCall
    | DynamicCall
    | { kind: "assign"; offset: number; name: number; value: Expr }
    | { kind: "return"; offset: number; fields: Field[] }
    // Preserve the complete repeat argument list: GetInteger(0) is applied
    // after typed argument merging, not to the first syntactic argument.
    | { kind: "loop"; offset: number; body: Statement[]; repeatOffset: number; repeatFields: Field[] }
    | { kind: "break"; offset: number; target: number }
    | {
          kind: "switch";
          offset: number;
          fields: Field[];
          cases: { labels: SwitchLabel[]; body: Statement[] }[];
          fallback?: Statement[];
      }
    // CScript::ExecuteIf: token 0x39 negates the condition after its single
    // evaluation (effects and faults are preserved). `negate: true` marks
    // that prefix; the flag is absent for plain `if`.
    | {
          kind: "if";
          offset: number;
          condition: CommandStatement | Expr;
          negate?: true;
          yes: Statement[];
          no: Statement[];
      }
    | { kind: "random"; offset: number; opcode: number; weights: number[]; branches: Statement[][] };
export type Definition =
    | { kind: "script"; offset: number; name: number; defaults: Field[]; body: Statement[] }
    | { kind: "global"; offset: number; name: number; value: Expr };
export type ModuleIR = { format: 2; source: string; definitions: Definition[] };
