// ChordGen: native port of Basic Pitch's feature model (features_model.onnx), which NeuralNote runs
// with ONNX Runtime. nnAudio's CQT2010v2 (9 octaves of 36 bins, 256-tap kernels, the signal halved
// by a 256-tap low-pass between octaves), then 10 log10 power normalised to 0..1 over the whole
// file, the folded batch norm, and harmonic stacking at 0.5, 1, 2 ... 7 times the frequency.

#include "Features.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <Eigen/Dense>

#include "BasicPitchData.h"

Features::Features()
{
    const auto* data = reinterpret_cast<const float*>(BasicPitchData::features_cqt_bin);
    const size_t expected = 2 * kBins * kTaps + kTaps + kBinsKept + 2;
    if (BasicPitchData::features_cqt_binSize != (int) (expected * sizeof(float)))
        return;

    std::vector<float> all(expected);
    std::memcpy(all.data(), data, expected * sizeof(float));
    auto it = all.begin();
    mReal.assign(it, it + kBins * kTaps);
    it += kBins * kTaps;
    mImag.assign(it, it + kBins * kTaps);
    it += kBins * kTaps;
    mLowpass.assign(it, it + kTaps);
    it += kTaps;
    mScale.assign(it, it + kBinsKept);
    it += kBinsKept;
    mGain = *it++;
    mOffset = *it;

    mKernels.resize(2 * kBins, kTaps);
    for (int b = 0; b < kBins; ++b)
        for (int j = 0; j < kTaps; ++j) {
            mKernels(b, j) = mReal[(size_t) (b * kTaps + j)];
            mKernels(b + kBins, j) = mImag[(size_t) (b * kTaps + j)];
        }
}

const float* Features::computeFeatures(const float* inAudio, size_t inNumSamples, size_t& outNumFrames)
{
    outNumFrames = 0;
    if (mReal.empty() || inNumSamples < 2 * kTaps)
        return nullptr;

    // Octave k uses hop 256 / 2^k on the signal halved k times, so every octave yields the same frames.
    std::vector<float> x(inAudio, inAudio + inNumSamples);
    std::vector<std::vector<float>> mag(kOctaves); // [octave][frame * kBins + bin], power
    size_t frames = SIZE_MAX;

    for (int k = 0; k < kOctaves; ++k) {
        const int hop = kTaps >> k;
        const auto len = (long) x.size();
        if (len < 2)
            return nullptr;

        // reflect padding of half a kernel on both sides
        const int half = kTaps / 2;
        std::vector<float> padded((size_t) (len + kTaps));
        for (long i = 0; i < len + kTaps; ++i) {
            long j = i - half;
            if (j < 0)
                j = -j;
            if (j >= len)
                j = 2 * (len - 1) - j;
            padded[(size_t) i] = x[(size_t) std::clamp(j, 0L, len - 1)];
        }

        // all frames at once: [36 real + 36 imaginary kernels] x [256 taps] times [256 taps] x [frames]
        const size_t n = (size_t) (len / hop + 1);
        frames = std::min(frames, n);
        Eigen::MatrixXf segments(kTaps, (Eigen::Index) n);
        for (size_t t = 0; t < n; ++t)
            std::copy(padded.begin() + (ptrdiff_t) (t * (size_t) hop),
                      padded.begin() + (ptrdiff_t) (t * (size_t) hop + kTaps),
                      segments.col((Eigen::Index) t).data());
        const Eigen::MatrixXf out = mKernels * segments;
        auto& m = mag[(size_t) k];
        m.assign(n * kBins, 0.0f);
        for (size_t t = 0; t < n; ++t)
            for (int b = 0; b < kBins; ++b) {
                const float re = out(b, (Eigen::Index) t), im = out(b + kBins, (Eigen::Index) t);
                m[t * kBins + (size_t) b] = re * re + im * im;
            }

        if (k == kOctaves - 1)
            break;

        // halve the rate: zero padding of 127 on both sides, low-pass, keep every other sample
        const int pad = kTaps / 2 - 1;
        const long outLen = (len - 2) / 2 + 1;
        std::vector<float> down((size_t) std::max(0L, outLen));
        const Eigen::Map<const Eigen::VectorXf> lowpass(mLowpass.data(), kTaps);
        for (long o = 0; o < outLen; ++o) {
            const long start = 2 * o - pad;
            const int j0 = (int) std::max(0L, -start), j1 = (int) std::min((long) kTaps, len - start);
            down[(size_t) o] = j1 > j0 ? lowpass.segment(j0, j1 - j0).dot(
                                             Eigen::Map<const Eigen::VectorXf>(x.data() + start + j0, j1 - j0))
                                       : 0.0f;
        }
        x.swap(down);
    }

    // bins from the lowest octave up; the lowest 15 of the 324 are dropped
    const int total = kOctaves * kBins, first = total - kBinsKept;
    std::vector<float> db(frames * kBinsKept);
    float lo = INFINITY, hi = -INFINITY;
    for (size_t t = 0; t < frames; ++t)
        for (int f = 0; f < kBinsKept; ++f) {
            const int g = f + first, octave = kOctaves - 1 - g / kBins, bin = g % kBins;
            const float s = mScale[(size_t) f];
            const float power = mag[(size_t) octave][t * kBins + (size_t) bin] * s * s;
            const float v = 10.0f * std::log10(power + 1e-10f);
            db[t * kBinsKept + (size_t) f] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    const float range = hi - lo;
    for (auto& v: db)
        v = (range == 0.0f ? 0.0f : (v - lo) / range) * mGain + mOffset;

    static constexpr int shifts[NUM_HARMONICS] = {-36, 0, 36, 57, 72, 84, 93, 101};
    mOutput.assign(frames * NUM_FREQ_IN * NUM_HARMONICS, 0.0f);
    for (size_t t = 0; t < frames; ++t)
        for (int f = 0; f < NUM_FREQ_IN; ++f)
            for (int h = 0; h < NUM_HARMONICS; ++h) {
                const int src = f + shifts[h];
                if (src >= 0 && src < kBinsKept)
                    mOutput[(t * NUM_FREQ_IN + (size_t) f) * NUM_HARMONICS + (size_t) h] = db[t * kBinsKept + (size_t) src];
            }

    outNumFrames = frames;
    return mOutput.data();
}
