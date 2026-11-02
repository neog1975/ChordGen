#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include "Engine.h"
#include "Pitch.h"

#ifndef CHORDGEN_FX
 #define CHORDGEN_FX 0 // 1 = the mixer-insert variant that listens to audio
#endif

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

class AudioImportJob;

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
    bool optCapture = true, optFollow = true, optInternal = true, optThru = true, optPreview = false,
         optLive = false, optMetronome = true;
    static constexpr bool isFx = CHORDGEN_FX != 0;

    float inputLevel() const { return inputPeak.load(); }
    float livePitch() const { return tracker.pitch.load(); }
    juce::String liveChord; // Live mode: the chord playing now
    void setOption (const juce::String& name, bool value);
    void auditionNote (int note);

    struct HostInfo
    {
        double bpm = 120.0, loopPos = 0.0;
        bool hostPlaying = false, active = false, bpmKnown = false;
    };
    HostInfo getHostInfo() const;

    // ChordGen FX: chords for an audio file (a stem), analysed on a background thread.
    void importAudio (const juce::File&, double bpm);
    void cancelImport();
    struct ImportStatus { bool running = false; float progress = 0.0f; juce::String file; };
    ImportStatus importStatus() const;
    struct Notice { juce::String title, body, tip; };
    Notice notice; // why the last file gave no chords; cleared when the user dismisses it

private:
    friend class PitchThread;
    void timerCallback() override;
    void drainAudio();
    void onPitchNote (const PitchTracker::DetectedNote&);
    void updateLiveChord (int bar);
    void onCapturedNotes (const std::vector<CapturedNote>&);
    void commitPending();
    void applyPattern();
    std::vector<CapturedNote> popCapturedNotes();
    std::shared_ptr<const Pattern> currentPattern() const;
    void allNotesOff (juce::MidiBuffer& out, int sample);
    void finishImport();

    std::vector<cg::State> undoStack, redoStack;
    std::unique_ptr<AudioImportJob> importJob;

    // audio / mic input
    PitchTracker tracker;
    std::unique_ptr<juce::Thread> pitchThread;
    juce::AbstractFifo audioFifo { 1 << 16 };
    std::vector<AudioFrame> audioFifoData, scratch;
    std::atomic<double> trackerRate { 11025.0 };
    double appliedTrackerRate = 0.0;
    int decim = 4, accN = 0;
    float acc = 0.0f;
    std::atomic<float> inputPeak { 0.0f };
    std::atomic<bool> audioCapture { false }, metronomeOn { true };
    std::atomic<int> beatsPerBar { 4 };
    double clickPhase = 0.0, clickFreq = 1200.0;
    int clickLeft = 0;

    // Live mode
    std::shared_ptr<const Pattern> livePattern;
    int liveBarKey = -1, liveBar = 0;
    std::atomic<bool> infoRunning { false };
    std::vector<std::pair<juce::String, cg::State>> favorites;
    int pendingPass = -1, passSeen = -1, passTicks = 0;
    bool hostWasPlaying = false;

    juce::Synthesiser synth;
    std::atomic<float> cutoff { 2400.0f }, attack { 0.012f };

    mutable juce::SpinLock patternLock;
    std::shared_ptr<const Pattern> pattern;

    juce::SpinLock captureLock;
    std::vector<CapturedNote> captured;
    struct OpenNote { double start = -1.0, abs = 0.0; int pass = 0; };
    std::array<OpenNote, 128> openNotes {};

    juce::SpinLock auditionLock;
    std::vector<int> pendingAuditions;
    struct Audition { int note; int samplesLeft; };
    std::vector<Audition> auditions;

    struct ActiveNote { int note, channel; double offBeat; };
    std::vector<ActiveNote> activeNotes;

    std::atomic<bool> previewOn { false }, followHost { true }, internalSound { true },
                      midiThru { true }, captureOn { true };

    std::atomic<double> infoBpm { 120.0 }, infoLoopPos { 0.0 };
    std::atomic<bool> infoHostPlaying { false }, infoActive { false }, infoBpmKnown { false };

    double sampleRate = 44100.0;
    double previewBeat = 0.0, lastPpq = 0.0, lastPpqPlayed = 0.0, lastBlockEnd = 0.0, clockOffset = 0.0;
    bool wasHostPlaying = false, wasActive = false, wasPreview = false, wasRunning = false;
    int pass = 0;
    std::atomic<int> currentPass { 0 };
    long long lastLoopIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordGenProcessor)
};
