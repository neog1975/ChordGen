// Runs synthetic audio through ChordGen FX's audio-file import: Basic Pitch and the stem / full song /
// drums classifier. A piano stem and a bass stem must pass, and ChordGen must hear the piano's chords;
// a mastered full mix and a drum loop must be rejected.
// Run: build/AudioImportTests_artefacts/Release/AudioImportTests
//      ... file ...                                 reports what ChordGen hears in audio files
//      ... --chords <bpm> stem.wav chords.mid ...   compares the chords heard with the chords in a MIDI file
#include "../Source/AudioImport.h"
#include "../Source/dsp/LoudnessMeter.h"
#include <climits>
#include <numeric>

namespace
{
constexpr double sr = 44100.0, bpm = 120.0, beat = 60.0 / bpm;
constexpr int bars = 8;
const int length = (int) (sr * beat * 4 * bars);

double hz (int midi) { return 440.0 * std::pow (2.0, (midi - 69) / 12.0); }

juce::Random rng (7);

// Am F C G, one chord per bar: root (octave 2) and triad (octave 4)
const int roots[4] = { 45, 41, 48, 43 };
const int triads[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 55, 60, 64 }, { 55, 59, 62 } };
const int melody[4][4] = { { 72, 71, 69, 64 }, { 69, 72, 69, 65 }, { 67, 64, 67, 72 }, { 71, 67, 74, 71 } };

void addTone (std::vector<float>& out, double start, double dur, double f, float gain, int harmonics, double decay, double damp = 0.4)
{
    const int s0 = (int) (start * sr), n = (int) (dur * sr);
    for (int i = 0; i < n && s0 + i < (int) out.size(); ++i)
    {
        const double t = i / sr;
        const double env = std::min (1.0, t / 0.004) * std::exp (-t * decay) * std::min (1.0, (dur - t) / 0.02);
        double v = 0.0;
        for (int h = 1; h <= harmonics && f * h < sr * 0.45; ++h)
            v += std::sin (juce::MathConstants<double>::twoPi * f * h * t) / h * std::exp (-t * h * damp);
        out[(size_t) (s0 + i)] += (float) (v * env * gain);
    }
}

std::vector<float> piano()
{
    std::vector<float> out ((size_t) length);
    for (int bar = 0; bar < bars; ++bar)
    {
        const int c = bar % 4;
        for (int k = 0; k < 3; ++k) addTone (out, bar * 4 * beat, 4 * beat, hz (triads[c][k]), 0.12f, 8, 0.6);
        for (int q = 0; q < 4; ++q) addTone (out, (bar * 4 + q) * beat, beat, hz (melody[c][q]), 0.16f, 8, 1.5);
    }
    return out;
}

std::vector<float> bass()
{
    std::vector<float> out ((size_t) length);
    for (int bar = 0; bar < bars; ++bar)
        for (int e = 0; e < 8; ++e)
            addTone (out, (bar * 4 + e * 0.5) * beat, 0.45 * beat, hz (roots[bar % 4]), 0.3f, 4, 2.0);
    return out;
}

// bright saw pad: the highs of a produced track
std::vector<float> pad()
{
    std::vector<float> out ((size_t) length);
    for (int bar = 0; bar < bars; ++bar)
        for (int k = 0; k < 3; ++k)
            addTone (out, bar * 4 * beat, 4 * beat, hz (triads[bar % 4][k] + 12), 0.05f, 60, 0.1, 0.0);
    return out;
}

std::vector<float> drums()
{
    std::vector<float> out ((size_t) length);
    for (int b = 0; b < bars * 4; ++b)
    {
        // kick: falling sine on every beat
        const int k0 = (int) (b * beat * sr);
        double phase = 0.0;
        for (int i = 0; i < (int) (0.35 * sr); ++i)
        {
            const double t = i / sr;
            phase += juce::MathConstants<double>::twoPi * (45.0 + 110.0 * std::exp (-t * 30.0)) / sr;
            out[(size_t) (k0 + i)] += (float) (std::sin (phase) * std::exp (-t * 9.0) * 0.9);
        }
        // snare on 2 and 4: noise plus a short tone
        if (b % 2 == 1)
        {
            float prev = 0.0f;
            for (int i = 0; i < (int) (0.2 * sr); ++i)
            {
                const double t = i / sr;
                const float noise = rng.nextFloat() * 2.0f - 1.0f, hp = noise - prev;
                prev = noise;
                out[(size_t) (k0 + i)] += (float) ((hp * 0.5 + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * t)) * std::exp (-t * 18.0));
            }
        }
        // closed hats on eighths
        for (int h = 0; h < 2; ++h)
        {
            const int h0 = k0 + (int) (h * beat * 0.5 * sr);
            float prev = 0.0f;
            for (int i = 0; i < (int) (0.05 * sr); ++i)
            {
                const float noise = rng.nextFloat() * 2.0f - 1.0f, hp = noise - prev;
                prev = noise;
                out[(size_t) (h0 + i)] += hp * 0.15f * (float) std::exp (-(i / sr) * 70.0);
            }
        }
    }
    return out;
}

juce::AudioBuffer<float> stereo (const std::vector<float>& mono, float peakDb)
{
    float peak = 0.0f;
    for (auto v : mono) peak = std::max (peak, std::abs (v));
    const float g = juce::Decibels::decibelsToGain (peakDb) / std::max (peak, 1e-9f);
    juce::AudioBuffer<float> b (2, (int) mono.size());
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < (int) mono.size(); ++i) b.setSample (ch, i, mono[(size_t) i] * g);
    return b;
}

float lufsOf (const juce::AudioBuffer<float>& b)
{
    LoudnessMeter m;
    m.prepare (sr);
    m.process (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
    return m.getIntegrated();
}

// A crude master: drive into a soft clipper until the mix reaches about -9 LUFS.
juce::AudioBuffer<float> master (std::vector<float> mix)
{
    float drive = 1.0f;
    juce::AudioBuffer<float> b;
    for (int k = 0; k < 40; ++k)
    {
        auto x = mix;
        float peak = 0.0f;
        for (auto v : mix) peak = std::max (peak, std::abs (v));
        for (auto& v : x) v = 0.94f * std::tanh (drive * v / peak);
        b = stereo (x, -0.3f);
        if (lufsOf (b) >= -9.5f) break;
        drive *= 1.25f;
    }
    return b;
}

std::vector<float> sum (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> out ((size_t) length);
    for (const auto& p : parts)
        for (size_t i = 0; i < out.size(); ++i) out[i] += p[i];
    return out;
}

const char* name (audioimport::Verdict v)
{
    using V = audioimport::Verdict;
    return v == V::ok ? "stem" : v == V::fullSong ? "full song" : v == V::drumsOnly ? "drums only" : v == V::noNotes ? "no notes" : "unreadable";
}

void print (const char* label, const audioimport::Result& r)
{
    const auto& c = r.cues;
    std::cout << label << ": " << name (r.verdict) << "\n"
              << "  kick " << juce::String (c.kickRate, 2) << "/s, snare " << juce::String (c.snareRate, 2) << "/s, percussive "
              << juce::String (c.percussive, 2) << ", full band " << juce::String (c.fullBand, 2) << ", "
              << juce::String (c.lufs, 1) << " LUFS, crest " << juce::String (c.crestDb, 1) << " dB, notes "
              << juce::String (c.noteCoverage, 2) << ", dense " << juce::String (c.dense, 2) << ", one pitch " << juce::String (c.onePitch, 2) << ", " << r.events.size() << " notes\n";
}

struct Heard { cg::State st; cg::Analysis A; audioimport::Placed placed; };

// What ChordGen FX does with a file it accepts: one chord per bar from the sound.
Heard hear (const audioimport::Result& r, double tempo)
{
    Heard h;
    h.placed = audioimport::place (r.frames, tempo, 0);
    h.st.audio = h.placed.beats;
    h.st.bars = h.placed.bars;
    h.st.source = 5;
    h.st.rhythm = 1.0;
    h.A = cg::analyse (h.st, true);
    return h;
}

juce::String chordsFor (const audioimport::Result& r, double tempo = bpm)
{
    const auto h = hear (r, tempo);
    juce::String s = cg::keyTitle (h.A.key) + ":";
    for (int i = 0; i < h.A.nSeg; ++i) s << " " << cg::chordName (h.A.chordAt (i), h.A, 0, true);
    return s;
}

// The triad a producer wrote in each bar of a chord MIDI file: the best of the 24 major and minor
// triads by note time, with a bonus for the lowest note as root. root -1: empty bar.
struct Triad { int root = -1; bool minor = false; };
std::vector<Triad> triadsInMidi (const juce::File& f, int& firstBar)
{
    // read the notes directly: files exported from a song arrangement start many bars in, so they
    // start here at their first bar with notes, as stems do
    juce::MidiFile mf;
    juce::FileInputStream in (f);
    if (! in.openedOk() || ! mf.readFrom (in) || mf.getTimeFormat() <= 0) return {};
    const double tpq = mf.getTimeFormat();
    std::vector<cg::Note> notes;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (! m.isNoteOn() || m.getChannel() == 10) continue;
            const double off = std::max (seq.getTimeOfMatchingKeyUp (i), m.getTimeStamp() + tpq / 4.0);
            notes.push_back ({ m.getNoteNumber(), (int) std::lround (m.getTimeStamp() / tpq * cg::stepsPerBeat),
                               std::max (1, (int) std::lround ((off - m.getTimeStamp()) / tpq * cg::stepsPerBeat)) });
        }
    }
    if (notes.empty()) return {};
    const int spBar = cg::timeSigs()[0].steps;
    int first = INT_MAX, end = 0;
    for (const auto& n : notes) { first = std::min (first, n.s); end = std::max (end, n.s + n.d); }
    first = first / spBar * spBar;
    firstBar = first / spBar;
    for (auto& n : notes) n.s -= first;
    const int bars = (end - first + spBar - 1) / spBar;
    std::vector<Triad> out ((size_t) bars);
    for (int bar = 0; bar < bars; ++bar)
    {
        std::array<double, 12> w {};
        int lowest = 128;
        for (const auto& n : notes)
        {
            const int ov = std::min ((bar + 1) * spBar, n.s + n.d) - std::max (bar * spBar, n.s);
            if (ov <= 0) continue;
            w[(size_t) (n.p % 12)] += ov;
            lowest = std::min (lowest, n.p);
        }
        double best = 0.0, tot = std::accumulate (w.begin(), w.end(), 0.0);
        for (int root = 0; root < 12; ++root)
            for (bool minor : { false, true })
            {
                const int third = (root + (minor ? 3 : 4)) % 12, fifth = (root + 7) % 12;
                double sc = 0.0;
                for (int pc = 0; pc < 12; ++pc) sc += w[(size_t) pc] * (pc == root || pc == third || pc == fifth ? 1.0 : -0.6);
                if (lowest < 128 && lowest % 12 == root) sc += 0.3 * tot;
                if (sc > best) { best = sc; out[(size_t) bar] = { root, minor }; }
            }
    }
    return out;
}

juce::String triadName (Triad t) { return t.root < 0 ? juce::String ("-") : cg::pcName (t.root, false, false) + (t.minor ? "m" : ""); }

// --chords: per bar, does the chord heard in the stem have the root and quality written in the MIDI?
int compareChords (int argc, char** argv)
{
    const double tempo = juce::String (argv[2]).getDoubleValue();
    int right = 0, total = 0;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const auto r = audioimport::analyseFile (cwd.getChildFile (argv[i]));
        int midiFirstBar = 0;
        const auto truth = triadsInMidi (cwd.getChildFile (argv[i + 1]), midiFirstBar);
        if (r.verdict != audioimport::Verdict::ok || truth.empty())
        {
            std::cout << argv[i] << ": not compared (" << (truth.empty() ? "no MIDI chords" : "file rejected") << ")\n";
            continue;
        }
        const auto h = hear (r, tempo);
        juce::String heard, written;
        int ok = 0, n = 0;
        for (int bar = 0; bar < h.A.nSeg; ++bar)
        {
            const auto& c = h.A.chordAt (bar);
            // both start at their first bar with sound; compare the same bar of the song (a loop repeats)
            const int songBar = bar + h.placed.firstBar - midiFirstBar, n0 = (int) truth.size();
            const Triad t = truth[(size_t) (((songBar % n0) + n0) % n0)];
            heard << " " << triadName ({ c.root, c.q == cg::Quality::min || c.q == cg::Quality::dim });
            written << " " << triadName (t);
            if (t.root < 0) continue;
            ++n;
            ok += c.root == t.root && (c.q == cg::Quality::min || c.q == cg::Quality::dim) == t.minor;
        }
        std::cout << juce::File (argv[i]).getFileName() << ": " << ok << "/" << n << " bars\n  heard  " << heard
                  << "\n  MIDI   " << written << "\n";
        right += ok;
        total += n;
    }
    std::cout << "Chords right: " << right << "/" << total << "\n";
    return 0;
}

// Runs the analysis on a worker thread with JUCE's default stack, as ChordGen FX does.
audioimport::Result analyseOnThread (const juce::AudioBuffer<float>& audio)
{
    struct Job : juce::Thread
    {
        explicit Job (const juce::AudioBuffer<float>& a) : juce::Thread ("analyse"), audio (a) {}
        void run() override { result = audioimport::analyse (audio, sr); }
        const juce::AudioBuffer<float>& audio;
        audioimport::Result result;
    } job (audio);
    job.startThread();
    while (job.isThreadRunning()) juce::Thread::sleep (20);
    return job.result;
}

int failures = 0;

void expect (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (! ok) ++failures;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc > 3 && juce::String (argv[1]) == "--chords")
        return compareChords (argc, argv);

    // real files given on the command line are only analysed and reported ([--bpm N] file ...)
    if (argc > 1)
    {
        double tempo = bpm;
        for (int i = 1; i < argc; ++i)
        {
            if (juce::String (argv[i]) == "--bpm" && i + 1 < argc) { tempo = juce::String (argv[++i]).getDoubleValue(); continue; }
            const auto r = audioimport::analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]));
            print (argv[i], r);
            if (r.verdict == audioimport::Verdict::ok) std::cout << "  " << chordsFor (r, tempo) << "\n";
        }
        return 0;
    }

    using V = audioimport::Verdict;
    const auto p = piano(), b = bass(), d = drums(), s = pad();

    {
        const auto r = analyseOnThread (stereo (p, -6.0f));
        print ("Piano stem", r);
        expect (r.verdict == V::ok, "accepted as a stem");
        const auto chords = chordsFor (r);
        std::cout << "  " << chords << "\n";
        expect (chords == "A minor: Am F C G Am F C G", "hears the chords Am F C G in every bar");
    }
    {
        const auto r = analyseOnThread (stereo (b, -3.0f));
        print ("Bass stem", r);
        expect (r.verdict == V::ok, "accepted as a stem");
    }
    {
        const auto r = analyseOnThread (master (sum ({ p, b, d, s })));
        print ("Full mix", r);
        expect (r.verdict == V::fullSong, "rejected as a full song");
    }
    {
        const auto r = analyseOnThread (stereo (d, -1.0f));
        print ("Drum loop", r);
        expect (r.verdict == V::drumsOnly, "rejected as drums only");
    }
    {
        // 120 BPM: a beat is 0.5 s, 43 frames; sound from bar 3 (4 s) to the middle of bar 4, then silence
        std::vector<cg::AudioBeat> frames ((size_t) (10.0 / audioimport::frameSeconds));
        for (size_t f = 0; f < frames.size(); ++f)
        {
            const double t = (double) f * audioimport::frameSeconds;
            if (t >= 4.0 && t < 7.0) { frames[f].chroma[9] = 1.0f; frames[f].bass[9] = 1.0f; frames[f].chroma[0] = t < 5.0 ? 0.5f : 0.0f; }
        }
        const auto placed = audioimport::place (frames, 120.0, 0);
        expect (placed.firstBar == 2 && placed.bars == 4 && placed.beats.size() == 12 && placed.beats[0].chroma[9] > 0.99f
                    && placed.beats[1].chroma[0] > 0.4f && placed.beats[2].chroma[0] < 0.01f && placed.beats[5].chroma[9] > 0.99f
                    && placed.beats[6].chroma[9] < 0.01f,
                "the sound placed on beats from the first bar that sounds");
    }

    std::cout << (failures == 0 ? "All passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
