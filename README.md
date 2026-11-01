# ChordGen

**Free, open-source chord suggestions for your melody.** ChordGen listens to a melody from the FL Studio piano roll, an audio track or a microphone, detects the key and suggests chord progressions that fit. Drag the result straight into the piano roll as MIDI.

<p align="center">
  <img src="docs/ui.png" alt="ChordGen" width="80%">
</p>

Formats: **VST3** and **Standalone** for Windows and macOS (Intel and Apple Silicon). Built for FL Studio; works in other VST3 hosts.

**⬇️ Download:** get the ready-to-install plugins for Windows and macOS from the [latest release](https://github.com/neog1975/ChordGen/releases/latest) (install steps are on that page).

> Status: early version (v0.3). Feedback, bug reports and pull requests are very welcome.

## Two plugins

FL Studio sends piano-roll notes only to instruments and audio only to effects, so ChordGen comes in two variants built from the same code:

| Plugin | Where in FL | Listens to |
|---|---|---|
| **ChordGen** | Channel Rack (instrument) | the notes in its piano roll |
| **ChordGen FX** | Mixer insert (effect) | an audio track or a microphone, one note at a time |

## How it works

1. Write a melody in ChordGen's piano roll (or sing/play into ChordGen FX) and press Play in FL.
2. After one pass of the pattern ChordGen takes the melody, detects the key and suggests five progressions.
3. Browse suggestions, swap single chords for alternatives, lock the ones you like and regenerate the rest.
4. Drag **Drag MIDI → Piano Roll** into FL, or save a `.mid` file.

## Features

- Key detection (Krumhansl profiles, re-ranked by how well the key's chords fit) with manual key lock; major, minor, harmonic minor and church modes
- Chord scoring per bar with exact note timing (1/24 beat), borrowed chords and secondary dominants
- Progression search with style habits: **Pop**, **Trap**, **Lo-Fi**, **Jazz**
- Complexity from triads to 7ths, 9ths and jazz voicings, with smooth voice leading
- 1 or 2 chords per bar, or one chord per 2 bars; 4, 8 or 16 bars; 4/4, 3/4 and 6/8
- Lock and regenerate, undo/redo, saved progressions
- Clash display: melody notes that fight the chord turn red, with an explanation of each choice
- Built-in sound (pad, bass, melody) synced to FL, plus MIDI out for your own synths
- **ChordGen FX**: YIN pitch tracking with live note and cents display, metronome, and a **Live mode** that plays the chord for the bar you just played
- MIDI import and export

## Building

Requirements: CMake 3.22+, a C++17 compiler (Xcode command line tools on macOS, Visual Studio on Windows) and JUCE 8.

```bash
git clone https://github.com/neog1975/ChordGen.git
cd ChordGen
git clone --depth 1 --branch 8.0.8 https://github.com/juce-framework/JUCE.git JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On macOS the plugins are copied to `~/Library/Audio/Plug-Ins/VST3` after building (turn this off with `-DCHORDGEN_COPY_PLUGIN=OFF`). Every push is also built for Windows and macOS by GitHub Actions.

### Tests

Configure with `-DCHORDGEN_TESTS=ON` to build:

- `ChordGenTests`: simulates FL playing a melody and checks capture timing and MIDI import
- `PitchTests`: runs a synthetic sung melody through the pitch tracker
- `MidiBatch <folder>`: imports every MIDI file in a folder and reports any note whose timing changed
- `ChordEval <folders> [--songs]`: compares ChordGen's chords with the chords producers wrote in chord packs or full arrangements

## License

ChordGen is released under the [GNU Affero General Public License v3.0](LICENSE). It is built with [JUCE](https://juce.com), which is available under the AGPLv3 or a commercial license.
