// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import * as path from "path";
import * as fs from "fs";

const OUTPUT = vscode.window.createOutputChannel("ALP SDK");

export function log(line: string): void {
    OUTPUT.appendLine(line);
}

export function showOutput(): void {
    OUTPUT.show(true);
}

/** Resolve `alpSdk.path` from settings, falling back to a workspace
 * directory containing `scripts/alp_project.py`.  Returns null if
 * neither finds a match. */
export function findSdkRoot(): string | null {
    const cfg = vscode.workspace.getConfiguration("alpSdk");
    const configured = cfg.get<string>("path", "").trim();
    if (configured && fs.existsSync(path.join(configured, "scripts", "alp_project.py"))) {
        return configured;
    }

    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        const candidate = folder.uri.fsPath;
        if (fs.existsSync(path.join(candidate, "scripts", "alp_project.py"))) {
            return candidate;
        }
        // Common case: app workspace alongside or inside an alp-sdk checkout.
        const sibling = path.join(candidate, "..", "alp-sdk");
        if (fs.existsSync(path.join(sibling, "scripts", "alp_project.py"))) {
            return path.resolve(sibling);
        }
    }
    return null;
}

/** Resolve the Python interpreter -- explicit setting wins; otherwise
 * `python3` on POSIX and `python` on Windows. */
export function pythonBinary(): string {
    const cfg = vscode.workspace.getConfiguration("alpSdk");
    const configured = cfg.get<string>("pythonPath", "").trim();
    if (configured) return configured;
    return process.platform === "win32" ? "python" : "python3";
}

/** Absolute path to the project board.yaml, defaulting to
 * `<workspace>/<alpSdk.boardYamlPath>`. */
export function boardYamlPath(): string | null {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) return null;
    const cfg = vscode.workspace.getConfiguration("alpSdk");
    const rel = cfg.get<string>("boardYamlPath", "board.yaml");
    return path.isAbsolute(rel) ? rel : path.join(folders[0]!.uri.fsPath, rel);
}

/** Working directory for `west build/flash/run` -- explicit setting
 * wins; otherwise the first workspace folder. */
export function westCwd(): string | null {
    const cfg = vscode.workspace.getConfiguration("alpSdk");
    const configured = cfg.get<string>("westCwd", "").trim();
    if (configured) return configured;
    const folders = vscode.workspace.workspaceFolders ?? [];
    return folders.length > 0 ? folders[0]!.uri.fsPath : null;
}

/** Show an info/warn message tied to a follow-up action. */
export async function offerAction(
    message: string,
    action: string,
    severity: "info" | "warn" = "info",
): Promise<boolean> {
    const show = severity === "warn" ? vscode.window.showWarningMessage
                                     : vscode.window.showInformationMessage;
    const pick = await show(message, action);
    return pick === action;
}
