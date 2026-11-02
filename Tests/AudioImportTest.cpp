// Runs synthetic audio through ChordGen FX's audio-file import: Basic Pitch transcription and the
// stem / full song / drums classifier. A piano stem and a bass stem must pass and give chords that
// fit; a mastered full mix and a drum loop must be rejected.
// Run: build/AudioImportTests_artefacts/Release/AudioImportTests [file ...]
#include "../Source/AudioImport.h"
#include "../Source/dsp/LoudnessMeter.h"

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

juce::String chordsFor (const audioimport::Result& r)
{
    const auto placed = audioimport::place (r.events, bpm, 0);
    cg::State st;
    st.notes = placed.notes;
    st.bars = placed.bars;
    st.rhythm = 1.0;
    const auto A = cg::analyse (st, true);
    juce::String s = cg::keyTitle (A.key) + ":";
    for (int i = 0; i < A.nSeg; ++i) s << " " << cg::chordName (A.chordAt (i), A, 0, true);
    return s;
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

    // real files given on the command line are only analysed and reported
    if (argc > 1)
    {
        for (int i = 1; i < argc; ++i)
        {
            const auto r = audioimport::analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]));
            print (argv[i], r);
            if (r.verdict == audioimport::Verdict::ok) std::cout << "  " << chordsFor (r) << "\n";
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
        expect (chords.contains ("Am") && chords.contains ("F") && chords.contains ("C") && chords.contains ("G"), "chords Am F C G found");

        bool exact = true;
        for (int pitch : { 57, 60, 64, 72 })
            exact = exact && std::any_of (r.events.begin(), r.events.end(), [pitch] (const audioimport::NoteEvent& e)
                                          { return e.pitch == pitch && e.start < 0.1; });
        expect (exact, "first bar's chord and melody notes transcribed at the start");
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
        // 120 BPM: 0.5 s = 1 beat = 24 steps; the stem starts in bar 3, which becomes bar 1
        const auto placed = audioimport::place ({ { 60, 4.0, 4.5, 0.5 }, { 64, 4.5, 5.25, 0.5 }, { 67, 6.0, 6.5, 0.5 } }, 120.0, 0);
        expect (placed.firstBar == 2 && placed.bars == 4 && placed.notes.size() == 3
                    && placed.notes[0] == cg::Note { 60, 0, 24 } && placed.notes[1] == cg::Note { 64, 24, 36 }
                    && placed.notes[2] == cg::Note { 67, 96, 24 },
                "notes placed on the 1/24-beat grid from the first bar with notes");
    }

    std::cout << (failures == 0 ? "All passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
