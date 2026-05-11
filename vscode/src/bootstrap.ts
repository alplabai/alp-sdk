// SPDX-License-Identifier: Apache-2.0

import * as vscode from "vscode";

type Host = "linux" | "darwin" | "win32";

interface BootstrapPlan {
    title: string;
    /** One terminal command per logical step.  Each is shown to the
     * user before running so they can opt out / edit. */
    steps: { description: string; command: string }[];
    /** Documentation links the user should follow for steps the
     * extension can't automate (vendor-licensed toolchains). */
    pointers: { name: string; url: string }[];
}

function planForHost(host: Host, os: "zephyr" | "yocto" | "baremetal"): BootstrapPlan {
    const pythonDeps = host === "win32"
        ? { description: "Install loader Python deps (pip)",
            command: "python -m pip install --user pyyaml jsonschema" }
        : { description: "Install loader Python deps (pip3)",
            command: "pip3 install --user pyyaml jsonschema" };

    const west = host === "win32"
        ? { description: "Install `west`", command: "python -m pip install --user west" }
        : { description: "Install `west`", command: "pip3 install --user west" };

    if (os === "zephyr") {
        return {
            title: `Bootstrap ALP SDK (Zephyr, ${host})`,
            steps: [pythonDeps, west],
            pointers: [
                {
                    name: "Zephyr SDK installer",
                    url: "https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html",
                },
                {
                    name: "Zephyr getting started guide",
                    url: "https://docs.zephyrproject.org/latest/develop/getting_started/index.html",
                },
            ],
        };
    }
    if (os === "yocto") {
        const yoctoStep = host === "linux"
            ? {
                description: "Install Yocto host packages (Ubuntu / Debian apt)",
                command: "sudo apt-get update && sudo apt-get install -y "
                       + "gawk wget git diffstat unzip texinfo gcc build-essential "
                       + "chrpath socat cpio python3 python3-pip python3-pexpect "
                       + "xz-utils debianutils iputils-ping python3-git python3-jinja2 "
                       + "libegl1-mesa libsdl1.2-dev pylint xterm python3-subunit "
                       + "mesa-common-dev zstd liblz4-tool file locales",
            }
            : {
                description: "Yocto host build is Linux-only.  Use a VM / Docker container.",
                command: "echo 'See pointers below'",
            };
        return {
            title: `Bootstrap ALP SDK (Yocto, ${host})`,
            steps: [pythonDeps, yoctoStep],
            pointers: [
                {
                    name: "Yocto Project quick build",
                    url: "https://docs.yoctoproject.org/brief-yoctoprojectqs/index.html",
                },
                {
                    name: "Yocto host requirements",
                    url: "https://docs.yoctoproject.org/ref-manual/system-requirements.html",
                },
            ],
        };
    }
    // baremetal
    return {
        title: `Bootstrap ALP SDK (baremetal, ${host})`,
        steps: [pythonDeps],
        pointers: [
            { name: "Alif Ensemble dev tools",
              url: "https://alifsemi.com/support/software-development-kit/" },
            { name: "Renesas RZ/V2N CMSIS-Driver pack",
              url: "https://www.renesas.com/us/en/software-tool/flexible-software-package-fsp" },
            { name: "NXP MCUXpresso for i.MX 93",
              url: "https://www.nxp.com/design/software/mcuxpresso-software-and-tools/" },
        ],
    };
}

async function pickOs(): Promise<"zephyr" | "yocto" | "baremetal" | null> {
    const items: vscode.QuickPickItem[] = [
        { label: "zephyr",    description: "Zephyr RTOS (west + Zephyr SDK)" },
        { label: "yocto",     description: "Yocto / OpenEmbedded user-space (Linux host required)" },
        { label: "baremetal", description: "No OS; vendor toolchains (Alif / Renesas / NXP)" },
    ];
    const pick = await vscode.window.showQuickPick(items, {
        title: "Alp: Which OS path do you want to bootstrap for?",
        placeHolder: "Pick the os: value matching your board.yaml.",
    });
    return (pick?.label as "zephyr" | "yocto" | "baremetal" | undefined) ?? null;
}

async function runBootstrap(): Promise<void> {
    const os = await pickOs();
    if (!os) return;

    const host = (process.platform === "win32" ? "win32"
                 : process.platform === "darwin" ? "darwin"
                 : "linux") as Host;
    const plan = planForHost(host, os);

    const term = vscode.window.createTerminal({ name: plan.title });
    term.show(true);
    term.sendText(`# ${plan.title}`);
    for (const step of plan.steps) {
        term.sendText(`# ${step.description}`);
        term.sendText(step.command);
    }

    if (plan.pointers.length > 0) {
        term.sendText("# Manual follow-ups (license-walled / vendor-specific):");
        for (const p of plan.pointers) {
            term.sendText(`#   - ${p.name}: ${p.url}`);
        }
    }
}

export function registerBootstrapCommand(): vscode.Disposable {
    return vscode.commands.registerCommand("alp.installDependencies", () => runBootstrap());
}
