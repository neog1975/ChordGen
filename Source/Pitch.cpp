#include "Pitch.h"
#include <algorithm>
#include <cmath>

PitchTracker::PitchTracker (std::function<void (const DetectedNote&)> cb)
    : onNote (std::move (cb)), ring ((size_t) window), buf ((size_t) window), diff ((size_t) window)
{
}

void PitchTracker::setSampleRate (double decimatedRate)
{
    rate = decimatedRate;
    reset();
}

void PitchTracker::reset()
{
    writePos = filled = sinceHop = 0;
    recent.clear();
    candidate = current = -1;
    candidateFrames = silentFrames = currentFrames = 0;
    pitch = 0.0f;
}

void PitchTracker::process (const AudioFrame* frames, int num)
{
    for (int i = 0; i < num; ++i)
    {
        ring[(size_t) writePos] = frames[i];
        writePos = (writePos + 1) % window;
        filled = std::min (filled + 1, window);
        if (++sinceHop >= hop && filled == window)
        {
            sinceHop = 0;
            analyseWindow();
        }
    }
}

void PitchTracker::analyseWindow()
{
    double energy = 0.0;
    for (int i = 0; i < window; ++i)
    {
        buf[(size_t) i] = ring[(size_t) ((writePos + i) % window)].x;
        energy += buf[(size_t) i] * buf[(size_t) i];
    }
    const auto& at = ring[(size_t) ((writePos + window / 2) % window)]; // time of the window centre
    const double rms = std::sqrt (energy / window);

    const int W = window / 2;
    const int tauMin = std::max (2, (int) (rate / 1100.0));  // up to ~C6
    const int tauMax = std::min (W - 1, (int) (rate / 65.0)); // down to ~C2

    if (rms < 0.008) // about -42 dBFS: silence or breath
    {
        track (0.0f, false, at);
        return;
    }

    // YIN: difference function and cumulative mean normalised difference
    diff[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < W; ++j)
        {
            const double delta = buf[(size_t) j] - buf[(size_t) (j + tau)];
            d += delta * delta;
        }
        running += d;
        diff[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
    }

    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (diff[(size_t) tau] < 0.15f)
        {
            while (tau + 1 <= tauMax && diff[(size_t) (tau + 1)] < diff[(size_t) tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = tauMin;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (diff[(size_t) tau] < diff[(size_t) best]) best = tau;
        if (diff[(size_t) best] > 0.3f) { track (0.0f, false, at); return; }
    }

    // parabolic interpolation around the minimum
    double tau = best;
    if (best > 1 && best < tauMax)
    {
        const double a = diff[(size_t) (best - 1)], b = diff[(size_t) best], c = diff[(size_t) (best + 1)];
        const double denom = a - 2.0 * b + c;
        if (std::abs (denom) > 1e-9) tau += 0.5 * (a - c) / denom;
    }
    const double f0 = rate / tau;
    track ((float) (69.0 + 12.0 * std::log2 (f0 / 440.0)), true, at);
}

void PitchTracker::track (float midi, bool voiced, const AudioFrame& at)
{
    if (! voiced)
    {
        pitch = 0.0f;
        recent.clear();
        candidate = -1;
        if (current >= 0 && ++silentFrames >= 4)
            endNote (lastVoicedBeat);
        return;
    }

    silentFrames = 0;
    recent.push_back (midi);
    if (recent.size() > 5) recent.erase (recent.begin());
    auto sorted = recent;
    std::sort (sorted.begin(), sorted.end());
    const float smooth = sorted[sorted.size() / 2];
    pitch = smooth;
    lastVoicedBeat = at.beat;

    if (current >= 0 && std::abs (smooth - (float) current) < 0.75f)
    {
        ++currentFrames;
        candidate = -1;
        return;
    }

    // a new pitch has to hold for three hops (~35 ms) before it counts as a note
    const int r = (int) std::lround (smooth);
    if (r == candidate) ++candidateFrames;
    else { candidate = r; candidateFrames = 1; candidateAt = at; }

    if (candidateFrames >= 3)
    {
        if (current >= 0) endNote (candidateAt.beat);
        current = candidate;
        currentAt = candidateAt;
        currentFrames = candidateFrames;
        candidate = -1;
    }
}

void PitchTracker::endNote (double endBeat)
{
    // ignore blips shorter than about 60 ms and anything heard while no transport ran
    if (current >= 0 && currentFrames >= 5 && currentAt.clock && onNote)
        onNote ({ current, currentAt.beat, endBeat, currentAt.pass });
    current = -1;
    currentFrames = 0;
    silentFrames = 0;
}
