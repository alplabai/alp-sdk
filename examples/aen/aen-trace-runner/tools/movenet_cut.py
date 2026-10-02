#!/usr/bin/env python3
"""tools/movenet_cut.py -- cut MoveNet SinglePose Lightning int8 down to its
all-NPU body.

The published model ends in a 29-op CPU tail (float CAST/QUANTIZE on the
input, argmax, GatherNd, float SQRT/DIV over 48x48x17) that Vela leaves on
the M55 and that alp-sdk's TFLM resolver does not register. This keeps only
the network: input = the int8 image tensor (q = grey - 128), outputs = the
four int8 maps the tail reads. src/vision/movenet.c is the tail, in integer C.

    flatc --python --gen-object-api -o GEN <tflite-micro>/tensorflow/compiler/mlir/lite/schema/schema.fbs
    PYTHONPATH=GEN .venv/bin/python tools/movenet_cut.py IN.tflite OUT.tflite

Tensor ids are MoveNet Lightning int8 v4's (Kaggle google/movenet
tfLite/singlepose-lightning-tflite-int8/1, sha256 in the design doc); the
script refuses any other model by checking their names.
"""
import sys

import flatbuffers
import tflite.Model as M

IN_T = 175  # 'tfl.quantize': [1,192,192,3] int8, scale 1, zp -128
OUT_T = {
    262: "Reshape_2",                        # centre score x centre weight, [1,2304,1]
    277: "Sigmoid",                          # keypoint heatmaps, [1,48,48,17]
    282: "kpt_offset_0/conv2d_7/BiasAdd;",   # per-cell keypoint offsets, [1,48,48,34]
    285: "kpt_regress_0/conv2d_6/BiasAdd",   # centre -> keypoint regression, [1,48,48,34]
}


def main(src, dst):
    m = M.ModelT.InitFromObj(M.Model.GetRootAsModel(open(src, "rb").read(), 0))
    sg = m.subgraphs[0]
    if sg.tensors[IN_T].name.decode() != "tfl.quantize":
        sys.exit("not MoveNet Lightning int8 v4 (input tensor name)")
    for t, n in OUT_T.items():
        if not sg.tensors[t].name.decode().startswith(n):
            sys.exit(f"not MoveNet Lightning int8 v4 (tensor {t})")

    # Keep the ops the four outputs depend on, stopping at the new input.
    producer = {t: i for i, op in enumerate(sg.operators) for t in op.outputs}
    keep, todo = set(), list(OUT_T)
    while todo:
        t = todo.pop()
        i = producer.get(t)
        if t == IN_T or i is None or i in keep:
            continue
        keep.add(i)
        todo.extend(x for x in sg.operators[i].inputs if x >= 0)
    sg.operators = [op for i, op in enumerate(sg.operators) if i in keep]
    sg.inputs, sg.outputs = [IN_T], list(OUT_T)
    m.signatureDefs = None

    b = flatbuffers.Builder(0)
    b.Finish(m.Pack(b), file_identifier=b"TFL3")
    open(dst, "wb").write(b.Output())
    print(f"{dst}: {len(sg.operators)} ops kept")


if __name__ == "__main__":
    main(*sys.argv[1:3])
