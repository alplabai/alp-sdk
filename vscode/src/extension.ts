// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";
import { registerLoaderCommands } from "./loader";
import { registerWestCommands } from "./west";
import { registerBootstrapCommand } from "./bootstrap";
import { createStatusBar } from "./statusBar";
import { registerConfiguratorCommand } from "./configuratorPanel";
import { registerDiagnostics } from "./diagnostics";

export function activate(context: vscode.ExtensionContext): void {
    context.subscriptions.push(
        ...registerLoaderCommands(),
        ...registerWestCommands(),
        registerBootstrapCommand(),
        createStatusBar(context),
        registerConfiguratorCommand(context),
        registerDiagnostics(context),
    );
}

export function deactivate(): void {
    // No-op -- all subscriptions are tied to ExtensionContext.
}
