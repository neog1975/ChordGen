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

    // A 4-bar melody in G major over G - Em - C - D
    const std::vector<MelodyNote> mel {
        { 67, 0, 1 }, { 71, 1, 1 }, { 74, 2, 2 },
        { 71, 4, 1 }, { 67, 5, 1 }, { 64, 6, 2 },
        { 72, 8, 1 }, { 76, 9, 1 }, { 79, 10, 2 },
        { 78, 12, 1 }, { 74, 13, 1 }, { 69, 14, 2 },
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

    std::cout << "captured:";
    for (const auto& n : p.state.notes) std::cout << " " << n.p << "@" << n.s << "/" << n.d;
    std::cout << "\n";

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
