// ChordGen: replaces NeuralNote's ONNX Runtime front end with a native port of the same graph
// (features_model.onnx). Constants come from ModelData/features_cqt.bin, see export_features.py.

#ifndef Features_h
#define Features_h

#include <cstddef>
#include <vector>

#include <Eigen/Core>

#include "BasicPitchConstants.h"

/**
 * Class to compute the CQT and harmonically stack those. Output of this can be given as input to Basic Pitch cnn.
 */
class Features
{
public:
    Features();

    /**
     * Compute features for full audio signal
     * @param inAudio Input audio at 22050 Hz. Should contain inNumSamples
     * @param inNumSamples Number of samples in inAudio
     * @param outNumFrames Number of frames that have been computed.
     * @return Pointer to features, [frame][NUM_FREQ_IN][NUM_HARMONICS].
     */
    const float* computeFeatures(const float* inAudio, size_t inNumSamples, size_t& outNumFrames);

private:
    static constexpr int kBins = 36, kTaps = 256, kOctaves = 9, kBinsKept = 309;

    std::vector<float> mReal, mImag, mLowpass, mScale;
    float mGain = 1.0f, mOffset = 0.0f;
    Eigen::MatrixXf mKernels; // [36 real; 36 imaginary] x 256
    std::vector<float> mOutput;
};

#endif // Features_h
