#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <map>
#include <vector>

// The ChordGen engine: key detection, chord scoring, progression search, voicing,
// and the note patterns used for playback and MIDI export. Pure functions on a State,
// run on the message thread.
namespace cg
{
struct Note
{
    int p = 60, s = 0, d = 1; // pitch, start and length in steps of 1/stepsPerBeat beat
    bool operator== (const Note& o) const { return p == o.p && s == o.s && d == o.d; }
};

enum class Quality { maj, min, dim, aug };
enum class Style { pop, trap, lofi, jazz };
constexpr int numStyles = 4;

struct ModeInfo { const char* name; std::array<int, 7> iv; bool minor; int off; };
const std::vector<ModeInfo>& modes(); // 0 major, 1 minor, 2 harmonic minor, 3 Dorian, 4 Phrygian, 5 Mixolydian, 6 Lydian

// Fine enough for 16ths, 32nds and triplets, so notes keep the exact timing FL sends.
constexpr int stepsPerBeat = 24;

struct TimeSig { const char* name; int steps, beats, num, den; std::array<double, 8> accent; }; // steps per bar; accent per eighth
double accentAt (const TimeSig&, int step);
const std::vector<TimeSig>& timeSigs(); // 0 4/4, 1 3/4, 2 6/8

struct StyleInfo
{
    const char* label;
    double fifth, step, strict, borrowed, repeat, patternW;
    int defaultComplexity;
    float cutoff;
    bool swing, walk;
    std::vector<std::vector<int>> patMaj, patMin;  // typical loops, semitones from the tonic
    std::map<int, double> bonusMin;
    std::vector<std::pair<double, double>> ch4, bs4, ch3, bs3; // preview rhythm: beat, length
};
const StyleInfo& styleInfo (Style);

struct Key
{
    int tonic = 9, mode = 1;
    bool operator== (const Key& o) const { return tonic == o.tonic && mode == o.mode; }
};

struct Chord
{
    int root = 0, semi = 0;
    Quality q = Quality::maj;
    int sev = 10, nin = 2;
    bool borrowed = false;
    juce::String tag;
    int sec = -1; // secondary dominant: target semitone
    int code() const { return root * 4 + (int) q; }
};

// Everything the user edits. Copied whole for undo.
struct State
{
    std::vector<Note> notes;
    int bars = 4, ts = 0;
    double rhythm = 1.0; // chords per bar: 2, 1 or 0.5
    int complexity = 1;
    Style style = Style::pop;
    bool borrowed = true, rootVoicing = false, bass = true, playMelody = true, patternMidi = true;
    int octave = 0;
    bool keyLocked = false;
    Key keyLock;
    std::map<int, int> locks; // segment -> chord code
    std::vector<int> prog;    // chord codes
    int sugIdx = 0, seed = 0, selected = 0;
    int source = 0;           // 0 FL piano roll, 1 drawn, 2 MIDI file, 3 audio track, 4 microphone, 5 audio file
    juce::String preset;
};

struct Fit { double score = 0.0; int pct = -1; };
struct Item { Note n; double w = 0.0, acc = 0.0; bool inside = false; };
struct Voicing { std::vector<int> notes; int bass = 45; };
struct KeyCandidate { Key key; double r = 0.0; };
struct Event { double t, d; int midi, type; float v; }; // type 0 chord, 1 bass, 2 melody

struct Analysis
{
    int segSteps = 8, nSeg = 4;
    std::vector<KeyCandidate> keys;
    Key key;
    bool minor = true, flats = false;
    std::array<bool, 12> scale {};
    std::vector<Chord> chords;
    std::vector<std::vector<Item>> items;
    std::vector<std::vector<Fit>> fits;   // [segment][chord index]
    std::vector<std::vector<int>> sugs;   // chord indices
    std::vector<int> prog;                // chord indices
    bool edited = false;
    std::vector<Voicing> voicing;
    std::vector<Note> clash;
    int rangeLo = 57, rangeHi = 84;

    const Chord& chordAt (int seg) const { return chords[(size_t) prog[(size_t) seg]]; }
    const Fit& fitAt (int seg) const { return fits[(size_t) seg][(size_t) prog[(size_t) seg]]; }
};

Analysis analyse (State& st, bool resuggest);

juce::String pcName (int pc, bool flats, bool pretty = true);
juce::String keyTitle (const Key&);
juce::String chordName (const Chord&, const Analysis&, int complexity, bool ascii = false);
juce::String roman (const Chord&, const Analysis&);
juce::String segmentLabel (int seg, double rhythm);
std::vector<int> alternatives (const State&, const Analysis&, int seg);
std::vector<Note> clashes (const State&, const Analysis&, const Chord&, int seg);
struct Explanation { juce::String text, clash; };
Explanation explain (const State&, const Analysis&, int seg);
int keyMatchPercent (const State&, const Analysis&);

std::vector<Event> buildEvents (const State&, const Analysis&, bool withPattern);
juce::MemoryBlock chordsMidi (const State&, const Analysis&, double bpm);       // chords + bass, one track
juce::MemoryBlock fullMidi (const State&, const Analysis&, double bpm);         // chords, bass, melody tracks
juce::String midiFileBase (const State&, const Analysis&);

struct ImportResult { std::vector<Note> notes; int bars = 4, ts = 0; double bpm = 0.0; juce::String error; };
ImportResult importMidi (const juce::File&);

juce::StringArray presetNames();
void loadPreset (State&, const juce::String& name);

juce::var toVar (const State&);
State fromVar (const juce::var&);
} // namespace cg
