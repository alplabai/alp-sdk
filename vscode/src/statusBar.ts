// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import * as fs from "fs";
import * as yaml from "js-yaml";
import { boardYamlPath, log } from "./util";

interface BoardSummary {
    sku?: string;
    carrier?: string;
    os?: string;
}

function summarise(path: string | null): BoardSummary | null {
    if (!path || !fs.existsSync(path)) return null;
    try {
        const doc = yaml.load(fs.readFileSync(path, "utf-8"));
        if (!doc || typeof doc !== "object") return null;
        const obj = doc as Record<string, unknown>;
        const som = (obj["som"] as Record<string, unknown> | undefined);
        const carrier = (obj["carrier"] as Record<string, unknown> | undefined);
        return {
            sku:     (som?.["sku"] as string | undefined) ?? undefined,
            carrier: (carrier?.["name"] as string | undefined) ?? undefined,
            os:      (obj["os"] as string | undefined) ?? undefined,
        };
    } catch (e) {
        log(`statusBar: failed to parse ${path}: ${e}`);
        return null;
    }
}

function refresh(item: vscode.StatusBarItem): void {
    const summary = summarise(boardYamlPath());
    if (!summary || !summary.sku) {
        item.text = "$(circuit-board) Alp: no board.yaml";
        item.tooltip = "Open the board configurator with `Alp: Open board configurator`.";
        item.command = "alp.openConfigurator";
        item.show();
        return;
    }
    const parts = [summary.sku];
    if (summary.carrier) parts.push(summary.carrier);
    if (summary.os) parts.push(summary.os);
    item.text = `$(circuit-board) ${parts.join(" · ")}`;
    item.tooltip = "Click to open the ALP board configurator.";
    item.command = "alp.openConfigurator";
    item.show();
}

export function createStatusBar(context: vscode.ExtensionContext): vscode.Disposable {
    const item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    refresh(item);

    const watcher = vscode.workspace.createFileSystemWatcher("**/board.yaml");
    watcher.onDidChange(() => refresh(item));
    watcher.onDidCreate(() => refresh(item));
    watcher.onDidDelete(() => refresh(item));
    context.subscriptions.push(watcher);

    return item;
}
