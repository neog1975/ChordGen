#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include "Engine.h"

// ChordGen FX: chords for an audio file (a stem). Runs off the message and audio threads:
// reads the file, transcribes it with Basic Pitch (polyphonic) and checks that it is an
// isolated stem, not a full song or a drum loop, before its notes reach the engine.
namespace audioimport
{
constexpr double maxSeconds = 180.0; // only the start of longer files is analysed

struct NoteEvent { int pitch = 60; double start = 0.0, end = 0.0, amplitude = 0.0; }; // seconds

// What the classifier listens for. Rates are per second of sound, shares 0..1.
struct Cues
{
    double seconds = 0.0, sounding = 1.0;  // length, and the share of it that is not silent
    double kickRate = 0.0, snareRate = 0.0; // strong low thumps / noisy cracks
    double percussive = 0.0;                // spectral energy that is percussive rather than harmonic
    double fullBand = 0.0;                  // frames with energy at 40-150 Hz, in the mids and above 5 kHz
    double lufs = -100.0, crestDb = 0.0;    // integrated loudness and peak-to-RMS
    double noteCoverage = 0.0;              // sounding time with at least one transcribed note
    double dense = 0.0;                     // sounding time with 4+ notes reaching from the bass to the highs
    double onePitch = 0.0;                  // note time on the most common pitch class (a tuned kick: 1)
};

enum class Verdict { ok, fullSong, drumsOnly, noNotes, unreadable };

struct Message { juce::String title, body, tip; };
Message messageFor (Verdict);

struct Result
{
    Verdict verdict = Verdict::unreadable;
    Cues cues;
    std::vector<NoteEvent> events;
    juce::String error;
};

// Returns false to cancel.
using Progress = std::function<bool (float)>;

bool isAudioFile (const juce::String& path);
Result analyseFile (const juce::File&, const Progress& = {});
Result analyse (const juce::AudioBuffer<float>&, double sampleRate, const Progress& = {});

// Cues from the audio itself; progress runs from 0.1 to 0.2. Returns false if cancelled.
bool measureAudio (Cues&, const juce::AudioBuffer<float>& audio, double sampleRate, const std::vector<float>& mono22k, const Progress& = {});
void measureNotes (Cues&, const std::vector<NoteEvent>&);
Verdict classify (const Cues&);

// Notes on ChordGen's grid (1/24 beat) from the first bar that has notes, up to 16 bars.
struct Placed { std::vector<cg::Note> notes; int bars = 4, firstBar = 0, totalBars = 0; };
Placed place (const std::vector<NoteEvent>&, double bpm, int ts);
} // namespace audioimport
