#include "AudioImport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "BasicPitch.h"
#include "dsp/MixAnalyzer.h"

namespace audioimport
{
namespace
{
constexpr double bpRate = BASIC_PITCH_SAMPLE_RATE; // 22050

bool keepGoing (const Progress& p, float v) { return ! p || p (juce::jlimit (0.0f, 1.0f, v)); }

// Mono at 22050 Hz: 8th-order Butterworth low-pass below the new Nyquist, then every other sample
// from 44.1 kHz, or Lagrange interpolation (as NeuralNote resamples).
std::vector<float> toBasicPitchRate (const juce::AudioBuffer<float>& in, double sr)
{
    const int n = in.getNumSamples(), chans = in.getNumChannels();
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < chans; ++ch)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), in.getReadPointer (ch), 1.0f / (float) chans, n);
    if (std::abs (sr - bpRate) < 1.0)
        return mono;

    if (sr > bpRate)
        for (const double q : { 0.5098, 0.6013, 0.9000, 2.5629 })
        {
            juce::IIRFilter f;
            f.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, bpRate * 0.45, q));
            f.processSamples (mono.data(), n);
        }

    const double ratio = sr / bpRate;
    const int outN = (int) std::floor ((double) n / ratio);
    std::vector<float> out ((size_t) juce::jmax (0, outN));
    if (std::abs (ratio - 2.0) < 1e-9)
    {
        for (int i = 0; i < outN; ++i) out[(size_t) i] = mono[(size_t) (2 * i)];
        return out;
    }
    mono.resize (mono.size() + 64, 0.0f); // the interpolator reads a little ahead
    juce::LagrangeInterpolator interp;
    interp.process (ratio, mono.data(), out.data(), outN);
    return out;
}

double dbOf (double energy) { return 10.0 * std::log10 (energy + 1e-20); }
} // namespace

//==============================================================================
Message messageFor (Verdict v)
{
    switch (v)
    {
        case Verdict::fullSong:
            return { "This looks like a full song.",
                     "ChordGen detects chords from isolated stems only (e.g. piano, guitar, synth or bass).",
                     juce::String::fromUTF8 ("Tip: In FL Studio, right-click the clip in the Playlist \xe2\x86\x92 Stem separation, then drop the instrument stem here.") };
        case Verdict::drumsOnly:
            return { "No chords found.",
                     "This file sounds like drums or percussion only. Drop a melodic or harmonic stem instead.", {} };
        case Verdict::noNotes:
            return { "No chords found.", "ChordGen could not hear any notes in this file.", {} };
        case Verdict::unreadable:
            return { "Could not read this file.", "Drop a WAV, AIFF, MP3 or FLAC file.", {} };
        case Verdict::ok:
            break;
    }
    return {};
}

bool isAudioFile (const juce::String& path)
{
    for (auto ext : { ".wav", ".wave", ".aif", ".aiff", ".mp3", ".flac" })
        if (path.endsWithIgnoreCase (ext)) return true;
    return false;
}

Result analyseFile (const juce::File& file, const Progress& progress)
{
    Result r;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        r.error = "unsupported or damaged file";
        return r;
    }

    const auto n = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (maxSeconds * reader->sampleRate));
    const int chans = (int) juce::jlimit (1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> audio (chans, n);
    const int chunk = 1 << 16;
    for (int pos = 0; pos < n; pos += chunk)
    {
        if (! keepGoing (progress, 0.05f * (float) pos / (float) n)) return r;
        reader->read (&audio, pos, juce::jmin (chunk, n - pos), pos, true, chans > 1);
    }
    return analyse (audio, reader->sampleRate, progress);
}

Result analyse (const juce::AudioBuffer<float>& audio, double sr, const Progress& progress)
{
    Result r;
    if (! keepGoing (progress, 0.05f)) return r;
    auto mono = toBasicPitchRate (audio, sr);
    if (! keepGoing (progress, 0.1f)) return r;

    if (! measureAudio (r.cues, audio, sr, mono, progress) || ! keepGoing (progress, 0.2f)) return r;

    // Basic Pitch needs a little more than a second; pad short files with silence
    mono.resize (juce::jmax (mono.size(), (size_t) (2.0 * bpRate)), 0.0f);
    auto bp = std::make_unique<BasicPitch>(); // the CNN's buffers are too big for a worker thread's stack
    bp->reset();
    bp->setParameters (0.7f, 0.5f, 125.0f);   // NeuralNote's defaults
    if (! bp->transcribeToMIDI (mono.data(), (int) mono.size(), [&] (float p) { return keepGoing (progress, 0.2f + 0.78f * p); }))
        return r;

    for (const auto& e : bp->getNoteEvents())
        r.events.push_back ({ e.pitch, e.startTime, e.endTime, e.amplitude });
    measureNotes (r.cues, r.events);
    r.verdict = classify (r.cues);
    keepGoing (progress, 1.0f);
    return r;
}

//==============================================================================
bool measureAudio (Cues& c, const juce::AudioBuffer<float>& audio, double sr, const std::vector<float>& y, const Progress& progress)
{
    c = {};
    c.seconds = (double) audio.getNumSamples() / sr;

    // loudness and crest factor, with Forge Master's analyser
    {
        const int n = audio.getNumSamples();
        const float* L = audio.getReadPointer (0);
        const float* R = audio.getReadPointer (juce::jmin (1, audio.getNumChannels() - 1));
        MixAnalyzer mix;
        mix.prepare (sr, (float) c.seconds);
        mix.start();
        MixAnalyzer::Result res;
        const int block = 4096;
        for (int pos = 0; pos < n; pos += block)
            mix.process (L + pos, R + pos, juce::jmin (block, n - pos), true);
        std::vector<float> zeros ((size_t) block, 0.0f);
        for (int k = 0; k < 16 && ! mix.takeResult (res); ++k) // rounding of the length
            mix.process (zeros.data(), zeros.data(), block, true);
        c.lufs = res.integratedLufs;
        c.crestDb = res.crestDb;
    }

    // spectrogram of the 22050 Hz mono signal: 1024-point frames, hop 512 (23 ms)
    constexpr int order = 10, size = 1 << order, hop = size / 2, bins = size / 2 + 1;
    const int frames = (int) y.size() >= size ? ((int) y.size() - size) / hop + 1 : 0;
    if (frames < 8) return true;
    if (! keepGoing (progress, 0.12f)) return false;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), buf ((size_t) size * 2);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
    std::vector<float> S ((size_t) frames * bins); // magnitude
    for (int t = 0; t < frames; ++t)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i) buf[(size_t) i] = y[(size_t) (t * hop + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        std::copy (buf.begin(), buf.begin() + bins, S.begin() + (ptrdiff_t) t * bins);
    }

    const double binHz = bpRate / size;
    auto bin = [binHz, lastBin = bins - 1] (double hz) { return juce::jlimit (0, lastBin, (int) std::lround (hz / binHz)); };
    const int low0 = bin (40.0), low1 = bin (150.0), high0 = bin (5000.0), crack0 = bin (1500.0), crack1 = bin (8000.0);

    // harmonic/percussive split: a bin is percussive when the median across frequency beats the
    // median across time (Fitzgerald 2010)
    std::vector<bool> isPerc (S.size());
    {
        constexpr int span = 17, halfSpan = span / 2;
        double perc = 0.0, total = 0.0;
        std::vector<float> tmp (span);
        for (int t = 0; t < frames; ++t)
        {
            if (t % 256 == 0 && ! keepGoing (progress, 0.12f + 0.07f * (float) t / (float) frames)) return false;
            for (int k = 1; k < bins; ++k)
            {
                int m = 0;
                for (int j = t - halfSpan; j <= t + halfSpan; ++j)
                    tmp[(size_t) m++] = S[(size_t) juce::jlimit (0, frames - 1, j) * bins + (size_t) k];
                std::nth_element (tmp.begin(), tmp.begin() + halfSpan, tmp.end());
                const float h = tmp[(size_t) halfSpan];
                m = 0;
                for (int j = k - halfSpan; j <= k + halfSpan; ++j)
                    tmp[(size_t) m++] = S[(size_t) t * bins + (size_t) juce::jlimit (1, bins - 1, j)];
                std::nth_element (tmp.begin(), tmp.begin() + halfSpan, tmp.end());
                const float p = tmp[(size_t) halfSpan];
                const size_t i = (size_t) t * bins + (size_t) k;
                const double e = (double) S[i] * S[i];
                total += e;
                if (p > h) { perc += e; isPerc[i] = true; }
            }
        }
        c.percussive = total > 0.0 ? perc / total : 0.0;
    }

    std::vector<double> eLow ((size_t) frames), eMid ((size_t) frames), eHigh ((size_t) frames), eTot ((size_t) frames),
                        kick ((size_t) frames), crack ((size_t) frames);
    double loudest = 0.0;
    for (int t = 0; t < frames; ++t)
    {
        const size_t row = (size_t) t * bins;
        double lo = 0, mid = 0, hi = 0;
        for (int k = 1; k < bins; ++k)
        {
            const double e = (double) S[row + (size_t) k] * S[row + (size_t) k];
            if (k >= low0 && k < low1) lo += e;
            else if (k >= low1 && k < high0) mid += e;
            else if (k >= high0) hi += e;
            if (isPerc[row + (size_t) k])
            {
                if (k >= low0 && k < low1) kick[(size_t) t] += e;
                if (k >= crack0 && k < crack1) crack[(size_t) t] += e;
            }
        }
        eLow[(size_t) t] = lo; eMid[(size_t) t] = mid; eHigh[(size_t) t] = hi;
        eTot[(size_t) t] = lo + mid + hi;
        loudest = std::max (loudest, eTot[(size_t) t]);
    }

    // full-band frames: lows, mids and highs all present, most of the time
    int active = 0, full = 0;
    for (int t = 0; t < frames; ++t)
    {
        const double tot = eTot[(size_t) t];
        if (tot < loudest * 1e-4) continue;
        ++active;
        if (dbOf (eLow[(size_t) t] / tot) > -22.0 && dbOf (eMid[(size_t) t] / tot) > -12.0 && dbOf (eHigh[(size_t) t] / tot) > -32.0)
            ++full;
    }
    c.fullBand = active > 0 ? (double) full / active : 0.0;
    c.sounding = (double) active / frames;

    // drum hits: sudden rises of the percussive part at 40-150 Hz (kick) and 1.5-8 kHz (snare, claps,
    // hats), loud against the whole mix
    auto hits = [&] (const std::vector<double>& e, double floorDb)
    {
        const double floor = loudest * std::pow (10.0, floorDb / 10.0);
        int count = 0;
        for (int t = 3; t < frames - 3; ++t)
        {
            const double before = std::max ({ e[(size_t) t - 1], e[(size_t) t - 2], e[(size_t) t - 3] });
            const double after = std::max ({ e[(size_t) t], e[(size_t) t + 1], e[(size_t) t + 2] });
            if (dbOf (e[(size_t) t]) - dbOf (before) >= 9.0 && after >= floor)
            {
                ++count;
                t += 4; // ~90 ms before the next hit
            }
        }
        return count / juce::jmax (1.0, (double) active * hop / bpRate); // per second of sound
    };
    c.kickRate = hits (kick, -20.0);
    c.snareRate = hits (crack, -30.0);
    return true;
}

void measureNotes (Cues& c, const std::vector<NoteEvent>& events)
{
    const double step = 0.05;
    const int n = (int) std::ceil (c.seconds / step);
    if (n <= 0) return;
    int covered = 0, dense = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i * step;
        int count = 0, lo = 128, hi = -1;
        for (const auto& e : events)
            if (e.start <= t && e.end > t)
            {
                ++count;
                lo = std::min (lo, e.pitch);
                hi = std::max (hi, e.pitch);
            }
        if (count > 0) ++covered;
        if (count >= 4 && lo <= 52 && hi >= 72) ++dense;
    }
    std::array<double, 12> pcTime {};
    double noteTime = 0.0;
    for (const auto& e : events)
    {
        pcTime[(size_t) (e.pitch % 12)] += e.end - e.start;
        noteTime += e.end - e.start;
    }
    c.onePitch = noteTime > 0.0 ? *std::max_element (pcTime.begin(), pcTime.end()) / noteTime : 0.0;

    const double sounding = juce::jmax (1.0, c.sounding * n);
    c.noteCoverage = juce::jmin (1.0, covered / sounding);
    c.dense = juce::jmin (1.0, dense / sounding);
}

// Each cue alone also fits some stems (a loud synth, a piano playing bass and chords, a plucky bass),
// so a file is called a full song only when most of them agree.
Verdict classify (const Cues& c)
{
    // kick and snare hits together: stems have one or the other (plucked bass, strummed chords)
    const bool drums = c.kickRate >= 0.5 && c.snareRate >= 0.5;
    const bool harmony = c.noteCoverage >= 0.4;
    const double hits = c.kickRate + c.snareRate;

    // drums only: mostly percussive, or percussive with few notes or all notes on one pitch (a tuned kick)
    if (hits >= 0.5 && ((c.percussive >= 0.5 && c.noteCoverage < 0.5)
                        || (c.percussive >= 0.25 && (c.noteCoverage < 0.25 || c.onePitch >= 0.9))))
        return Verdict::drumsOnly;

    const bool drumsAndHarmony = drums && harmony;
    const bool fullBand = c.fullBand >= 0.6;
    const bool mastered = c.lufs >= -15.0 && c.lufs <= -5.0 && c.crestDb <= 13.0;
    const bool dense = c.dense >= 0.3;
    const int score = (int) drumsAndHarmony + (int) fullBand + (int) mastered + (int) dense;
    if (score >= 3 && (drumsAndHarmony || fullBand))
        return Verdict::fullSong;

    // a few stray notes (risers, impacts, effects) give no chords worth showing
    if (c.noteCoverage < 0.1)
        return c.percussive >= 0.25 || hits >= 0.5 ? Verdict::drumsOnly : Verdict::noNotes;
    return Verdict::ok;
}

//==============================================================================
Placed place (const std::vector<NoteEvent>& events, double bpm, int ts)
{
    Placed out;
    if (events.empty() || bpm <= 0.0) return out;
    const int spBar = cg::timeSigs()[(size_t) ts].steps;
    const double stepsPerSecond = bpm / 60.0 * cg::stepsPerBeat;

    std::vector<cg::Note> all;
    for (const auto& e : events)
    {
        const cg::Note n { e.pitch, (int) std::lround (e.start * stepsPerSecond),
                           juce::jmax (1, (int) std::lround ((e.end - e.start) * stepsPerSecond)) };
        if (std::none_of (all.begin(), all.end(), [&] (const cg::Note& o) { return o.p == n.p && o.s == n.s; }))
            all.push_back (n);
    }
    std::sort (all.begin(), all.end(), [] (const cg::Note& a, const cg::Note& b) { return a.s != b.s ? a.s < b.s : a.p < b.p; });

    int lastEnd = 0;
    for (const auto& n : all) lastEnd = std::max (lastEnd, n.s + n.d);
    out.totalBars = (lastEnd + spBar - 1) / spBar;
    out.firstBar = all.front().s / spBar;

    // stems often start with silence: begin at the first bar with notes, keeping the bar lines
    const int offset = out.firstBar * spBar;
    const int needed = (lastEnd - offset + spBar - 1) / spBar;
    out.bars = needed <= 4 ? 4 : needed <= 8 ? 8 : 16;
    const int end = out.bars * spBar;
    for (auto n : all)
    {
        n.s -= offset;
        if (n.s >= end) continue;
        n.d = std::min (n.d, end - n.s);
        out.notes.push_back (n);
    }
    return out;
}
} // namespace audioimport
