// Copied unchanged from the author's Forge Master project (Source/dsp/MixAnalyzer.h).
#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include "LoudnessMeter.h"

// Listens to the incoming mix for a fixed amount of non-silent audio and
// summarises it for the auto-master: octave-band spectrum, crest factor,
// stereo correlation, low-end side energy and integrated loudness.
// Accumulates on the audio thread; the result is handed over through an
// atomic flag and read on the message thread.
class MixAnalyzer
{
public:
    static constexpr int numBands = 10;
    static constexpr float centres[numBands] { 40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 16000 };

    struct Result
    {
        std::array<float, numBands> bandDb {};
        float crestDb = 0.0f;
        float correlation = 1.0f;
        float lowSideDb = -100.0f;   // side vs mid energy below ~150 Hz
        float integratedLufs = -100.0f;
    };

    void prepare (double fs, float secondsToListen = 20.0f)
    {
        sampleRate = fs;
        needed = (long) (fs * secondsToListen);

        for (int k = 0; k < numBands; ++k)
        {
            const double f = std::min ((double) centres[k], fs * 0.45);
            bands[(size_t) k] = bandPass (f, 1.41);
        }
        lowM = lowS = lowPass (150.0);
        meter.prepare (fs);
        active.store (false);
    }

    void start()   { startPending.store (true); }
    void cancel()  { cancelPending.store (true); }
    bool isListening() const { return active.load(); }
    float getProgress() const { return progress.load(); }

    // True once after each completed analysis; copies the result out.
    bool takeResult (Result& out)
    {
        if (! resultReady.exchange (false, std::memory_order_acquire))
            return false;
        out = result;
        return true;
    }

    void process (const float* L, const float* R, int n, bool audible)
    {
        if (startPending.exchange (false))
            reset();
        if (cancelPending.exchange (false))
            active.store (false);
        if (! active.load() || ! audible)
            return;

        meter.process (L, R, n);

        for (int i = 0; i < n; ++i)
        {
            const double l = L[i], r = R[i];
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);

            for (int k = 0; k < numBands; ++k)
            {
                const double y = bands[(size_t) k].process (m);
                bandEnergy[(size_t) k] += y * y;
            }
            const double lm = lowM.process (m), ls = lowS.process (s);
            lowMidEnergy += lm * lm;
            lowSideEnergy += ls * ls;

            sumLL += l * l; sumRR += r * r; sumLR += l * r;
            peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
        }

        count += n;
        progress.store (std::min (1.0f, (float) count / (float) needed));

        if (count >= needed)
            finish();
    }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    Biquad bandPass (double f, double q) const
    {
        const double w = 2.0 * 3.14159265358979323846 * f / sampleRate;
        const double alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        Biquad b;
        b.b0 = alpha / a0; b.b1 = 0.0; b.b2 = -alpha / a0;
        b.a1 = -2.0 * std::cos (w) / a0; b.a2 = (1.0 - alpha) / a0;
        return b;
    }

    Biquad lowPass (double f) const
    {
        const double w = 2.0 * 3.14159265358979323846 * f / sampleRate;
        const double alpha = std::sin (w) / (2.0 * 0.7071), a0 = 1.0 + alpha, c = std::cos (w);
        Biquad b;
        b.b0 = (1.0 - c) / 2.0 / a0; b.b1 = (1.0 - c) / a0; b.b2 = b.b0;
        b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
        return b;
    }

    static float toDb (double energy) { return energy > 1e-20 ? (float) (10.0 * std::log10 (energy)) : -200.0f; }

    void reset()
    {
        for (auto& b : bands) b.z1 = b.z2 = 0.0;
        lowM.z1 = lowM.z2 = lowS.z1 = lowS.z2 = 0.0;
        bandEnergy.fill (0.0);
        lowMidEnergy = lowSideEnergy = sumLL = sumRR = sumLR = peak = 0.0;
        count = 0;
        progress.store (0.0f);
        meter.reset();
        active.store (true);
    }

    void finish()
    {
        for (int k = 0; k < numBands; ++k)
            result.bandDb[(size_t) k] = toDb (bandEnergy[(size_t) k] / (double) count);

        const double meanSquare = (sumLL + sumRR) / (2.0 * (double) count);
        result.crestDb = toDb (peak * peak) - toDb (meanSquare);
        result.correlation = (sumLL > 0.0 && sumRR > 0.0) ? (float) (sumLR / std::sqrt (sumLL * sumRR)) : 1.0f;
        result.lowSideDb = toDb (lowSideEnergy) - toDb (lowMidEnergy);
        result.integratedLufs = meter.getIntegrated();

        active.store (false);
        progress.store (1.0f);
        resultReady.store (true, std::memory_order_release);
    }

    double sampleRate = 44100.0;
    long needed = 0, count = 0;
    std::array<Biquad, numBands> bands {};
    Biquad lowM, lowS;
    std::array<double, numBands> bandEnergy {};
    double lowMidEnergy = 0, lowSideEnergy = 0, sumLL = 0, sumRR = 0, sumLR = 0, peak = 0;
    LoudnessMeter meter;
    Result result;

    std::atomic<bool> startPending { false }, cancelPending { false }, active { false }, resultReady { false };
    std::atomic<float> progress { 0.0f };
};
