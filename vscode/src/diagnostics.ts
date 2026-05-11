// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import * as cp from "child_process";
import * as path from "path";
import { findSdkRoot, pythonBinary, log } from "./util";

/**
 * Surfaces `validate_board_yaml.py` failures as inline diagnostics
 * on the active board.yaml file -- the same content the *Alp:
 * Validate board.yaml* command writes to the output channel, but
 * pinned to the editor so they appear in the Problems panel and
 * as a squiggle on the document.
 *
 * Trigger points: every save + every open of a file named
 * `board.yaml`.  The validator runs in <100 ms locally so this is
 * cheap; if it ever isn't, we'll debounce or move to onWillSave.
 *
 * The Red Hat YAML extension we depend on already covers JSON-
 * Schema violations as inline diagnostics; this layer adds the
 * checks the schema can't express -- missing SoM preset, missing
 * carrier preset without inline `populated`, and the v0.3
 * hw_rev / SDK-version compatibility window.
 *
 * Line ranges: the validator emits text like
 *     `FAIL som preset: no preset for E1M-NX9999 at ...`
 * without a column.  We attach all entries to line 1 of the
 * document so the user gets a Problems-panel summary; opening the
 * file shows a single squiggle on the first line linking to the
 * full text.  Targeted ranges per field land in v0.4 when we add
 * a YAML AST parser (js-yaml-ast or similar) to map field paths.
 */

const SEVERITY: Record<number, vscode.DiagnosticSeverity> = {
    1: vscode.DiagnosticSeverity.Error,    // schema violation
    2: vscode.DiagnosticSeverity.Warning,  // missing-preset
    3: vscode.DiagnosticSeverity.Error,    // hw-rev incompatibility
};

function isBoardYaml(doc: vscode.TextDocument): boolean {
    return path.basename(doc.uri.fsPath).toLowerCase() === "board.yaml";
}

function validate(
    doc: vscode.TextDocument,
    collection: vscode.DiagnosticCollection,
): void {
    if (!isBoardYaml(doc)) return;
    const sdk = findSdkRoot();
    if (!sdk) {
        collection.delete(doc.uri);
        return;
    }
    const validator = path.join(sdk, "scripts", "validate_board_yaml.py");
    const result = cp.spawnSync(pythonBinary(),
        [validator, "--input", doc.uri.fsPath],
        { encoding: "utf8" });
    log(`$ ${pythonBinary()} ${validator} --input ${doc.uri.fsPath} (rv=${result.status})`);

    if (result.status === 0) {
        collection.delete(doc.uri);
        return;
    }

    const severity = SEVERITY[result.status ?? 1]
        ?? vscode.DiagnosticSeverity.Error;
    const lines = (result.stderr || "").split(/\r?\n/)
        .filter((l) => l.trim().length > 0)
        // Drop the trailing summary line; keep the per-failure body.
        .filter((l) => !/^\s*\S+:\s+missing-preset/.test(l)
                    && !/^\s*\S+:\s+hardware-revision/.test(l));

    // Pin everything to line 0 -- columnless validator output, so a
    // single squiggle on the first line is the honest representation.
    const range = new vscode.Range(0, 0, 0, doc.lineAt(0).text.length);
    const diags = lines.map((message) =>
        new vscode.Diagnostic(range, message.trim(), severity));
    collection.set(doc.uri, diags);
}

export function registerDiagnostics(
    context: vscode.ExtensionContext,
): vscode.Disposable {
    const collection = vscode.languages.createDiagnosticCollection("alp-sdk");
    context.subscriptions.push(collection);

    // Validate on open + save + the active editor's initial focus,
    // so the first time the user touches board.yaml the Problems
    // panel reflects current state.
    const disposables: vscode.Disposable[] = [
        vscode.workspace.onDidOpenTextDocument(
            (doc) => validate(doc, collection)),
        vscode.workspace.onDidSaveTextDocument(
            (doc) => validate(doc, collection)),
    ];
    for (const editor of vscode.window.visibleTextEditors) {
        validate(editor.document, collection);
    }

    return vscode.Disposable.from(...disposables, collection);
}
