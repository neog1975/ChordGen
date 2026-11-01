// Simulates FL Studio playing a melody into ChordGen and prints what the plugin captured
// and which chords it suggests. Run: build/ChordGenTests_artefacts/Release/ChordGenTests
#include "../Source/PluginProcessor.h"

namespace
{
struct TestHead : juce::AudioPlayHead
{
    PositionInfo info;
    juce::Optional<PositionInfo> getPosition() const override { return info; }
};

struct MelodyNote { int pitch; double start, length; }; // beats

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

void playLoop (ChordGenProcessor& p, TestHead& head, const std::vector<MelodyNote>& mel, double loopBeats,
               double fromBeat, double toBeat, double bpm)
{
    const double sr = 44100.0;
    const int block = 512;
    const double beatsPerBlock = block * bpm / 60.0 / sr;
    juce::AudioBuffer<float> buf (2, block);

    for (double b = fromBeat; b < toBeat; b += beatsPerBlock)
    {
        const double loopPos = std::fmod (b, loopBeats);
        head.info.setIsPlaying (true);
        head.info.setPpqPosition (loopPos); // FL pattern mode: position wraps at the loop end
        head.info.setBpm (bpm);

        juce::MidiBuffer midi;
        for (const auto& n : mel)
        {
            for (const auto [t, on] : { std::pair { n.start, true }, std::pair { n.start + n.length, false } })
            {
                const double tt = std::fmod (t, loopBeats); // FL sends a note-off at the loop end after jumping back
                if ((tt >= loopPos && tt < loopPos + beatsPerBlock) || (tt + loopBeats >= loopPos && tt + loopBeats < loopPos + beatsPerBlock) || (! on && tt < 1e-9 && loopPos < beatsPerBlock))
                {
                    const int s = juce::jlimit (0, block - 1, (int) ((tt - loopPos) / beatsPerBlock * block));
                    midi.addEvent (on ? juce::MidiMessage::noteOn (1, n.pitch, 0.8f) : juce::MidiMessage::noteOff (1, n.pitch), s);
                }
            }
        }
        p.processBlock (buf, midi);
        pump (1);
    }
}

void report (ChordGenProcessor& p, const char* when)
{
    const auto& A = p.analysis;
    juce::String chords;
    for (int i = 0; i < A.nSeg; ++i) chords << cg::chordName (A.chordAt (i), A, p.state.complexity) << " ";
    std::cout << when << ": " << p.state.notes.size() << " notes, pending " << p.pending.size()
              << ", key " << cg::keyTitle (A.key) << ", chords " << chords << "\n";
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    ChordGenProcessor p;
    TestHead head;
    p.setPlayHead (&head);
    p.prepareToPlay (44100.0, 512);
    report (p, "start (example melody)");

    // A 4-bar melody in G major with 16ths, dotted notes and a low second voice
    const std::vector<MelodyNote> mel {
        { 67, 0, 0.25 }, { 67, 0.5, 0.25 }, { 71, 0.75, 0.25 }, { 74, 1, 0.75 }, { 74, 1.75, 0.25 }, { 72, 2.5, 1.5 },
        { 71, 4, 0.25 }, { 67, 4.25, 0.25 }, { 64, 4.5, 0.5 }, { 62, 5.25, 0.25 }, { 64, 6, 2 },
        { 72, 8, 0.25 }, { 76, 8.75, 0.25 }, { 79, 9, 1.5 }, { 48, 10.5, 0.125 }, { 48, 11, 0.125 },
        { 78, 12, 0.5 }, { 74, 12.75, 0.25 }, { 69, 13, 3 }, { 50, 13.5, 0.125 },
    };
    const double loop = 16.0;

    playLoop (p, head, mel, loop, 0.0, loop, 120.0);
    pump (100);
    report (p, "after pass 1");
    playLoop (p, head, mel, loop, loop, loop + 1.0, 120.0);
    pump (300);
    report (p, "1 beat into pass 2");
    playLoop (p, head, mel, loop, loop + 1.0, 2 * loop + 0.5, 120.0);
    pump (100);
    report (p, "after pass 2");

    // stop the transport
    head.info.setIsPlaying (false);
    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < 10; ++i) { juce::MidiBuffer m; p.processBlock (buf, m); pump (5); }
    pump (200);
    report (p, "after stop");

    // every note must come back at exactly the position and length FL sent
    int exact = 0;
    for (const auto& m : mel)
    {
        const cg::Note want { m.pitch, (int) std::lround (m.start * cg::stepsPerBeat), (int) std::lround (m.length * cg::stepsPerBeat) };
        const bool ok = std::find (p.state.notes.begin(), p.state.notes.end(), want) != p.state.notes.end();
        exact += ok;
        if (! ok) std::cout << "  timing differs: " << m.pitch << " @" << m.start << " len " << m.length << "\n";
    }
    for (const auto& n : p.state.notes) if (n.p == 69) std::cout << "  got 69 @" << n.s << " len " << n.d << "\n";
    std::cout << "capture timing: " << exact << "/" << mel.size() << " exact, " << p.state.notes.size() << " notes\n";

    // the same melody as a .mid file through Import
    {
        juce::MidiMessageSequence seq;
        for (const auto& m : mel)
        {
            seq.addEvent (juce::MidiMessage::noteOn (1, m.pitch, 0.8f), m.start * 960);
            seq.addEvent (juce::MidiMessage::noteOff (1, m.pitch), (m.start + m.length) * 960);
        }
        seq.sort();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        mf.addTrack (seq);
        const auto f = juce::File::createTempFile (".mid");
        { juce::FileOutputStream out (f); mf.writeTo (out); }
        const auto r = cg::importMidi (f);
        f.deleteFile();
        int ok = 0;
        for (const auto& m : mel)
        {
            const cg::Note want { m.pitch, (int) std::lround (m.start * cg::stepsPerBeat), (int) std::lround (m.length * cg::stepsPerBeat) };
            ok += std::find (r.notes.begin(), r.notes.end(), want) != r.notes.end();
        }
        std::cout << "import timing: " << ok << "/" << mel.size() << " exact" << (r.error.isNotEmpty() ? " error " + r.error : juce::String()) << "\n";
    }

    for (int cx = 0; cx < 4; ++cx)
    {
        p.state.complexity = cx;
        p.commit (false);
        std::cout << "complexity " << cx << ":";
        for (int i = 0; i < p.analysis.nSeg; ++i)
        {
            std::cout << "  " << cg::chordName (p.analysis.chordAt (i), p.analysis, cx) << " [";
            for (auto m : p.analysis.voicing[(size_t) i].notes) std::cout << " " << m;
            std::cout << " ]";
        }
        std::cout << "\n";
    }
    return 0;
}
