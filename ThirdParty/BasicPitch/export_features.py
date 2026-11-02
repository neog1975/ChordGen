"""Exports the constants of Basic Pitch's feature model (features_model.onnx: CQT + normalised log +
harmonic stacking) to features_cqt.bin, which Features.cpp runs natively instead of ONNX Runtime.

    python3 -m venv venv && venv/bin/pip install onnx numpy
    venv/bin/python export_features.py ModelData/features_model.onnx ModelData/features_cqt.bin

Layout, all little-endian float32:
    real kernels [36][256], imaginary kernels [36][256], low-pass [256], bin scale [309], gain, offset
"""
import sys

import numpy as np
import onnx
from onnx import numpy_helper

model = onnx.load(sys.argv[1])
consts = {t.name: numpy_helper.to_array(t) for t in model.graph.initializer}
prefix = "model_1/cqt2010v2_1/"

real = consts[prefix + "conv1d_15/ExpandDims_1:0"].reshape(36, 256)
imag = consts[prefix + "conv1d_16/ExpandDims_1:0"].reshape(36, 256)
lowpass = consts[prefix + "conv1d_17/ExpandDims_1:0"].reshape(256)
scale = consts[prefix + "Sqrt:0"].reshape(309)
gain = consts["model_1/10086:0"].reshape(1)
offset = consts["model_1/tf.__operators__.add_1/y:0"].reshape(1)

blob = np.concatenate([real.ravel(), imag.ravel(), lowpass, scale, gain, offset]).astype("<f4")
blob.tofile(sys.argv[2])
print(f"wrote {blob.size} floats to {sys.argv[2]}")
