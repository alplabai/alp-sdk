# tests/fixtures/models/gen_tiny_model.py
"""Generate a tiny int8 FULLY_CONNECTED .tflite fixture (no TensorFlow).

Run to (re)create tiny_int8.tflite:
    py -3.14 tests/fixtures/models/gen_tiny_model.py
Requires the `model-compile` extra (tflite + flatbuffers)."""
from __future__ import annotations
from pathlib import Path

import flatbuffers
import tflite as t

K, N = 4, 2


def _buffer(b, data: bytes):
    if not data:
        t.BufferStart(b); return t.BufferEnd(b)
    d = b.CreateByteVector(data)
    t.BufferStart(b); t.BufferAddData(b, d); return t.BufferEnd(b)


def _quant(b, scale, zp):
    t.QuantizationParametersStartScaleVector(b, 1); b.PrependFloat32(scale); sv = b.EndVector()
    t.QuantizationParametersStartZeroPointVector(b, 1); b.PrependInt64(zp); zv = b.EndVector()
    t.QuantizationParametersStart(b)
    t.QuantizationParametersAddScale(b, sv); t.QuantizationParametersAddZeroPoint(b, zv)
    return t.QuantizationParametersEnd(b)


def _tensor(b, shape, ttype, buf_idx, name, scale, zp):
    nm = b.CreateString(name); q = _quant(b, scale, zp)
    t.TensorStartShapeVector(b, len(shape))
    for d in reversed(shape):
        b.PrependInt32(d)
    sh = b.EndVector()
    t.TensorStart(b)
    t.TensorAddShape(b, sh); t.TensorAddType(b, ttype); t.TensorAddBuffer(b, buf_idx)
    t.TensorAddName(b, nm); t.TensorAddQuantization(b, q)
    return t.TensorEnd(b)


# flatbuffers vectors are written back-to-front: open with the element count, then
# Prepend each element in REVERSE so they end up in forward order. (_offvec below
# does the same for vectors of table offsets.)
def _ivec(b, start_fn, vals):
    start_fn(b, len(vals))
    for v in reversed(vals):
        b.PrependInt32(v)
    return b.EndVector()


def _offvec(b, start_fn, offs):
    start_fn(b, len(offs))
    for off in reversed(offs):
        b.PrependUOffsetTRelative(off)
    return b.EndVector()


def build() -> bytes:
    b = flatbuffers.Builder(1024)
    bufs = [_buffer(b, b""), _buffer(b, b"\x01" * (N * K)), _buffer(b, b"\x00" * (N * 4))]
    buffers = _offvec(b, t.ModelStartBuffersVector, bufs)
    t_in = _tensor(b, [1, K], t.TensorType.INT8, 0, "input", 0.0078125, -1)
    t_w = _tensor(b, [N, K], t.TensorType.INT8, 1, "weights", 0.005, 0)
    t_b = _tensor(b, [N], t.TensorType.INT32, 2, "bias", 0.0078125 * 0.005, 0)
    t_out = _tensor(b, [1, N], t.TensorType.INT8, 0, "output", 0.004, 2)
    tensors = _offvec(b, t.SubGraphStartTensorsVector, [t_in, t_w, t_b, t_out])
    sg_in = _ivec(b, t.SubGraphStartInputsVector, [0])
    sg_out = _ivec(b, t.SubGraphStartOutputsVector, [3])
    op_in = _ivec(b, t.OperatorStartInputsVector, [0, 1, 2])
    op_out = _ivec(b, t.OperatorStartOutputsVector, [3])
    t.FullyConnectedOptionsStart(b); fc = t.FullyConnectedOptionsEnd(b)
    t.OperatorStart(b)
    t.OperatorAddOpcodeIndex(b, 0); t.OperatorAddInputs(b, op_in); t.OperatorAddOutputs(b, op_out)
    t.OperatorAddBuiltinOptionsType(b, t.BuiltinOptions.FullyConnectedOptions)
    t.OperatorAddBuiltinOptions(b, fc)
    op = t.OperatorEnd(b)
    operators = _offvec(b, t.SubGraphStartOperatorsVector, [op])
    sg_name = b.CreateString("main")
    t.SubGraphStart(b)
    t.SubGraphAddTensors(b, tensors); t.SubGraphAddInputs(b, sg_in)
    t.SubGraphAddOutputs(b, sg_out); t.SubGraphAddOperators(b, operators); t.SubGraphAddName(b, sg_name)
    sg = t.SubGraphEnd(b)
    subgraphs = _offvec(b, t.ModelStartSubgraphsVector, [sg])
    t.OperatorCodeStart(b)
    t.OperatorCodeAddDeprecatedBuiltinCode(b, t.BuiltinOperator.FULLY_CONNECTED)
    t.OperatorCodeAddBuiltinCode(b, t.BuiltinOperator.FULLY_CONNECTED)
    t.OperatorCodeAddVersion(b, 1)
    oc = t.OperatorCodeEnd(b)
    opcodes = _offvec(b, t.ModelStartOperatorCodesVector, [oc])
    desc = b.CreateString("alp tiny int8 fixture")
    t.ModelStart(b)
    t.ModelAddVersion(b, 3); t.ModelAddOperatorCodes(b, opcodes)
    t.ModelAddSubgraphs(b, subgraphs); t.ModelAddBuffers(b, buffers); t.ModelAddDescription(b, desc)
    b.Finish(t.ModelEnd(b), b"TFL3")
    return bytes(b.Output())


if __name__ == "__main__":
    out = Path(__file__).with_name("tiny_int8.tflite")
    out.write_bytes(build())
    print(f"wrote {out} ({out.stat().st_size} bytes)")
