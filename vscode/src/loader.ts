// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import * as cp from "child_process";
import * as path from "path";
import * as fs from "fs";
import { findSdkRoot, pythonBinary, boardYamlPath, log, showOutput } from "./util";

type EmitMode = "zephyr-conf" | "cmake-args" | "yocto-conf" | "dts-overlay";

const DEFAULT_OUTPUT: Record<EmitMode, string> = {
    "zephyr-conf": "build/generated/alp.conf",
    "dts-overlay": "build/generated/alp.overlay",
    "cmake-args":  "build/generated/alp-cmake-args.txt",
    "yocto-conf":  "build/generated/alp-yocto.conf",
};

async function runLoader(emit: EmitMode): Promise<void> {
    const sdk = findSdkRoot();
    if (!sdk) {
        await vscode.window.showErrorMessage(
            "Alp: alp-sdk root not found.  Set `alpSdk.path` in settings " +
            "to the directory that contains scripts/alp_project.py.");
        return;
    }
    const board = boardYamlPath();
    if (!board) {
        await vscode.window.showErrorMessage("Alp: no workspace folder is open.");
        return;
    }
    if (!fs.existsSync(board)) {
        await vscode.window.showErrorMessage(`Alp: board.yaml not found at ${board}.`);
        return;
    }

    const folder = vscode.workspace.workspaceFolders![0]!.uri.fsPath;
    const output = path.join(folder, DEFAULT_OUTPUT[emit]);
    fs.mkdirSync(path.dirname(output), { recursive: true });

    const loader = path.join(sdk, "scripts", "alp_project.py");
    const args = ["--input", board, "--emit", emit, "--output", output];
    log(`$ ${pythonBinary()} ${loader} ${args.join(" ")}`);

    const result = cp.spawnSync(pythonBinary(), [loader, ...args], {
        encoding: "utf8",
    });
    if (result.stdout) log(result.stdout);
    if (result.stderr) log(result.stderr);

    if (result.status !== 0) {
        showOutput();
        await vscode.window.showErrorMessage(
            `Alp: ${emit} generation failed (rv=${result.status}).  See the ALP SDK output channel.`);
        return;
    }
    const doc = await vscode.workspace.openTextDocument(output);
    await vscode.window.showTextDocument(doc, { preview: true });
    vscode.window.setStatusBarMessage(`Alp: wrote ${path.relative(folder, output)}`, 5000);
}

async function runValidator(): Promise<void> {
    const sdk = findSdkRoot();
    if (!sdk) {
        await vscode.window.showErrorMessage(
            "Alp: alp-sdk root not found (set `alpSdk.path`).");
        return;
    }
    const board = boardYamlPath();
    if (!board || !fs.existsSync(board)) {
        await vscode.window.showErrorMessage(
            `Alp: board.yaml not found at ${board ?? "<unset>"}.`);
        return;
    }

    const validator = path.join(sdk, "scripts", "validate_board_yaml.py");
    log(`$ ${pythonBinary()} ${validator} --input ${board}`);
    const result = cp.spawnSync(pythonBinary(), [validator, "--input", board], {
        encoding: "utf8",
    });
    if (result.stdout) log(result.stdout);
    if (result.stderr) log(result.stderr);
    showOutput();

    if (result.status === 0) {
        await vscode.window.showInformationMessage("Alp: board.yaml is clean.");
    } else if (result.status === 2) {
        await vscode.window.showWarningMessage(
            "Alp: board.yaml has missing-preset failures.  See the ALP SDK output channel.");
    } else {
        await vscode.window.showErrorMessage(
            "Alp: board.yaml schema violation.  See the ALP SDK output channel.");
    }
}

export function registerLoaderCommands(): vscode.Disposable[] {
    return [
        vscode.commands.registerCommand("alp.generateZephyrConf", () => runLoader("zephyr-conf")),
        vscode.commands.registerCommand("alp.generateDtsOverlay", () => runLoader("dts-overlay")),
        vscode.commands.registerCommand("alp.generateCmakeArgs",  () => runLoader("cmake-args")),
        vscode.commands.registerCommand("alp.generateYoctoConf",  () => runLoader("yocto-conf")),
        vscode.commands.registerCommand("alp.validateBoardYaml",  () => runValidator()),
    ];
}
