// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import { westCwd, findSdkRoot } from "./util";

/** Run a `west ...` invocation in a dedicated terminal so the user
 * sees the live output and can Ctrl-C / re-run via terminal history. */
function runInTerminal(name: string, command: string): void {
    const cwd = westCwd();
    const env: { [key: string]: string } = {};
    const sdk = findSdkRoot();
    if (sdk) env["EXTRA_ZEPHYR_MODULES"] = sdk;

    const existing = vscode.window.terminals.find((t) => t.name === name);
    const term = existing ?? vscode.window.createTerminal({ name, cwd: cwd ?? undefined, env });
    term.show(true);
    term.sendText(command);
}

async function pickBoardAndExamplePath(): Promise<{ board: string; example: string } | null> {
    const board = await vscode.window.showInputBox({
        prompt: "Zephyr board target (e.g. native_sim/native/64, alp_e1m_evk_aen)",
        value: "native_sim/native/64",
    });
    if (!board) return null;
    const example = await vscode.window.showInputBox({
        prompt: "Path to the application (relative to the west cwd)",
        value: "examples/gpio-button-led",
    });
    if (!example) return null;
    return { board, example };
}

async function westBuild(): Promise<void> {
    const sel = await pickBoardAndExamplePath();
    if (!sel) return;
    runInTerminal("alp · west build",
        `west build -b ${sel.board} ${sel.example} -p auto`);
}

async function westFlash(): Promise<void> {
    runInTerminal("alp · west flash", "west flash");
}

async function westRunNativeSim(): Promise<void> {
    runInTerminal("alp · west run", "west build -t run");
}

export function registerWestCommands(): vscode.Disposable[] {
    return [
        vscode.commands.registerCommand("alp.westBuild", () => westBuild()),
        vscode.commands.registerCommand("alp.westFlash", () => westFlash()),
        vscode.commands.registerCommand("alp.westRunNativeSim", () => westRunNativeSim()),
    ];
}
