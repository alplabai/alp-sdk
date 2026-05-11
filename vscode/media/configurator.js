// SPDX-License-Identifier: Apache-2.0
//
// Webview client for the ALP board configurator.  Receives an
// `init` message from the extension host with the current board.yaml
// model + the preset catalogue (released SoM SKUs, carriers and
// their populated defaults, schema enums) and binds controls to it.

/* global acquireVsCodeApi */

(function () {
    "use strict";

    const vscode = acquireVsCodeApi();

    /** @type {object|null} */
    let model = null;
    /** @type {object|null} */
    let catalogue = null;
    /** @type {string|null} */
    let boardPath = null;

    function $(id) {
        const el = document.getElementById(id);
        if (!el) throw new Error("missing element: " + id);
        return el;
    }

    function fillSelect(el, values, current, placeholder) {
        el.innerHTML = "";
        if (placeholder !== undefined) {
            const opt = document.createElement("option");
            opt.value = "";
            opt.textContent = placeholder;
            el.appendChild(opt);
        }
        for (const v of values) {
            const opt = document.createElement("option");
            opt.value = v;
            opt.textContent = v;
            if (v === current) opt.selected = true;
            el.appendChild(opt);
        }
    }

    function carrierPreset(name) {
        if (!catalogue) return null;
        return catalogue.carriers.find((c) => c.name === name) || null;
    }

    function renderCarrierPopulated() {
        const host = $("carrier-populated");
        host.innerHTML = "";
        if (!model || !catalogue) return;

        const carrierName = (model.carrier && model.carrier.name) || "";
        const preset = carrierPreset(carrierName);
        // Source of truth for the row keys: union of (preset defaults,
        // user overrides) so both shipped chips and customer additions
        // appear in the UI.
        const merged = Object.assign({}, preset ? preset.populated : {},
            (model.carrier && model.carrier.populated) || {});
        const keys = Object.keys(merged).sort();
        for (const chip of keys) {
            const row = document.createElement("div");
            row.className = "populated-row";
            const label = document.createElement("label");
            const checkbox = document.createElement("input");
            checkbox.type = "checkbox";
            checkbox.checked = !!merged[chip];
            checkbox.addEventListener("change", () => {
                model.carrier = model.carrier || { name: carrierName };
                model.carrier.populated = model.carrier.populated || {};
                const presetDefault = preset && preset.populated[chip];
                if (presetDefault === checkbox.checked) {
                    // Match the preset default -- drop the override
                    // entry so the saved yaml stays minimal.
                    delete model.carrier.populated[chip];
                } else {
                    model.carrier.populated[chip] = checkbox.checked;
                }
            });
            label.appendChild(checkbox);
            label.appendChild(document.createTextNode(chip));
            row.appendChild(label);

            const source = document.createElement("span");
            source.className = "source";
            if (preset && chip in preset.populated) {
                source.textContent = "carrier preset";
            } else {
                source.textContent = "custom";
            }
            row.appendChild(source);
            host.appendChild(row);
        }
    }

    function renderLibraries() {
        const host = $("libraries");
        host.innerHTML = "";
        if (!catalogue) return;
        const selected = new Set(model.libraries || []);
        for (const lib of catalogue.libraries) {
            const row = document.createElement("div");
            row.className = "library-row";
            const label = document.createElement("label");
            const checkbox = document.createElement("input");
            checkbox.type = "checkbox";
            checkbox.checked = selected.has(lib);
            checkbox.addEventListener("change", () => {
                if (checkbox.checked) selected.add(lib);
                else selected.delete(lib);
                model.libraries = Array.from(selected.values()).sort();
            });
            label.appendChild(checkbox);
            label.appendChild(document.createTextNode(lib));
            row.appendChild(label);
            host.appendChild(row);
        }
    }

    function bindStatic() {
        const skuEl = $("som-sku");
        skuEl.addEventListener("change", () => {
            model.som = model.som || { sku: "" };
            model.som.sku = skuEl.value;
        });

        const carrierEl = $("carrier-name");
        carrierEl.addEventListener("change", () => {
            model.carrier = model.carrier || {};
            model.carrier.name = carrierEl.value;
            // When the carrier changes, drop the user's prior overrides
            // -- they're meaningless against a different preset.
            delete model.carrier.populated;
            renderCarrierPopulated();
        });

        const osEl = $("os-choice");
        osEl.addEventListener("change", () => {
            model.os = osEl.value;
        });

        const backendEl = $("inference-backend");
        backendEl.addEventListener("change", () => {
            if (backendEl.value === "") {
                if (model.inference) delete model.inference.backend;
            } else {
                model.inference = model.inference || {};
                model.inference.backend = backendEl.value;
            }
        });

        const arenaEl = $("inference-arena");
        arenaEl.addEventListener("change", () => {
            const val = parseInt(arenaEl.value, 10);
            if (Number.isFinite(val) && val >= 16) {
                model.inference = model.inference || {};
                model.inference.default_arena_kib = val;
            } else if (model.inference) {
                delete model.inference.default_arena_kib;
            }
        });

        for (const key of ["wifi", "mqtt", "ble", "tls"]) {
            const el = $("iot-" + key);
            el.addEventListener("change", () => {
                model.iot = model.iot || {};
                model.iot[key] = el.checked;
            });
        }

        const lastErrEl = $("diag-last-error");
        lastErrEl.addEventListener("change", () => {
            model.diagnostics = model.diagnostics || {};
            model.diagnostics.last_error = lastErrEl.checked;
        });

        const logEl = $("diag-log-level");
        logEl.addEventListener("change", () => {
            model.diagnostics = model.diagnostics || {};
            model.diagnostics.log_level = logEl.value;
        });

        $("save").addEventListener("click", () => {
            vscode.postMessage({ type: "save", payload: model });
        });
        $("reload").addEventListener("click", () => {
            vscode.postMessage({ type: "reload" });
        });
    }

    function applyModelToControls() {
        if (!model || !catalogue) return;
        fillSelect($("som-sku"), catalogue.skus,
            model.som && model.som.sku);
        fillSelect($("carrier-name"),
            catalogue.carriers.map((c) => c.name),
            model.carrier && model.carrier.name);
        fillSelect($("os-choice"), catalogue.osChoices, model.os);
        fillSelect($("inference-backend"), catalogue.inferenceBackends,
            model.inference && model.inference.backend,
            "(use SoM preset's preferred_backend)");
        fillSelect($("diag-log-level"), catalogue.logLevels,
            model.diagnostics && model.diagnostics.log_level);
        $("inference-arena").value =
            (model.inference && model.inference.default_arena_kib) || "";
        for (const key of ["wifi", "mqtt", "ble", "tls"]) {
            $("iot-" + key).checked =
                !!(model.iot && model.iot[key]);
        }
        $("diag-last-error").checked =
            !model.diagnostics || model.diagnostics.last_error !== false;
        renderCarrierPopulated();
        renderLibraries();
    }

    window.addEventListener("message", (evt) => {
        const msg = evt.data;
        if (msg.type === "init") {
            model = msg.model;
            catalogue = msg.catalogue;
            boardPath = msg.boardPath;
            applyModelToControls();
            $("status").textContent = "Editing " + boardPath;
        } else if (msg.type === "saved") {
            $("status").textContent = "Saved " + msg.boardPath;
        }
    });

    bindStatic();
})();
