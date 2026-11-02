#include "PluginProcessor.h"
#include "PluginEditor.h"
#if CHORDGEN_FX
 #include "AudioImport.h"
#endif

namespace
{
constexpr int bassChannel = 2, melodyChannel = 3;

struct AnySound : juce::SynthesiserSound
{
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

// Small built-in synth so the plugin makes sound on its own:
// detuned saws through a low-pass for chords, sine for bass, triangle for the melody.
class ChordVoice : public juce::SynthesiserVoice
{
public:
    ChordVoice (std::atomic<float>& cutoffRef, std::atomic<float>& attackRef)
        : cutoff (cutoffRef), attack (attackRef) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<AnySound*> (s) != nullptr; }

    void startNote (int note, float velocity, juce::SynthesiserSound*, int) override
    {
        kind = isPlayingChannel (bassChannel) ? 1 : isPlayingChannel (melodyChannel) ? 2 : 0;
        freq = juce::MidiMessage::getMidiNoteInHertz (note);
        level = velocity;
        phase1 = phase2 = 0.0;
        lp = 0.0f;

        juce::ADSR::Parameters p;
        if (kind == 0)      p = { attack.load(), 0.4f, 0.65f, 0.25f };
        else if (kind == 1) p = { 0.005f, 0.3f, 0.6f, 0.08f };
        else                p = { 0.005f, 0.2f, 0.5f, 0.08f };

        env.setSampleRate (getSampleRate());
        env.setParameters (p);
        env.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
            env.noteOff();
        else
        {
            env.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! isVoiceActive())
            return;

        const auto sr = getSampleRate();
        const auto inc1 = freq / sr, inc2 = freq * 1.004 / sr;
        const auto a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmax (1600.0f, cutoff.load() * 1.4f) / (float) sr);

        for (int i = 0; i < num; ++i)
        {
            float s;
            if (kind == 0)
            {
                s = (float) ((2.0 * phase1 - 1.0) + (2.0 * phase2 - 1.0)) * 0.5f;
                lp += a * (s - lp);
                s = lp * 0.09f;
            }
            else if (kind == 1)
            {
                // sine plus a little second and third harmonic, so the bass is heard on small speakers
                const auto w = juce::MathConstants<double>::twoPi * phase1;
                s = (float) (std::sin (w) + 0.35 * std::sin (2.0 * w) + 0.15 * std::sin (3.0 * w)) * 0.28f;
            }
            else
                s = (float) (4.0 * std::abs (phase1 - 0.5) - 1.0) * 0.16f;

            phase1 += inc1; if (phase1 >= 1.0) phase1 -= 1.0;
            phase2 += inc2; if (phase2 >= 1.0) phase2 -= 1.0;

            const auto e = env.getNextSample() * level;
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, start + i, s * e);

            if (! env.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    std::atomic<float>& cutoff;
    std::atomic<float>& attack;
    int kind = 0;
    double freq = 440.0, phase1 = 0.0, phase2 = 0.0;
    float level = 0.0f, lp = 0.0f;
    juce::ADSR env;
};
} // namespace

// Runs the pitch tracker away from the audio thread.
class PitchThread : public juce::Thread
{
public:
    explicit PitchThread (ChordGenProcessor& p) : juce::Thread ("ChordGen pitch"), proc (p) {}
    void run() override
    {
        while (! threadShouldExit())
        {
            proc.drainAudio();
            wait (5);
        }
    }

private:
    ChordGenProcessor& proc;
};

#if CHORDGEN_FX
// Reads, transcribes and checks an audio file away from the message and audio threads.
class AudioImportJob : public juce::Thread
{
public:
    AudioImportJob (const juce::File& f, double b) : juce::Thread ("ChordGen audio import"), file (f), bpm (b) {}

    void run() override
    {
        result = audioimport::analyseFile (file, [this] (float p) { progress = p; return ! threadShouldExit(); });
        done = true;
    }

    const juce::File file;
    const double bpm;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> done { false };
    audioimport::Result result;
};
#else
class AudioImportJob : public juce::Thread
{
public:
    AudioImportJob() : juce::Thread ("unused") {}
    void run() override {}
};
#endif

ChordGenProcessor::ChordGenProcessor()
    : AudioProcessor (isFx ? BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                           : BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      tracker ([this] (const PitchTracker::DetectedNote& n) { onPitchNote (n); })
{
    for (int i = 0; i < 24; ++i)
        synth.addVoice (new ChordVoice (cutoff, attack));
    synth.addSound (new AnySound());

    captured.reserve (4096);
    activeNotes.reserve (512);
    auditions.reserve (32);
    pendingAuditions.reserve (32);
    pattern = std::make_shared<Pattern>();

    audioFifoData.resize ((size_t) audioFifo.getTotalSize());
    scratch.resize (8192);

    cg::loadPreset (state, "A minor pop");
    if (isFx) { state.source = 3; optThru = false; midiThru = false; }
    commit (true);
    startTimerHz (20);

    if (isFx)
    {
        pitchThread = std::make_unique<PitchThread> (*this);
        pitchThread->startThread();
    }
}

ChordGenProcessor::~ChordGenProcessor()
{
    stopTimer();
    cancelImport();
    if (pitchThread != nullptr)
        pitchThread->stopThread (1000);
}

void ChordGenProcessor::drainAudio()
{
    const double rate = trackerRate.load();
    if (std::abs (rate - appliedTrackerRate) > 1e-6) { appliedTrackerRate = rate; tracker.setSampleRate (rate); }

    const auto scope = audioFifo.read (audioFifo.getNumReady());
    if (scope.blockSize1 > 0) tracker.process (audioFifoData.data() + scope.startIndex1, scope.blockSize1);
    if (scope.blockSize2 > 0) tracker.process (audioFifoData.data() + scope.startIndex2, scope.blockSize2);
}

void ChordGenProcessor::onPitchNote (const PitchTracker::DetectedNote& n)
{
    if (! audioCapture.load() || ! captureOn.load()) return;
    const juce::SpinLock::ScopedLockType lock (captureLock);
    if (captured.size() < captured.capacity())
        captured.push_back ({ n.pitch, n.start, n.start + n.length, n.pass });
}

bool ChordGenProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    if (! isFx) return true;
    const auto in = layouts.getMainInputChannelSet();
    return in == out || in == juce::AudioChannelSet::mono();
}

void ChordGenProcessor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    synth.setCurrentPlaybackSampleRate (sr);
    activeNotes.clear();
    decim = juce::jmax (1, (int) std::lround (sr / 11025.0));
    trackerRate = sr / decim;
    acc = 0.0f;
    accN = 0;
}

std::shared_ptr<const Pattern> ChordGenProcessor::currentPattern() const
{
    const juce::SpinLock::ScopedLockType lock (patternLock);
    return pattern;
}

void ChordGenProcessor::allNotesOff (juce::MidiBuffer& out, int sample)
{
    for (const auto& n : activeNotes)
        out.addEvent (juce::MidiMessage::noteOff (n.channel, n.note), sample);
    activeNotes.clear();
}

void ChordGenProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numIn = isFx ? getTotalNumInputChannels() : 0;
    for (int ch = numIn; ch < buffer.getNumChannels(); ++ch) // the FX passes its input through
        buffer.clear (ch, 0, numSamples);

    double bpm = 120.0, ppq = 0.0;
    bool hostPlaying = false, bpmKnown = false;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) { bpm = *b; bpmKnown = true; }
            if (auto p = pos->getPpqPosition()) ppq = *p;
            hostPlaying = pos->getIsPlaying();
        }
    }

    const auto beatsPerSample = bpm / 60.0 / sampleRate;
    const auto pat = currentPattern();
    const auto loopBeats = pat->loopBeats > 0.0 ? pat->loopBeats : 16.0;

    // The clock: FL's transport, or our own Preview loop while FL is stopped.
    const bool preview = ! hostPlaying && previewOn.load();
    if (preview && ! wasPreview)
        previewBeat = 0.0;
    const bool running = hostPlaying || preview;
    const double clockBeat = hostPlaying ? ppq : previewBeat;

    // A new capture pass starts when the clock starts, jumps back (loop) or crosses a loop boundary.
    // unwrapped time keeps counting across FL's loop jumps, so note lengths stay exact
    if (running && wasRunning && clockBeat + 1e-6 < lastPpq)
        clockOffset += lastBlockEnd - clockBeat;
    const double absBeat = clockBeat + clockOffset;
    lastBlockEnd = clockBeat + numSamples * beatsPerSample;

    if (running)
    {
        const auto loopIndex = (long long) std::floor (clockBeat / loopBeats);
        if (! wasRunning || clockBeat + 1e-6 < lastPpq || loopIndex != lastLoopIndex)
        {
            if (! wasRunning)
                openNotes.fill ({});
            currentPass = ++pass;
        }
        lastLoopIndex = loopIndex;
    }
    lastPpq = clockBeat;

    // Audio / mic input: mono, decimated to ~11 kHz, handed to the pitch thread with its musical time.
    if (numIn > 0)
    {
        float peak = 0.0f;
        int n = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            float x = 0.0f;
            for (int ch = 0; ch < numIn; ++ch) x += buffer.getSample (ch, i);
            x /= (float) numIn;
            peak = std::max (peak, std::abs (x));
            acc += x;
            if (++accN >= decim && n < (int) scratch.size())
            {
                scratch[(size_t) n++] = { acc / (float) accN, clockBeat + i * beatsPerSample, absBeat + i * beatsPerSample, pass, running };
                acc = 0.0f;
                accN = 0;
            }
        }
        const auto scope = audioFifo.write (std::min (n, audioFifo.getFreeSpace()));
        for (int k = 0; k < scope.blockSize1; ++k) audioFifoData[(size_t) (scope.startIndex1 + k)] = scratch[(size_t) k];
        for (int k = 0; k < scope.blockSize2; ++k) audioFifoData[(size_t) (scope.startIndex2 + k)] = scratch[(size_t) (scope.blockSize1 + k)];
        inputPeak = std::max (peak, inputPeak.load() * 0.85f);
    }

    juce::MidiBuffer out;

    // Capture the melody coming from FL's piano roll, and pass it through on the melody channel.
    for (const auto meta : midi)
    {
        auto m = meta.getMessage();
        const auto t = ppq + meta.samplePosition * beatsPerSample;
        const auto tAbs = absBeat + meta.samplePosition * beatsPerSample;

        if (hostPlaying && captureOn.load() && m.isNoteOnOrOff())
        {
            const auto n = m.getNoteNumber();
            auto& open = openNotes[(size_t) n];
            if (m.isNoteOn())
                open = { t, tAbs, pass };
            else if (open.start >= 0.0)
            {
                const auto length = juce::jmax (0.0, tAbs - open.abs);
                const juce::SpinLock::ScopedTryLockType lock (captureLock);
                if (lock.isLocked() && captured.size() < captured.capacity())
                    captured.push_back ({ n, open.start, open.start + length, open.pass });
                open = {};
            }
        }

        if (midiThru.load() && m.isNoteOnOrOff())
        {
            m.setChannel (melodyChannel);
            out.addEvent (m, meta.samplePosition);
        }
    }

    // Play the chord pattern: in sync with FL when it plays, or on our own clock for Preview.
    const bool active = (hostPlaying && followHost.load()) || preview;
    int clickAt = -1;
    bool clickDown = false;

    if (! active && wasActive)
        allNotesOff (out, 0);

    if (active)
    {
        const auto startBeat = hostPlaying ? ppq : previewBeat;
        const auto endBeat = startBeat + numSamples * beatsPerSample;
        const auto toSample = [&] (double beat)
        {
            return juce::jlimit (0, numSamples - 1, (int) ((beat - startBeat) / beatsPerSample));
        };

        // a jump in the host position (looping, seeking) ends whatever is sounding
        if (hostPlaying && ! activeNotes.empty() && ppq + 1e-6 < lastPpqPlayed)
            allNotesOff (out, 0);

        for (auto it = activeNotes.begin(); it != activeNotes.end();)
        {
            if (it->offBeat < endBeat)
            {
                out.addEvent (juce::MidiMessage::noteOff (it->channel, it->note), toSample (it->offBeat));
                it = activeNotes.erase (it);
            }
            else
                ++it;
        }

        const auto firstLoop = (long long) std::floor (startBeat / loopBeats);
        const auto lastLoop = (long long) std::floor (endBeat / loopBeats);

        for (auto li = firstLoop; li <= lastLoop; ++li)
        {
            for (const auto& e : pat->events)
            {
                // the melody only sounds in Preview; while FL plays, the real melody comes through
                if (e.channel == melodyChannel && hostPlaying)
                    continue;

                const auto tb = (double) li * loopBeats + e.beat;
                if (tb < startBeat || tb >= endBeat)
                    continue;

                const auto s = toSample (tb);
                for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it)
                {
                    if (it->note == e.note && it->channel == e.channel)
                    {
                        out.addEvent (juce::MidiMessage::noteOff (e.channel, e.note), s);
                        activeNotes.erase (it);
                        break;
                    }
                }

                out.addEvent (juce::MidiMessage::noteOn (e.channel, e.note, e.velocity), s);
                if (activeNotes.size() < activeNotes.capacity())
                    activeNotes.push_back ({ e.note, e.channel, tb + e.length });
            }
        }

        // metronome while Preview runs, so you can sing or play in time
        if (preview && metronomeOn.load())
        {
            const double b = std::ceil (startBeat - 1e-9);
            if (b < endBeat)
            {
                clickAt = toSample (b);
                clickDown = std::fmod (b, (double) juce::jmax (1, beatsPerBar.load())) < 1e-6;
            }
        }

        infoLoopPos = std::fmod (startBeat, loopBeats);
        if (preview)
            previewBeat = endBeat;
        lastPpqPlayed = ppq;
    }

    // Single notes clicked in the editor (keyboard, piano roll).
    {
        const juce::SpinLock::ScopedTryLockType lock (auditionLock);
        if (lock.isLocked())
        {
            for (auto n : pendingAuditions)
            {
                out.addEvent (juce::MidiMessage::noteOn (melodyChannel, n, 0.8f), 0);
                if (auditions.size() < auditions.capacity())
                    auditions.push_back ({ n, (int) (sampleRate * 0.35) });
            }
            pendingAuditions.clear();
        }
    }
    for (auto it = auditions.begin(); it != auditions.end();)
    {
        it->samplesLeft -= numSamples;
        if (it->samplesLeft <= 0)
        {
            out.addEvent (juce::MidiMessage::noteOff (melodyChannel, it->note), numSamples - 1);
            it = auditions.erase (it);
        }
        else
            ++it;
    }

    if (internalSound.load())
        synth.renderNextBlock (buffer, out, 0, numSamples);
    else
        synth.allNotesOff (0, false);

    if (clickAt >= 0)
    {
        clickLeft = (int) (sampleRate * 0.04);
        clickPhase = 0.0;
        clickFreq = clickDown ? 1800.0 : 1200.0;
    }
    for (int i = juce::jmax (0, clickAt); i < numSamples && clickLeft > 0; ++i, --clickLeft)
    {
        const auto env = (float) clickLeft / (float) (sampleRate * 0.04);
        const auto v = (float) std::sin (clickPhase) * env * env * 0.25f;
        clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sampleRate;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.addSample (ch, i, v);
    }

    midi.swapWith (out);

    wasHostPlaying = hostPlaying;
    wasActive = active;
    wasPreview = preview;
    wasRunning = running;
    infoRunning = running;

    infoBpm = bpm;
    infoBpmKnown = bpmKnown;
    infoHostPlaying = hostPlaying;
    infoActive = active;
}

//==============================================================================
void ChordGenProcessor::applyPattern()
{
    auto p = std::make_shared<Pattern>();
    p->loopBeats = state.bars * cg::timeSigs()[(size_t) state.ts].beats;
    for (const auto& e : cg::buildEvents (state, analysis, true))
        p->events.push_back ({ e.t, e.d, e.midi, e.type + 1, e.v });

    cutoff = cg::styleInfo (state.style).cutoff;
    attack = state.style == cg::Style::trap ? 0.09f : 0.012f;
    beatsPerBar = cg::timeSigs()[(size_t) state.ts].beats;
    audioCapture = state.source == 3 || state.source == 4;

    const juce::SpinLock::ScopedLockType lock (patternLock);
    pattern = (optLive && livePattern != nullptr) ? livePattern : std::move (p);
}

void ChordGenProcessor::commit (bool resuggest)
{
    analysis = cg::analyse (state, resuggest);
    applyPattern();
    sendChangeMessage();
}

void ChordGenProcessor::pushHistory()
{
    undoStack.push_back (state);
    if (undoStack.size() > 150) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void ChordGenProcessor::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back (state);
    state = undoStack.back();
    undoStack.pop_back();
    commit (false);
}

void ChordGenProcessor::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back (state);
    state = redoStack.back();
    redoStack.pop_back();
    commit (false);
}

void ChordGenProcessor::regenerate()
{
    pushHistory();
    const auto before = state.prog;
    // try new seeds until an unlocked chord actually changes
    for (int tries = 0; tries < 12; ++tries)
    {
        state.seed = (state.seed * 7 + 13) % 9973 + 1;
        state.sugIdx = 0;
        auto A = cg::analyse (state, true);
        for (size_t k = 0; k < A.sugs.size(); ++k)
        {
            std::vector<int> codes;
            for (auto j : A.sugs[k]) codes.push_back (A.chords[(size_t) j].code());
            if (codes != before) { state.prog = codes; state.sugIdx = (int) k; break; }
        }
        if (state.prog != before) break;
    }
    if (state.prog == before) showToast ("Every chord is locked or no other option fits.");
    commit (false);
}

void ChordGenProcessor::saveFavorite()
{
    juce::String title = cg::keyTitle (analysis.key) + ":";
    for (int i = 0; i < analysis.nSeg; ++i) title += " " + cg::chordName (analysis.chordAt (i), analysis, state.complexity);
    favorites.insert (favorites.begin(), { title, state });
    if (favorites.size() > 12) favorites.pop_back();
    showToast ("Saved " + title);
}

void ChordGenProcessor::restoreFavorite (int index)
{
    if (index < 0 || index >= (int) favorites.size()) return;
    pushHistory();
    state = favorites[(size_t) index].second;
    commit (false);
    showToast ("Restored " + favorites[(size_t) index].first);
}

void ChordGenProcessor::clearFavorites()
{
    favorites.clear();
    sendChangeMessage();
}

juce::StringArray ChordGenProcessor::favoriteNames() const
{
    juce::StringArray names;
    for (const auto& f : favorites) names.add (f.first);
    return names;
}

void ChordGenProcessor::setOption (const juce::String& name, bool value)
{
    if (name == "preview")       { optPreview = value; previewOn = value; }
    else if (name == "follow")   { optFollow = value; followHost = value; }
    else if (name == "internal") { optInternal = value; internalSound = value; }
    else if (name == "thru")     { optThru = value; midiThru = value; }
    else if (name == "capture")  { optCapture = value; captureOn = value; if (! value) pending.clear(); }
    else if (name == "metronome") { optMetronome = value; metronomeOn = value; }
    else if (name == "live")
    {
        optLive = value;
        livePattern = nullptr;
        liveBarKey = -1;
        liveChord = {};
        applyPattern();
    }
    sendChangeMessage();
}

void ChordGenProcessor::auditionNote (int note)
{
    const juce::SpinLock::ScopedLockType lock (auditionLock);
    if (pendingAuditions.size() < pendingAuditions.capacity())
        pendingAuditions.push_back (juce::jlimit (0, 127, note));
}

std::vector<CapturedNote> ChordGenProcessor::popCapturedNotes()
{
    std::vector<CapturedNote> result;
    const juce::SpinLock::ScopedLockType lock (captureLock);
    result.swap (captured);
    captured.reserve (4096);
    return result;
}

// Capture mode: collect the notes of one pass of the FL pattern, then take them as the melody.
void ChordGenProcessor::timerCallback()
{
#if CHORDGEN_FX
    if (importJob != nullptr && importJob->done.load())
        finishImport();
#endif

    const auto notes = popCapturedNotes();
    if (! notes.empty())
        onCapturedNotes (notes);

    const bool playing = infoHostPlaying.load();
    if (hostWasPlaying && ! playing)
        commitPending();

    // Live mode: when a bar ends, the chord that fits it plays in the next bar
    if (optLive && infoRunning.load())
    {
        const int bpb = cg::timeSigs()[(size_t) state.ts].beats;
        const int bar = (int) std::floor (infoLoopPos.load() / bpb);
        const int key = currentPass.load() * 1000 + bar;
        if (key != liveBarKey)
        {
            if (liveBarKey >= 0) updateLiveChord (liveBar);
            liveBarKey = key;
            liveBar = bar;
        }
    }
    else
        liveBarKey = -1;

    // take the melody shortly after a pass ends, so the next pass already plays matching chords
    const int cur = currentPass.load();
    if (cur != passSeen) { passSeen = cur; passTicks = 0; }
    else if (++passTicks == 3 && playing && pendingPass >= 0 && pendingPass < cur)
    {
        commitPending();
        pendingPass = cur;
    }
    if (playing && optPreview)
        setOption ("preview", false);
    hostWasPlaying = playing;
}

void ChordGenProcessor::onCapturedNotes (const std::vector<CapturedNote>& notes)
{
    if (! optCapture) return;
    const int total = state.bars * cg::timeSigs()[(size_t) state.ts].steps;
    for (const auto& c : notes)
    {
        if (c.pass < pendingPass) continue; // late note of a pass that was already taken
        if (c.pass != pendingPass) { commitPending(); pendingPass = c.pass; }
        const int s = ((int) std::lround (c.start * cg::stepsPerBeat) % total + total) % total;
        const int d = juce::jlimit (1, total - s, (int) std::lround ((c.end - c.start) * cg::stepsPerBeat));
        const bool dup = std::any_of (pending.begin(), pending.end(), [&] (const cg::Note& n) { return n.p == c.pitch && n.s == s; });
        if (! dup) pending.push_back ({ c.pitch, s, d });
    }
    sendChangeMessage();
}

void ChordGenProcessor::commitPending()
{
    if (pending.empty()) return;
    auto key = [] (std::vector<cg::Note> v)
    {
        std::sort (v.begin(), v.end(), [] (const cg::Note& a, const cg::Note& b) { return a.s != b.s ? a.s < b.s : a.p < b.p; });
        return v;
    };
    auto next = std::move (pending);
    pending.clear();
    if (key (next) == key (state.notes)) { sendChangeMessage(); return; }

    pushHistory();
    state.notes = next;
    state.source = 0;
    state.preset = {};
    state.locks.clear();
    state.selected = 0;
    commit (true);
    showToast ("Captured " + juce::String ((int) next.size()) + " notes from FL");
}

void ChordGenProcessor::updateLiveChord (int bar)
{
    const int steps = cg::timeSigs()[(size_t) state.ts].steps;
    cg::State t = state;
    t.notes.clear();
    for (const auto& n : pending)
        if (n.s >= bar * steps && n.s < (bar + 1) * steps)
            t.notes.push_back ({ n.p, n.s - bar * steps, std::min (n.d, (bar + 1) * steps - n.s) });
    if (t.notes.empty()) return; // nothing sung or played: keep the chord

    // keep the key steady: use the lock, or the key of everything heard so far
    if (! t.keyLocked)
    {
        cg::State all = state;
        all.notes.insert (all.notes.end(), pending.begin(), pending.end());
        all.locks.clear();
        t.keyLock = cg::analyse (all, true).key;
        t.keyLocked = true;
    }
    t.bars = 1; t.rhythm = 1.0; t.locks.clear(); t.prog.clear(); t.sugIdx = 0; t.seed = 0; t.playMelody = false;
    const auto A = cg::analyse (t, true);

    auto p = std::make_shared<Pattern>();
    p->loopBeats = cg::timeSigs()[(size_t) state.ts].beats;
    for (const auto& e : cg::buildEvents (t, A, true))
        if (e.type != 2) p->events.push_back ({ e.t, e.d, e.midi, e.type + 1, e.v });
    livePattern = p;
    liveChord = cg::chordName (A.chordAt (0), A, t.complexity);
    applyPattern();
    sendChangeMessage();
}

ChordGenProcessor::HostInfo ChordGenProcessor::getHostInfo() const
{
    HostInfo h;
    h.bpm = infoBpm.load();
    h.loopPos = infoLoopPos.load();
    h.hostPlaying = infoHostPlaying.load();
    h.active = infoActive.load();
    h.bpmKnown = infoBpmKnown.load();
    return h;
}

//==============================================================================
void ChordGenProcessor::importAudio (const juce::File& file, double bpm)
{
#if CHORDGEN_FX
    cancelImport();
    notice = {};
    importJob = std::make_unique<AudioImportJob> (file, bpm);
    importJob->startThread (juce::Thread::Priority::low);
    sendChangeMessage();
#else
    juce::ignoreUnused (file, bpm);
#endif
}

void ChordGenProcessor::cancelImport()
{
    if (importJob == nullptr) return;
    importJob->stopThread (10000);
    importJob = nullptr;
    sendChangeMessage();
}

ChordGenProcessor::ImportStatus ChordGenProcessor::importStatus() const
{
    ImportStatus s;
#if CHORDGEN_FX
    if (importJob != nullptr)
    {
        s.running = true;
        s.progress = importJob->progress.load();
        s.file = importJob->file.getFileName();
    }
#endif
    return s;
}

void ChordGenProcessor::finishImport()
{
#if CHORDGEN_FX
    const auto job = std::move (importJob);
    job->stopThread (1000);
    const auto& r = job->result;

    auto reject = [this] (audioimport::Verdict v)
    {
        const auto m = audioimport::messageFor (v);
        notice = { m.title, m.body, m.tip };
        sendChangeMessage();
    };
    if (r.verdict != audioimport::Verdict::ok) { reject (r.verdict); return; }
    const auto placed = audioimport::place (r.frames, job->bpm, state.ts);
    if (placed.beats.empty()) { reject (audioimport::Verdict::noNotes); return; }

    pushHistory();
    state.notes.clear();
    state.audio = placed.beats;
    state.bars = placed.bars;
    state.source = 5;
    state.preset = {};
    state.keyLocked = false;
    state.locks.clear();
    state.selected = 0;
    commit (true);

    juce::String text = "Listened to " + job->file.getFileName();
    if (placed.firstBar > 0 || placed.totalBars > placed.firstBar + placed.bars)
        text << " (bars " << placed.firstBar + 1 << "-" << placed.firstBar + placed.bars << " of " << placed.totalBars << ")";
    showToast (text);
#endif
}

void ChordGenProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 2);
    root->setProperty ("state", cg::toVar (state));
    juce::Array<juce::var> favs;
    for (const auto& [name, st] : favorites)
    {
        auto* f = new juce::DynamicObject();
        f->setProperty ("name", name);
        f->setProperty ("state", cg::toVar (st));
        favs.add (juce::var (f));
    }
    root->setProperty ("favorites", favs);
    root->setProperty ("capture", optCapture);
    root->setProperty ("follow", optFollow);
    root->setProperty ("internal", optInternal);
    root->setProperty ("thru", optThru);
    root->setProperty ("accent", accent);
    root->setProperty ("live", optLive);
    root->setProperty ("metronome", optMetronome);

    juce::ValueTree vt ("ChordGen");
    vt.setProperty ("json", juce::JSON::toString (juce::var (root), true), nullptr);
    if (auto xml = vt.createXml())
        copyXmlToBinary (*xml, dest);
}

void ChordGenProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    const auto vt = juce::ValueTree::fromXml (*xml);
    if (! vt.hasType ("ChordGen") || ! vt.hasProperty ("json")) return;

    const auto root = juce::JSON::parse (vt.getProperty ("json").toString());
    if (! root.isObject()) return;

    state = cg::fromVar (root["state"]);
    favorites.clear();
    if (auto* favs = root["favorites"].getArray())
        for (const auto& f : *favs)
            favorites.push_back ({ f["name"].toString(), cg::fromVar (f["state"]) });
    setOption ("capture", root.getProperty ("capture", true));
    setOption ("follow", root.getProperty ("follow", true));
    setOption ("internal", root.getProperty ("internal", true));
    setOption ("thru", root.getProperty ("thru", true));
    accent = juce::jlimit (0, 3, (int) root.getProperty ("accent", 0));
    setOption ("metronome", root.getProperty ("metronome", true));
    setOption ("live", root.getProperty ("live", false));
    undoStack.clear();
    redoStack.clear();
    commit (false);
}

juce::AudioProcessorEditor* ChordGenProcessor::createEditor()
{
    return new ChordGenEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChordGenProcessor();
}
