# Basic Pitch for ChordGen FX

Polyphonic note transcription for ChordGen FX's audio-file import. This is [Spotify's Basic Pitch](https://github.com/spotify/basic-pitch) model, embedded in C++ the way [NeuralNote](https://github.com/DamRsn/NeuralNote) does it: its CNN runs on [RTNeural](https://github.com/jatinchowdhury18/RTNeural), and its notes are created the way basic-pitch creates them.

## Origin

The code and the model files come from NeuralNote **v1.1.0** (commit `113f0586`, the last release that runs Basic Pitch; later versions use another model), `Lib/Model` and `Lib/ModelData`:

| File | From NeuralNote | Changes for ChordGen |
|---|---|---|
| `BasicPitch.h/.cpp` | `Lib/Model/BasicPitch.*` | `transcribeToMIDI` takes `const float*`, reports progress, can be cancelled and returns early on very short input |
| `BasicPitchCNN.h/.cpp` | `Lib/Model/BasicPitchCNN.*` | model data from the `BasicPitchData` namespace instead of `BinaryData` |
| `BasicPitchConstants.h` | `Lib/Model/BasicPitchConstants.h` | none |
| `Notes.h/.cpp` | `Lib/Model/Notes.*` | `NoteUtils.h` (NeuralNote's UI strings, needs JUCE) left out; its `hzToMidi` copied into `Notes.cpp`; `<algorithm>` included |
| `Features.h/.cpp` | `Lib/Model/Features.*` | **rewritten**: a native C++ port of `features_model.onnx` instead of ONNX Runtime |
| `ModelData/cnn_*.json` | `Lib/ModelData/` | none (Basic Pitch's CNN weights, converted by NeuralNote for RTNeural) |
| `ModelData/features_model.onnx` | `Lib/ModelData/` | none; kept so `features_cqt.bin` can be regenerated |
| `ModelData/features_cqt.bin` | new | CQT kernels and constants exported from `features_model.onnx` by `export_features.py` |

### Why no ONNX Runtime

NeuralNote runs the feature front end (a constant-Q transform, a normalised log and harmonic stacking) with ONNX Runtime, linked from a prebuilt static library that its build script downloads from a third-party release. ChordGen runs the same graph natively in `Features.cpp`, using the graph's own constants. That keeps the build to source code fetched from pinned URLs, and the universal macOS and Windows builds need nothing extra. On test audio the port matches ONNX Runtime's output to within 0.001 (the features range from -0.88 to 1.6).

RTNeural is not vendored. CMake fetches commit `2ca066e5` (the one NeuralNote v1.1.0 uses) as a hash-checked archive.

## Licenses

- NeuralNote: Apache License 2.0, Copyright 2024 Damien Ronssin (developed by Damien Ronssin and Tibor Vass). The full text is in [`LICENSE`](LICENSE).
- Basic Pitch model and weights: Apache License 2.0, Copyright 2022 Spotify AB. Its notice file is [`NOTICE-basic-pitch`](NOTICE-basic-pitch).
- RTNeural: BSD 3-Clause, Copyright jatinchowdhury18. Its bundled Eigen is MPL 2.0, and its nlohmann/json is MIT.

The Apache License 2.0 is compatible with the GNU AGPLv3 (and GPLv3): Apache-licensed code may be included in an AGPLv3 program, as long as the Apache license and notices travel with it and modified files say so. This folder does that. ChordGen as a whole is AGPLv3.
