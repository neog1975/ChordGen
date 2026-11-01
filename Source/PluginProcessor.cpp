#include "PluginProcessor.h"
#include "PluginEditor.h"

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

ChordGenProcessor::ChordGenProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (int i = 0; i < 24; ++i)
        synth.addVoice (new ChordVoice (cutoff, attack));
    synth.addSound (new AnySound());

    captured.reserve (4096);
    activeNotes.reserve (512);
    auditions.reserve (32);
    pendingAuditions.reserve (32);
    pattern = std::make_shared<Pattern>();

    cg::loadPreset (state, "A minor pop");
    commit (true);
    startTimerHz (20);
}

ChordGenProcessor::~ChordGenProcessor()
{
    stopTimer();
}

bool ChordGenProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ChordGenProcessor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    synth.setCurrentPlaybackSampleRate (sr);
    activeNotes.clear();
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
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

    double bpm = 120.0, ppq = 0.0;
    bool hostPlaying = false;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto p = pos->getPpqPosition()) ppq = *p;
            hostPlaying = pos->getIsPlaying();
        }
    }

    const auto beatsPerSample = bpm / 60.0 / sampleRate;
    const auto pat = currentPattern();
    const auto loopBeats = pat->loopBeats > 0.0 ? pat->loopBeats : 16.0;

    // A new capture pass starts when FL starts playing, jumps back (loop) or crosses a loop boundary.
    if (hostPlaying)
    {
        const auto loopIndex = (long long) std::floor (ppq / loopBeats);
        if (! wasHostPlaying || ppq + 1e-6 < lastPpq || loopIndex != lastLoopIndex)
        {
            if (! wasHostPlaying)
                openNotes.fill ({});
            currentPass = ++pass;
        }
        lastLoopIndex = loopIndex;
    }
    lastPpq = ppq;

    juce::MidiBuffer out;

    // Capture the melody coming from FL's piano roll, and pass it through on the melody channel.
    for (const auto meta : midi)
    {
        auto m = meta.getMessage();
        const auto t = ppq + meta.samplePosition * beatsPerSample;

        if (hostPlaying && captureOn.load() && m.isNoteOnOrOff())
        {
            const auto n = m.getNoteNumber();
            auto& open = openNotes[(size_t) n];
            if (m.isNoteOn())
                open = { t, pass };
            else if (open.start >= 0.0)
            {
                // a note-off for a note that ends at the loop end arrives after FL jumps back
                auto length = t - open.start;
                if (length <= 0.0) length += loopBeats - std::fmod (open.start, loopBeats);
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
    const bool preview = ! hostPlaying && previewOn.load();
    const bool active = (hostPlaying && followHost.load()) || preview;

    if (preview && ! wasPreview)
        previewBeat = 0.0;

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

    midi.swapWith (out);

    wasHostPlaying = hostPlaying;
    wasActive = active;
    wasPreview = preview;

    infoBpm = bpm;
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

    const juce::SpinLock::ScopedLockType lock (patternLock);
    pattern = std::move (p);
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
    const auto notes = popCapturedNotes();
    if (! notes.empty())
        onCapturedNotes (notes);

    const bool playing = infoHostPlaying.load();
    if (hostWasPlaying && ! playing)
        commitPending();

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
        const int s = ((int) std::lround (c.start * 2) % total + total) % total;
        const int d = juce::jlimit (1, total - s, (int) std::lround ((c.end - c.start) * 2));
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

ChordGenProcessor::HostInfo ChordGenProcessor::getHostInfo() const
{
    HostInfo h;
    h.bpm = infoBpm.load();
    h.loopPos = infoLoopPos.load();
    h.hostPlaying = infoHostPlaying.load();
    h.active = infoActive.load();
    return h;
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
