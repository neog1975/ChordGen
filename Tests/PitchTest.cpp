// Feeds a synthetic "sung" melody (harmonics, vibrato, noise, gaps) through the pitch tracker
// and prints the notes it found. Run: build/PitchTests_artefacts/Release/PitchTests
#include "../Source/Pitch.h"
#include <cmath>
#include <iostream>
#include <random>

int main()
{
    const double rate = 11025.0, bpm = 100.0;
    std::vector<PitchTracker::DetectedNote> found;
    PitchTracker tracker ([&] (const PitchTracker::DetectedNote& n) { found.push_back (n); });
    tracker.setSampleRate (rate);

    struct N { int pitch; double start, length; }; // beats
    const std::vector<N> melody { { 57, 0, 1 }, { 60, 1, 0.5 }, { 64, 1.5, 1.5 }, { 62, 3.25, 0.5 }, { 60, 4, 2 }, { 55, 6.5, 1 }, { 69, 8, 2 } };
    const double totalBeats = 11.0;
    const int total = (int) (totalBeats * 60.0 / bpm * rate);

    std::mt19937 rng (7);
    std::normal_distribution<float> noise (0.0f, 0.01f);
    std::vector<AudioFrame> frames ((size_t) total);
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double beat = i * bpm / 60.0 / rate, t = i / rate;
        float x = 0.0f;
        for (const auto& n : melody)
        {
            if (beat < n.start || beat >= n.start + n.length - 0.05) continue;
            const double f = 440.0 * std::pow (2.0, (n.pitch - 69 + 0.25 * std::sin (2 * 3.14159 * 5.5 * t)) / 12.0);
            phase += 2 * 3.14159265 * f / rate;
            x = (float) (0.4 * std::sin (phase) + 0.2 * std::sin (2 * phase) + 0.1 * std::sin (3 * phase));
        }
        frames[(size_t) i] = { x + noise (rng), beat, 1, true };
    }
    tracker.process (frames.data(), total);
    // a little silence to close the last note
    std::vector<AudioFrame> tail (2000, AudioFrame { 0.0f, totalBeats, 1, true });
    tracker.process (tail.data(), (int) tail.size());

    int hits = 0;
    for (const auto& n : melody)
    {
        bool ok = false;
        for (const auto& f : found)
            ok = ok || (f.pitch == n.pitch && std::abs (f.start - n.start) < 0.2);
        hits += ok;
        std::cout << "expected " << n.pitch << " @" << n.start << (ok ? "  found\n" : "  MISSING\n");
    }
    std::cout << "detected:";
    for (const auto& f : found) std::cout << " " << f.pitch << "@" << std::round (f.start * 100) / 100 << "-" << std::round (f.end * 100) / 100;
    std::cout << "\n" << hits << "/" << melody.size() << " notes found, " << found.size() << " detected\n";
    return hits == (int) melody.size() ? 0 : 1;
}
