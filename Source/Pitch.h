#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <vector>

// One decimated input sample with the musical time it belongs to.
struct AudioFrame
{
    float x = 0.0f;
    double beat = 0.0; // host ppq, or the preview clock
    int pass = 0;      // capture pass at this sample
    bool clock = false; // a transport (FL or Preview) is running
};

// Monophonic pitch tracking (YIN) and note segmentation for an audio track or a microphone.
// Feed it frames from one thread; it reports finished notes through onNote.
class PitchTracker
{
public:
    struct DetectedNote { int pitch; double start, end; int pass; };

    explicit PitchTracker (std::function<void (const DetectedNote&)> onNoteCallback);

    void setSampleRate (double decimatedRate);
    void process (const AudioFrame* frames, int num);
    void reset();

    std::atomic<float> pitch { 0.0f }; // current MIDI pitch with fraction; 0 = no pitch

private:
    void analyseWindow();
    void track (float midi, bool voiced, const AudioFrame& at);
    void endNote (double endBeat);

    std::function<void (const DetectedNote&)> onNote;
    double rate = 11025.0;
    static constexpr int window = 1024, hop = 128;

    std::vector<AudioFrame> ring;
    int writePos = 0, filled = 0, sinceHop = 0;
    std::vector<float> buf, diff;

    std::vector<float> recent; // last voiced pitches, for a median filter
    int candidate = -1, candidateFrames = 0, silentFrames = 0;
    AudioFrame candidateAt;
    int current = -1, currentFrames = 0;
    AudioFrame currentAt;
    double lastVoicedBeat = 0.0;
};
