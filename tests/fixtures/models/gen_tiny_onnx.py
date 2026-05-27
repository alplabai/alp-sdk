# tests/fixtures/models/gen_tiny_onnx.py
"""Generate the tiny CNN .onnx fixture for the DEEPX dxcom e2e test.

Run to (re)create tiny_cnn.onnx (a 2-conv classifier, input [1,3,224,224],
output [1,10]):
    <torch-env>/bin/python tests/fixtures/models/gen_tiny_onnx.py
Requires torch (e.g. the dx-com WSL venv). The committed tiny_cnn.onnx is the
source of truth for the test; this script documents its provenance and lets you
regenerate it. The fixture is plain fp32 -- dxcom does the INT8 quantization at
compile time from the calibration set the test points it at."""
from __future__ import annotations
from pathlib import Path

import torch
import torch.nn as nn


class TinyCNN(nn.Module):
    """Just enough real ops (conv/bn/relu/global-avg-pool/gemm) for dxcom to
    partition onto the NPU, tiny enough to compile in seconds."""

    def __init__(self):
        super().__init__()
        self.c1 = nn.Conv2d(3, 8, 3, 2, 1)
        self.b1 = nn.BatchNorm2d(8)
        self.c2 = nn.Conv2d(8, 16, 3, 2, 1)
        self.b2 = nn.BatchNorm2d(16)
        self.act = nn.ReLU()
        self.pool = nn.AdaptiveAvgPool2d(1)
        self.fc = nn.Linear(16, 10)

    def forward(self, x):
        x = self.act(self.b1(self.c1(x)))
        x = self.act(self.b2(self.c2(x)))
        return self.fc(self.pool(x).flatten(1))


if __name__ == "__main__":
    torch.manual_seed(0)
    out = Path(__file__).with_name("tiny_cnn.onnx")
    torch.onnx.export(
        TinyCNN().eval(),
        torch.randn(1, 3, 224, 224),
        str(out),
        input_names=["input"],
        output_names=["out"],
        opset_version=17,
        dynamo=False,
    )
    print(f"wrote {out} ({out.stat().st_size} bytes)")
