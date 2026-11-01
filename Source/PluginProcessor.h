#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include "Engine.h"

// One note of the chord pattern. Times are in beats (quarter notes) from the start of the loop.
// Channel 1 = chords, 2 = bass, 3 = melody.
struct PatternEvent
{
    double beat = 0.0, length = 0.0;
    int note = 60, channel = 1;
    float velocity = 0.8f;
};

struct Pattern
{
    std::vector<PatternEvent> events;
    double loopBeats = 16.0;
};

// A melody note heard from FL while the transport was running.
struct CapturedNote
{
    int pitch = 60;
    double start = 0.0, end = 0.0; // host ppq
    int pass = 0;                  // increments on every new loop pass / transport start
};

class ChordGenProcessor : public juce::AudioProcessor,
                          public juce::ChangeBroadcaster,
                          private juce::Timer
{
public:
    ChordGenProcessor();
    ~ChordGenProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ChordGen"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Message thread: the editable state, its analysis and history.
    cg::State state;
    cg::Analysis analysis;

    void commit (bool resuggest);  // re-analyse, update the playing pattern, notify the editor
    void pushHistory();
    void undo();
    void redo();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    void regenerate();
    void saveFavorite();
    void restoreFavorite (int index);
    void clearFavorites();
    juce::StringArray favoriteNames() const;

    void showToast (const juce::String& text) { toastText = text; ++toastId; sendChangeMessage(); }
    juce::String toastText;
    int toastId = 0;

    std::vector<cg::Note> pending; // notes heard in the current FL pass, not yet committed
    int accent = 0;

    // options that live outside undo
    bool optCapture = true, optFollow = true, optInternal = true, optThru = true, optPreview = false;
    void setOption (const juce::String& name, bool value);
    void auditionNote (int note);

    struct HostInfo
    {
        double bpm = 120.0, loopPos = 0.0;
        bool hostPlaying = false, active = false;
    };
    HostInfo getHostInfo() const;

private:
    void timerCallback() override;
    void onCapturedNotes (const std::vector<CapturedNote>&);
    void commitPending();
    void applyPattern();
    std::vector<CapturedNote> popCapturedNotes();
    std::shared_ptr<const Pattern> currentPattern() const;
    void allNotesOff (juce::MidiBuffer& out, int sample);

    std::vector<cg::State> undoStack, redoStack;
    std::vector<std::pair<juce::String, cg::State>> favorites;
    int pendingPass = -1;
    bool hostWasPlaying = false;

    juce::Synthesiser synth;
    std::atomic<float> cutoff { 2400.0f }, attack { 0.012f };

    mutable juce::SpinLock patternLock;
    std::shared_ptr<const Pattern> pattern;

    juce::SpinLock captureLock;
    std::vector<CapturedNote> captured;
    std::array<double, 128> openNotes {};

    juce::SpinLock auditionLock;
    std::vector<int> pendingAuditions;
    struct Audition { int note; int samplesLeft; };
    std::vector<Audition> auditions;

    struct ActiveNote { int note, channel; double offBeat; };
    std::vector<ActiveNote> activeNotes;

    std::atomic<bool> previewOn { false }, followHost { true }, internalSound { true },
                      midiThru { true }, captureOn { true };

    std::atomic<double> infoBpm { 120.0 }, infoLoopPos { 0.0 };
    std::atomic<bool> infoHostPlaying { false }, infoActive { false };

    double sampleRate = 44100.0;
    double previewBeat = 0.0, lastPpq = 0.0, lastPpqPlayed = 0.0;
    bool wasHostPlaying = false, wasActive = false, wasPreview = false;
    int pass = 0;
    long long lastLoopIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordGenProcessor)
};
