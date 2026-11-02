// Copied unchanged from the author's Forge Master project (Source/dsp/LoudnessMeter.h).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// ITU-R BS.1770-4 / EBU R128 loudness meter for stereo.
// Momentary (400 ms), short-term (3 s) and gated integrated loudness.
// Integrated uses a fixed histogram so the audio thread never allocates.
class LoudnessMeter
{
public:
    void prepare (double fs)
    {
        constexpr double pi = 3.14159265358979323846;

        // Stage 1: high-shelf (head model)
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan (pi * f0 / fs);
        double Vh = std::pow (10.0, G / 20.0);
        double Vb = std::pow (Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                  2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };

        // Stage 2: RLB high-pass
        f0 = 38.13547087602444; Q = 0.5003270373238773;
        K = std::tan (pi * f0 / fs);
        a0 = 1.0 + K / Q + K * K;
        hipass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };

        blockLen = (int) std::lround (fs * 0.1);
        reset();
    }

    void reset()
    {
        for (auto& s : state) s = {};
        acc = 0.0; count = 0; filled = 0; ringPos = 0;
        ring.fill (0.0);
        hist.fill (0);
        momentary = shortTerm = integrated = -100.0f;
        ticks = 0;
    }

    void requestReset() { resetPending.store (true); }

    void process (const float* L, const float* R, int n)
    {
        if (resetPending.exchange (false))
            reset();

        for (int i = 0; i < n; ++i)
        {
            const double l = filter (0, L[i]);
            const double r = filter (1, R[i]);
            acc += l * l + r * r;

            if (++count >= blockLen)
                finishBlock();
        }
    }

    float getMomentary() const  { return momentaryOut.load(); }
    float getShortTerm() const  { return shortTermOut.load(); }
    float getIntegrated() const { return integratedOut.load(); }

    // Number of completed 100 ms blocks; lets the processor react once per block.
    int getTicks() const { return ticks; }

private:
    struct Biquad { double b0, b1, b2, a1, a2; };
    struct State  { double s1 = 0, s2 = 0, h1 = 0, h2 = 0; };

    static constexpr int ringSize = 30;          // 30 x 100 ms = 3 s
    static constexpr int histBins = 800;         // -70 .. +10 LUFS in 0.1 dB steps
    static constexpr double histMin = -70.0;

    double filter (int ch, double x)
    {
        auto& s = state[(size_t) ch];
        double y = shelf.b0 * x + s.s1;
        s.s1 = shelf.b1 * x - shelf.a1 * y + s.s2;
        s.s2 = shelf.b2 * x - shelf.a2 * y;

        double z = hipass.b0 * y + s.h1;
        s.h1 = hipass.b1 * y - hipass.a1 * z + s.h2;
        s.h2 = hipass.b2 * y - hipass.a2 * z;
        return z;
    }

    static double toLufs (double energy) { return energy > 0.0 ? -0.691 + 10.0 * std::log10 (energy) : -100.0; }
    static double fromLufs (double lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

    double meanOfLast (int blocks) const
    {
        const int avail = std::min (blocks, filled);
        if (avail == 0) return 0.0;
        double sum = 0.0;
        for (int k = 1; k <= avail; ++k)
            sum += ring[(size_t) ((ringPos - k + ringSize) % ringSize)];
        return sum / avail;
    }

    void finishBlock()
    {
        ring[(size_t) ringPos] = acc / blockLen;
        ringPos = (ringPos + 1) % ringSize;
        filled = std::min (filled + 1, ringSize);
        acc = 0.0; count = 0;
        ++ticks;

        const double mEnergy = meanOfLast (4);
        momentary = (float) toLufs (mEnergy);
        shortTerm = (float) toLufs (meanOfLast (ringSize));

        // 400 ms gating blocks with 75 % overlap feed the integrated histogram
        if (filled >= 4 && momentary > histMin)
        {
            const int bin = std::clamp ((int) ((momentary - histMin) * 10.0), 0, histBins - 1);
            ++hist[(size_t) bin];
            integrated = (float) computeIntegrated();
        }

        momentaryOut.store (momentary);
        shortTermOut.store (shortTerm);
        integratedOut.store (integrated);
    }

    double computeIntegrated() const
    {
        double sum = 0.0; long total = 0;
        for (int b = 0; b < histBins; ++b)
            if (hist[(size_t) b] > 0)
            {
                sum += hist[(size_t) b] * fromLufs (histMin + (b + 0.5) * 0.1);
                total += hist[(size_t) b];
            }
        if (total == 0) return -100.0;

        const double relGate = toLufs (sum / total) - 10.0;
        sum = 0.0; total = 0;
        for (int b = 0; b < histBins; ++b)
        {
            const double lufs = histMin + (b + 0.5) * 0.1;
            if (hist[(size_t) b] > 0 && lufs >= relGate)
            {
                sum += hist[(size_t) b] * fromLufs (lufs);
                total += hist[(size_t) b];
            }
        }
        return total > 0 ? toLufs (sum / total) : -100.0;
    }

    Biquad shelf {}, hipass {};
    std::array<State, 2> state {};
    std::array<double, ringSize> ring {};
    std::array<int, histBins> hist {};
    double acc = 0.0;
    int blockLen = 4800, count = 0, filled = 0, ringPos = 0, ticks = 0;
    float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f;
    std::atomic<float> momentaryOut { -100.0f }, shortTermOut { -100.0f }, integratedOut { -100.0f };
    std::atomic<bool> resetPending { false };
};
