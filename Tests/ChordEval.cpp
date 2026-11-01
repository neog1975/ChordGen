// Measures how well ChordGen finds the chords a producer wrote.
// For every chord/pad/piano MIDI: the written chord per bar is the answer; ChordGen only gets the
// top voice as its melody. Usage: ChordEval <folder> [<folder> ...] [--details]
#include "../Source/Engine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <iostream>

namespace
{
struct Raw { int p; double on, off; int part = 0; }; // beats; part = track and channel

std::vector<Raw> readNotes (const juce::File& f)
{
    std::vector<Raw> out;
    juce::FileInputStream in (f);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in) || mf.getTimeFormat() <= 0) return out;
    const double tpq = mf.getTimeFormat();
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isNoteOn() && m.getChannel() != 10)
                out.push_back ({ m.getNoteNumber(), m.getTimeStamp() / tpq, seq.getTimeOfMatchingKeyUp (i) / tpq, t * 16 + m.getChannel() });
        }
    }
    return out;
}

struct Triad { int root = -1; bool minor = false; };

// The chord a bar spells out: the major or minor triad that covers most of the sounding notes,
// with a bonus for the lowest note as root.
Triad truthChord (const std::vector<Raw>& notes, double a, double b)
{
    std::array<double, 12> w {};
    int lowest = 128;
    double total = 0;
    for (const auto& n : notes)
    {
        const double ov = std::min (b, n.off) - std::max (a, n.on);
        if (ov <= 0) continue;
        w[(size_t) (n.p % 12)] += ov;
        total += ov;
        lowest = std::min (lowest, n.p);
    }
    if (total <= 0) return {};
    Triad best;
    double bestScore = -1e9;
    for (int r = 0; r < 12; ++r)
        for (const bool minor : { false, true })
        {
            const int t = minor ? 3 : 4;
            double s = 0;
            for (int pc = 0; pc < 12; ++pc)
            {
                const int rel = (pc - r + 12) % 12;
                s += (rel == 0 || rel == t || rel == 7) ? w[(size_t) pc] : -0.5 * w[(size_t) pc];
            }
            s += 0.3 * w[(size_t) r] + (lowest % 12 == r ? 0.2 * total : 0.0);
            if (s > bestScore) { bestScore = s; best = { r, minor }; }
        }
    return best;
}

// Top voice: a note counts as melody if no higher note is sounding when it starts.
std::vector<Raw> topVoice (const std::vector<Raw>& notes)
{
    std::vector<Raw> out;
    for (const auto& n : notes)
    {
        bool covered = false;
        for (const auto& o : notes)
            if (o.p > n.p && o.on <= n.on + 1e-6 && o.off > n.on + 1e-6) { covered = true; break; }
        if (! covered) out.push_back (n);
    }
    return out;
}

// Full arrangements: the lead (a mostly one-note-at-a-time part, highest on average) is the melody,
// everything else that plays below it is the harmony the producer wrote.
bool splitSong (const std::vector<Raw>& notes, std::vector<Raw>& melody, std::vector<Raw>& harmony)
{
    std::map<int, std::vector<Raw>> parts;
    for (const auto& n : notes) parts[n.part].push_back (n);
    if (parts.size() < 2) return false;
    int best = -1;
    double bestPitch = -1;
    for (auto& [id, v] : parts)
    {
        if (v.size() < 24) continue;
        int overlapping = 0;
        double pitch = 0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            pitch += v[i].p;
            for (size_t j = 0; j < v.size(); ++j)
                if (i != j && std::abs (v[j].on - v[i].on) < 0.05) { ++overlapping; break; }
        }
        pitch /= (double) v.size();
        if (overlapping < (int) v.size() / 5 && pitch > bestPitch) { bestPitch = pitch; best = id; }
    }
    if (best < 0) return false;
    melody = parts[best];
    harmony.clear();
    int polyNotes = 0;
    for (auto& [id, v] : parts)
        if (id != best)
            for (const auto& n : v) if (n.p < bestPitch) { harmony.push_back (n); ++polyNotes; }
    return polyNotes >= 32;
}

std::vector<int> pcsOf (const Triad& t) { return { t.root, (t.root + (t.minor ? 3 : 4)) % 12, (t.root + 7) % 12 }; }
} // namespace

int main (int argc, char** argv)
{
    bool details = false, songs = false;
    juce::Array<juce::File> files;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg = juce::String::fromUTF8 (argv[i]);
        if (arg == "--details") { details = true; continue; }
        if (arg == "--songs") { songs = true; continue; }
        files.addArray (juce::File (arg).findChildFiles (juce::File::findFiles, true, "*.mid;*.midi"));
    }

    int bars = 0, exactTop = 0, compatTop = 0, exactAll = 0, keyOk = 0, fileCount = 0;
    for (const auto& f : files)
    {
        auto notes = readNotes (f);
        if (notes.size() < 4) continue;
        std::vector<Raw> melodyPart;
        if (songs)
        {
            std::vector<Raw> harmony;
            if (! splitSong (notes, melodyPart, harmony)) continue;
            // look at 16 bars from the first bar where melody and harmony both play
            double start = 1e9;
            for (const auto& n : melodyPart) start = std::min (start, n.on);
            double hStart = 1e9;
            for (const auto& n : harmony) hStart = std::min (hStart, n.on);
            const double from = std::floor (std::max (start, hStart) / 4.0) * 4.0, to = from + 64.0;
            auto window = [&] (std::vector<Raw> v)
            {
                std::vector<Raw> out;
                for (auto n : v) if (n.on >= from && n.on < to) { n.on -= from; n.off = std::min (n.off, to) - from; out.push_back (n); }
                return out;
            };
            melodyPart = window (melodyPart);
            notes = window (harmony);
            if (melodyPart.size() < 16 || notes.size() < 16) continue;
        }
        double end = 0;
        for (const auto& n : notes) end = std::max (end, n.off);
        const int nBars = juce::jlimit (1, 16, (int) std::ceil (end / 4.0 - 1e-6));
        const int barsUsed = nBars <= 4 ? 4 : nBars <= 8 ? 8 : 16;

        auto toState = [&] (const std::vector<Raw>& src)
        {
            cg::State st;
            st.bars = barsUsed;
            for (const auto& n : src)
            {
                const int s = (int) std::lround (n.on * cg::stepsPerBeat), d = std::max (1, (int) std::lround ((n.off - n.on) * cg::stepsPerBeat));
                if (s < barsUsed * 4 * cg::stepsPerBeat) st.notes.push_back ({ n.p, s, std::min (d, barsUsed * 4 * cg::stepsPerBeat - s) });
            }
            return st;
        };

        auto top = toState (songs ? melodyPart : topVoice (notes));
        auto all = toState (notes);
        const auto A = cg::analyse (top, true);
        const auto B = cg::analyse (all, true);
        ++fileCount;

        // key: same scale as the key found from all the notes (relative keys count as the same)
        auto scaleOf = [] (const cg::Key& k) { std::array<bool, 12> s {}; for (auto x : cg::modes()[(size_t) k.mode].iv) s[(size_t) ((x + k.tonic) % 12)] = true; return s; };
        keyOk += scaleOf (A.key) == scaleOf (B.key);

        juce::String line;
        for (int i = 0; i < std::min (nBars, A.nSeg); ++i)
        {
            const auto t = truthChord (notes, i * 4.0, i * 4.0 + 4.0);
            if (t.root < 0) continue;
            ++bars;
            auto matches = [&] (const cg::Analysis& X, int& exact, int* compat)
            {
                const auto& c = X.chordAt (i);
                const bool minor = c.q == cg::Quality::min || c.q == cg::Quality::dim;
                if (c.root == t.root && minor == t.minor) ++exact;
                if (compat)
                {
                    const int third = (c.root + (minor ? 3 : 4)) % 12, fifth = (c.root + (c.q == cg::Quality::dim ? 6 : 7)) % 12;
                    int common = 0;
                    for (int pc : pcsOf (t)) common += (pc == c.root) + (pc == third) + (pc == fifth);
                    *compat += common >= 2;
                }
            };
            matches (A, exactTop, &compatTop);
            matches (B, exactAll, nullptr);
            if (details)
                line << cg::pcName (t.root, false, false) << (t.minor ? "m" : "") << "/" << cg::chordName (A.chordAt (i), A, 0, true) << "/" << cg::chordName (B.chordAt (i), B, 0, true) << " ";
        }
        if (details) std::cout << f.getFileNameWithoutExtension().substring (0, 6) << " " << cg::keyTitle (B.key) << ": " << line << "\n";
    }

    auto pct = [] (int a, int b) { return b ? juce::String (100.0 * a / b, 1) + "%" : juce::String ("-"); };
    std::cout << "files " << fileCount << ", bars " << bars << "\n"
              << "from the top voice only: same chord " << pct (exactTop, bars) << ", shares 2+ notes " << pct (compatTop, bars) << "\n"
              << "from all notes (sanity check): same chord " << pct (exactAll, bars) << "\n"
              << "key matches the full harmony: " << pct (keyOk, fileCount) << "\n";
    return 0;
}
