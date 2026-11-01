// Runs every .mid in a folder through ChordGen's import and engine and reports what changed.
// Usage: MidiBatch <folder> [details]
#include "../Source/Engine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <iostream>

namespace
{
struct Raw { int p; double on, off; int track; };

std::vector<Raw> readAll (const juce::File& f, int& tpq)
{
    std::vector<Raw> out;
    juce::FileInputStream in (f);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) return out;
    tpq = mf.getTimeFormat();
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isNoteOn() && m.getChannel() != 10)
                out.push_back ({ m.getNoteNumber(), m.getTimeStamp(), seq.getTimeOfMatchingKeyUp (i), t });
        }
    }
    return out;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) { std::cout << "usage: MidiBatch <folder> [details]\n"; return 1; }
    const bool details = argc > 2;
    const juce::File dir (juce::String::fromUTF8 (argv[1]));
    auto files = dir.findChildFiles (juce::File::findFiles, true, "*.mid;*.midi");
    files.sort();

    int total = 0, perfect = 0, totalNotes = 0, exactNotes = 0, droppedLong = 0, offGrid = 0, multiTrackLoss = 0, errors = 0;
    std::map<juce::String, int> keys;

    for (const auto& f : files)
    {
        int tpq = 480;
        const auto raw = readAll (f, tpq);
        const auto r = cg::importMidi (f);
        ++total;
        if (r.error.isNotEmpty() || tpq <= 0) { ++errors; std::cout << f.getFileName() << ": " << r.error << "\n"; continue; }

        // which raw notes came through exactly (same pitch, start and length on the 1/24-beat grid)?
        const double step = tpq / (double) cg::stepsPerBeat;
        const int barSteps = cg::timeSigs()[(size_t) r.ts].steps;
        int exact = 0, beyond = 0, otherTracks = 0, grid = 0;
        std::map<int, int> perTrack;
        for (const auto& n : raw) ++perTrack[n.track];
        int mainTrack = 0, mainCount = -1;
        for (auto [t, c] : perTrack) if (c > mainCount) { mainTrack = t; mainCount = c; }

        for (const auto& n : raw)
        {
            if (n.track != mainTrack) { ++otherTracks; continue; }
            const double s = n.on / step, d = (n.off - n.on) / step;
            if (std::abs (s - std::round (s)) > 0.01 || std::abs (d - std::round (d)) > 0.01) ++grid;
            const cg::Note want { n.p, (int) std::lround (s), std::max (1, (int) std::lround (d)) };
            if (want.s >= r.bars * barSteps) { ++beyond; continue; }
            auto clipped = want;
            clipped.d = std::min (want.d, r.bars * barSteps - want.s);
            exact += std::find (r.notes.begin(), r.notes.end(), clipped) != r.notes.end();
        }
        const int considered = mainCount - beyond;
        totalNotes += considered;
        exactNotes += exact;
        droppedLong += beyond;
        offGrid += grid;
        multiTrackLoss += otherTracks;
        if (exact == considered && beyond == 0) ++perfect;

        cg::State st;
        st.notes = r.notes; st.bars = r.bars; st.ts = r.ts;
        auto A = cg::analyse (st, true);
        const auto key = cg::keyTitle (A.key);
        ++keys[key];

        if (details || exact != considered || beyond > 0 || otherTracks > 0)
        {
            juce::String chords;
            for (int i = 0; i < A.nSeg; ++i) chords << cg::chordName (A.chordAt (i), A, 1, true) << " ";
            std::cout << f.getFileNameWithoutExtension().upToFirstOccurrenceOf (" ", false, false)
                      << ": notes " << mainCount << " exact " << exact << "/" << considered
                      << (beyond ? " | " + juce::String (beyond) + " past 16 bars" : juce::String())
                      << (otherTracks ? " | " + juce::String (otherTracks) + " in other tracks" : juce::String())
                      << (grid ? " | " + juce::String (grid) + " off 1/24 grid" : juce::String())
                      << " | " << r.bars << " bars " << cg::timeSigs()[(size_t) r.ts].name << " | " << key << " | " << chords << "\n";
        }
    }

    std::cout << "\nfiles " << total << ", read errors " << errors << ", files with every note exact " << perfect << "\n"
              << "notes exact " << exactNotes << "/" << totalNotes << ", cut after 16 bars " << droppedLong
              << ", in other tracks " << multiTrackLoss << ", off the 1/24 grid " << offGrid << "\n";
    std::cout << "keys:";
    for (auto& [k, c] : keys) std::cout << " " << k << " (" << c << ")";
    std::cout << "\n";
    return 0;
}
