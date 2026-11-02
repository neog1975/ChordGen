#include "Engine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace cg
{
namespace
{
int mod12 (int n) { return ((n % 12) + 12) % 12; }

// Weights tuned on ~2300 trance MIDI files (chord packs and full arrangements, see Tests/ChordEval):
// the notes of the bar count more than typical-progression habits, and repeating a chord is normal.
constexpr double fitWeight = 4.0, transWeight = 0.6, repeatScale = 0.2;

const juce::String flatSign  = juce::String::fromUTF8 ("\xe2\x99\xad");
const juce::String sharpSign = juce::String::fromUTF8 ("\xe2\x99\xaf");
const juce::String degree    = juce::String::fromUTF8 ("\xc2\xb0");
const juce::String arrow     = juce::String::fromUTF8 ("\xe2\x86\x92");
const juce::String enDash    = juce::String::fromUTF8 ("\xe2\x80\x93");

const char* const sharpNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const flatNames[]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

const std::array<double, 12> kkMajor { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
const std::array<double, 12> kkMinor { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

bool contains (const std::vector<int>& v, int x) { return std::find (v.begin(), v.end(), x) != v.end(); }

std::vector<int> triad (const Chord& c)
{
    const int t = (c.q == Quality::maj || c.q == Quality::aug) ? 4 : 3;
    const int f = c.q == Quality::dim ? 6 : c.q == Quality::aug ? 8 : 7;
    return { 0, t, f };
}

char func (const Chord& c, bool minor)
{
    const int s = c.semi;
    if (c.sec >= 0) return 'D';
    if (s == 0) return 'T';
    if (! minor)
    {
        if (s == 9 || s == 4) return 'T';
        if (s == 5 || s == 2 || s == 8 || s == 1 || s == 10 || s == 3) return 'S';
        return 'D';
    }
    if (s == 3) return 'T';
    if (s == 8 || s == 5 || s == 2 || s == 1) return 'S';
    return 'D';
}

struct Spelled { juce::String sfx; std::vector<int> iv; };

// Chord symbol suffix and intervals for a complexity level (0 triads, 1 7ths, 2 9ths, 3 jazz).
Spelled spell (const Chord& c, int cx, bool minor)
{
    const auto tri = triad (c);
    const int t = tri[1], f = tri[2];
    if (c.q == Quality::aug) return { "aug", { 0, 4, 8 } };
    if (cx == 0) return { c.q == Quality::maj ? "" : c.q == Quality::min ? "m" : "dim", { 0, t, f } };

    const char fn = func (c, minor);
    int sev = c.sev;
    if (c.q == Quality::min && sev == 11) sev = 10;
    if (c.q == Quality::maj && (fn == 'D' || c.sec >= 0) && (c.semi == 7 || c.sec >= 0)) sev = 10;
    if (c.q == Quality::dim) return sev == 9 ? Spelled { "dim7", { 0, 3, 6, 9 } } : Spelled { "m7b5", { 0, 3, 6, 10 } };

    const bool dom = c.q == Quality::maj && sev == 10;
    if (cx == 1) return { c.q == Quality::min ? "m7" : dom ? "7" : "maj7", { 0, t, f, sev } };
    if (cx == 2)
    {
        if (c.q == Quality::min) return c.nin == 1 ? Spelled { "m7", { 0, 3, 7, 10 } } : Spelled { "m9", { 0, 3, 7, 10, 14 } };
        return dom ? Spelled { "9", { 0, 4, 7, 10, 14 } } : Spelled { "maj9", { 0, 4, 7, 11, 14 } };
    }
    if (c.q == Quality::min)
    {
        if (c.nin == 1) return { "m7", { 0, 3, 7, 10 } };
        return c.semi == 0 ? Spelled { "m9", { 0, 3, 7, 10, 14 } } : Spelled { "m11", { 0, 3, 10, 14, 17 } };
    }
    if (dom)
    {
        const bool toMinor = minor || c.sec == 9 || c.sec == 2 || c.sec == 4;
        return toMinor ? Spelled { "7b9", { 0, 4, 7, 10, 13 } } : Spelled { "13", { 0, 4, 10, 14, 21 } };
    }
    return c.semi == 0 ? Spelled { "6/9", { 0, 4, 7, 9, 14 } } : Spelled { "maj9", { 0, 4, 7, 11, 14 } };
}

std::vector<Item> itemsIn (const State& st, int a, int b)
{
    const auto& T = timeSigs()[(size_t) st.ts];
    std::vector<Item> out;
    for (const auto& n : st.notes)
    {
        if (n.s >= b || n.s + n.d <= a) continue;
        const int ov = std::min (b, n.s + n.d) - std::max (a, n.s);
        const bool inside = n.s >= a;
        const double acc = inside ? accentAt (T, n.s) * (n.s == a ? 1.15 : 1.0) : 0.7;
        out.push_back ({ n, ov * acc, acc, inside });
    }
    return out;
}

std::array<double, 12> pcWeights (const State& st)
{
    const auto& T = timeSigs()[(size_t) st.ts];
    std::array<double, 12> w {};
    for (const auto& n : st.notes)
        w[(size_t) (n.p % 12)] += n.d * accentAt (T, n.s);

    if (! st.notes.empty())
    {
        auto first = st.notes.front(), last = st.notes.front();
        for (const auto& n : st.notes)
        {
            if (n.s < first.s) first = n;
            if (n.s + n.d >= last.s + last.d) last = n;
        }
        w[(size_t) (first.p % 12)] += first.d * 0.5;
        w[(size_t) (last.p % 12)] += last.d * 0.8;
    }
    return w;
}

double corr (const std::array<double, 12>& x, const std::array<double, 12>& y)
{
    const double mx = std::accumulate (x.begin(), x.end(), 0.0) / 12.0, my = std::accumulate (y.begin(), y.end(), 0.0) / 12.0;
    double num = 0, dx = 0, dy = 0;
    for (size_t i = 0; i < 12; ++i)
    {
        const double a = x[i] - mx, b = y[i] - my;
        num += a * b; dx += a * a; dy += b * b;
    }
    return dx > 0 && dy > 0 ? num / std::sqrt (dx * dy) : 0.0;
}

int refineMode (int tonic, bool minor, const std::array<double, 12>& w)
{
    const std::vector<int> opts = minor ? std::vector<int> { 1, 2, 3, 4 } : std::vector<int> { 0, 5, 6 };
    int best = opts[0];
    double bestScore = 1e9;
    for (size_t i = 0; i < opts.size(); ++i)
    {
        std::array<bool, 12> set {};
        for (auto x : modes()[(size_t) opts[i]].iv) set[(size_t) ((x + tonic) % 12)] = true;
        double out = 0;
        for (size_t pc = 0; pc < 12; ++pc) if (! set[pc]) out += w[pc];
        const double sc = out + (double) i * 0.01;
        if (sc < bestScore) { bestScore = sc; best = opts[i]; }
    }
    return best;
}

std::vector<Chord> buildChords (const Key& key, bool borrowed)
{
    const auto& M = modes()[(size_t) key.mode];
    std::vector<Chord> list;
    auto add = [&] (int semi, Quality q, int sev, int nin, bool isBorrowed = false, const char* tag = "", int sec = -1)
    {
        Chord c;
        c.root = (key.tonic + semi) % 12; c.semi = semi; c.q = q; c.sev = sev; c.nin = nin;
        c.borrowed = isBorrowed; c.tag = tag; c.sec = sec;
        for (const auto& x : list) if (x.code() == c.code()) return;
        list.push_back (c);
    };

    for (int i = 0; i < 7; ++i)
    {
        const int r = M.iv[(size_t) i];
        auto rel = [&] (int k) { return mod12 (M.iv[(size_t) ((i + k) % 7)] - r); };
        const int t = rel (2), f = rel (4);
        if (t == 4 && f == 7)      add (r, Quality::maj, rel (6), rel (1));
        else if (t == 3 && f == 7) add (r, Quality::min, rel (6), rel (1));
        else if (t == 3 && f == 6) add (r, Quality::dim, rel (6), rel (1));
        else if (t == 4 && f == 8) add (r, Quality::aug, rel (6), rel (1));
    }

    if (borrowed)
    {
        if (! M.minor)
        {
            add (5, Quality::min, 10, 2, true, "borrowed from minor");
            add (8, Quality::maj, 11, 2, true, "borrowed from minor");
            add (10, Quality::maj, 10, 2, true, "borrowed from minor");
            add (3, Quality::maj, 11, 2, true, "borrowed from minor");
            add (2, Quality::maj, 10, 2, true, "secondary dominant", 7);
            add (4, Quality::maj, 10, 2, true, "secondary dominant", 9);
            add (9, Quality::maj, 10, 2, true, "secondary dominant", 2);
        }
        else
        {
            add (7, Quality::maj, 10, 2, true, "major V from harmonic minor");
            add (5, Quality::maj, 10, 2, true, "major IV from Dorian");
            add (1, Quality::maj, 11, 2, true, "flat II from Phrygian");
            add (11, Quality::dim, 9, 2, true, "leading-tone chord");
        }
    }
    return list;
}

std::vector<KeyCandidate> detectKeys (const State& st, const std::array<double, 12>& w)
{
    struct Raw { int tonic; bool minor; double r, rank; };
    std::vector<Raw> res;
    for (int t = 0; t < 12; ++t)
    {
        std::array<double, 12> x {};
        for (size_t i = 0; i < 12; ++i) x[i] = w[(i + (size_t) t) % 12];
        res.push_back ({ t, false, corr (x, kkMajor), 0 });
        res.push_back ({ t, true, corr (x, kkMinor), 0 });
    }
    std::stable_sort (res.begin(), res.end(), [] (const Raw& a, const Raw& b) { return a.r > b.r; });

    // Short melodies make the profile match ambiguous (relative keys share notes), so re-rank the
    // best candidates by how well the key's own chords harmonize each bar and whether the tonic
    // chord fits the opening bar. Minor keys may use the major V (leading tone).
    const int spBar = timeSigs()[(size_t) st.ts].steps;
    std::vector<std::vector<Item>> bars;
    for (int b = 0; b < st.bars; ++b)
    {
        auto it = itemsIn (st, b * spBar, (b + 1) * spBar);
        if (! it.empty()) bars.push_back (std::move (it));
    }
    auto triFit = [] (int root, const std::vector<int>& tri, const std::vector<Item>& items)
    {
        double tot = 0, s = 0;
        for (const auto& it : items) { tot += it.w; s += it.w * (contains (tri, mod12 (it.n.p - root)) ? 1.0 : -0.5); }
        return tot > 0 ? s / tot : 0.0;
    };

    for (size_t k = 0; k < res.size(); ++k)
    {
        auto& c = res[k];
        if (k >= 8) { c.rank = c.r - 2; continue; }
        auto chords = buildChords ({ c.tonic, c.minor ? 1 : 0 }, false);
        if (c.minor) { Chord v; v.root = (c.tonic + 7) % 12; v.q = Quality::maj; chords.push_back (v); }
        double harm = 0;
        for (const auto& items : bars)
        {
            double best = -1e9;
            for (const auto& ch : chords) best = std::max (best, triFit (ch.root, triad (ch), items));
            harm += best;
        }
        if (! bars.empty()) harm /= (double) bars.size();
        const std::vector<int> tonicTri = c.minor ? std::vector<int> { 0, 3, 7 } : std::vector<int> { 0, 4, 7 };
        c.rank = 0.6 * c.r + 0.5 * harm
               + 0.45 * (bars.empty() ? 0.0 : triFit (c.tonic, tonicTri, bars.front()))
               + 0.1 * (bars.empty() ? 0.0 : triFit (c.tonic, tonicTri, bars.back()));
    }
    std::stable_sort (res.begin(), res.end(), [] (const Raw& a, const Raw& b) { return a.rank > b.rank; });

    std::vector<KeyCandidate> out;
    // Church modes only when the music really sits on their tonic (first and last bar);
    // otherwise the same notes are named as the plain major or minor key they belong to.
    const std::vector<int> tonicMajor { 0, 4, 7 }, tonicMinor { 0, 3, 7 };
    auto sitsOn = [&] (int tonic, bool minor)
    {
        const auto& tri = minor ? tonicMinor : tonicMajor;
        return ! bars.empty() && triFit (tonic, tri, bars.front()) > 0.5 && triFit (tonic, tri, bars.back()) > 0.5;
    };
    for (const auto& c : res)
    {
        Key k { c.tonic, refineMode (c.tonic, c.minor, w) };
        if (k.mode >= 3 && ! sitsOn (k.tonic, modes()[(size_t) k.mode].minor))
        {
            // same notes as: Dorian = major a tone lower, Phrygian = a major third lower,
            // Mixolydian = a fourth higher, Lydian = a fourth lower
            const int parent = mod12 (k.tonic + modes()[(size_t) k.mode].off);
            const Key major { parent, 0 }, minor { mod12 (parent + 9), 1 };
            int rMaj = 1000, rMin = 1000;
            for (size_t i = 0; i < res.size(); ++i)
            {
                if (res[i].tonic == major.tonic && ! res[i].minor) rMaj = std::min (rMaj, (int) i);
                if (res[i].tonic == minor.tonic && res[i].minor) rMin = std::min (rMin, (int) i);
            }
            k = rMin < rMaj ? minor : major;
        }
        out.push_back ({ k, c.r });
    }
    return out;
}

struct ToneSets { std::vector<int> tri, ext; };
ToneSets toneSets (const Chord& c, int cx, bool minor)
{
    ToneSets t { triad (c), {} };
    for (auto x : spell (c, cx, minor).iv)
    {
        const int pc = x % 12;
        if (! contains (t.tri, pc) && ! contains (t.ext, pc)) t.ext.push_back (pc);
    }
    return t;
}

Fit fit (const Chord& c, const std::vector<Item>& items, int cx, const Analysis& A)
{
    const auto ts = toneSets (c, cx, A.minor);
    double tot = 0, sc = 0, good = 0;
    for (const auto& it : items)
    {
        const int rel = mod12 (it.n.p - c.root);
        tot += it.w;
        // a note outside the scale is strong evidence for the chord that contains it (G# -> E in A minor)
        const bool chrom = ! A.scale[(size_t) (it.n.p % 12)];
        double cr;
        if (contains (ts.tri, rel)) { cr = chrom ? 2.5 : 1.0; good += it.w; } // root, third and fifth alike: melodies sit on all of them
        else if (contains (ts.ext, rel)) { cr = chrom ? 1.2 : 0.55; good += it.w; }
        else if (chrom) cr = -2.5;
        else if (rel == c.sev) cr = 0.15;
        else if (rel == c.nin) cr = 0.1;
        else
        {
            cr = -0.5;
            const bool halfStepAbove = std::any_of (ts.tri.begin(), ts.tri.end(), [rel] (int x) { return (x + 1) % 12 == rel; });
            if (halfStepAbove) cr -= (it.inside && it.acc >= 1.05) ? 0.8 : 0.5;
        }
        sc += it.w * cr;
    }
    if (tot <= 0) return {};
    return { sc / tot, (int) std::lround (good / tot * 100.0) };
}

double trans (const Chord& a, const Chord& b, const StyleInfo& st, bool minor)
{
    if (a.code() == b.code()) return st.repeat * repeatScale;
    double s = 0;
    switch (mod12 (b.root - a.root))
    {
        case 5:  s += 0.6 * st.fifth; break;
        case 7:  s += 0.12; break;
        case 9:  s += 0.3; break;
        case 8:  s += 0.28; break;
        case 2:  s += 0.35 * st.step; break;
        case 10: s += 0.28 * st.step; break;
        case 1:  s += 0.05; break;
        case 11: s += 0.1; break;
        case 3:  s += 0.1; break;
        case 4:  s += 0.08; break;
        case 6:  s -= 0.35; break;
        default: break;
    }
    const char fa = func (a, minor), fb = func (b, minor);
    if (fa == 'T' && fb == 'S') s += 0.25;
    else if (fa == 'S' && fb == 'D') s += 0.3;
    else if (fa == 'D' && fb == 'T') s += 0.4;
    else if (fa == 'T' && fb == 'D') s += 0.1;
    else if (fa == 'S' && fb == 'T') s += 0.15;
    else if (fa == 'D' && fb == 'S') s -= 0.35 * st.strict;
    else if (fa == 'S' && fb == 'S') s += 0.05;

    if (a.sec >= 0) s += b.semi == a.sec ? 0.7 : -0.7;
    if (a.q == Quality::dim) { const int iv = mod12 (b.root - a.root); s += (iv == 1 || iv == 5) ? 0.3 : -0.3; }

    for (const auto& p : minor ? st.patMin : st.patMaj)
        for (size_t i = 0; i < p.size(); ++i)
            if (p[i] == a.semi && p[(i + 1) % p.size()] == b.semi)
                return s + st.patternW;
    return s;
}

double prior (const Chord& c, const StyleInfo& st, Style style, bool minor)
{
    double s = 0;
    // the major V in minor is the most common borrowed chord, so it costs less
    if (c.borrowed) s -= 0.45 * (1 - st.borrowed) * ((minor && c.semi == 7) ? 0.3 : 1.0);
    if (c.q == Quality::dim) s -= style == Style::jazz ? 0.05 : 0.3;
    if (c.q == Quality::aug) s -= 0.7;
    if (minor)
        if (auto it = st.bonusMin.find (c.semi); it != st.bonusMin.end()) s += it->second;
    return s;
}

double hash3 (int a, int b, int c)
{
    uint32_t h = 2166136261u ^ (uint32_t) a;
    h = (h ^ (uint32_t) b) * 16777619u;
    h = (h ^ (uint32_t) c) * 16777619u;
    h ^= h >> 13;
    h *= 1274126177u;
    return (double) (h % 10000u) / 10000.0;
}

std::vector<std::vector<int>> search (const State& st, const Analysis& A)
{
    const auto& S = styleInfo (st.style);
    const auto& chords = A.chords;
    const size_t B = 250;
    struct Node { int c, prev; double score; };
    std::vector<Node> arena;
    arena.reserve ((size_t) A.nSeg * B + 16);
    std::vector<int> beam { -1 };

    for (int i = 0; i < A.nSeg; ++i)
    {
        std::vector<int> opts;
        const auto lock = st.locks.find (i);
        for (int j = 0; j < (int) chords.size(); ++j)
            if (lock == st.locks.end() || chords[(size_t) j].code() == lock->second)
                opts.push_back (j);

        std::vector<Node> cand;
        cand.reserve (beam.size() * opts.size());
        for (const int p : beam)
        {
            const double base = p < 0 ? 0.0 : arena[(size_t) p].score;
            for (const int j : opts)
            {
                const auto& c = chords[(size_t) j];
                double s = base + fitWeight * A.fits[(size_t) i][(size_t) j].score + prior (c, S, st.style, A.minor);
                s += p >= 0 ? transWeight * trans (chords[(size_t) arena[(size_t) p].c], c, S, A.minor) : (c.semi == 0 ? 0.5 : 0.0);
                if (st.seed != 0) s += (hash3 (st.seed, i, j) - 0.5) * 0.9;
                cand.push_back ({ j, p, s });
            }
        }
        const size_t keep = std::min (B, cand.size());
        std::partial_sort (cand.begin(), cand.begin() + (long) keep, cand.end(), [] (const Node& a, const Node& b) { return a.score > b.score; });
        beam.clear();
        for (size_t k = 0; k < keep; ++k) { arena.push_back (cand[k]); beam.push_back ((int) arena.size() - 1); }
    }

    struct Full { std::vector<int> seq; double score; };
    std::vector<Full> full;
    for (const int p : beam)
    {
        Full f;
        for (int q = p; q >= 0; q = arena[(size_t) q].prev) f.seq.insert (f.seq.begin(), arena[(size_t) q].c);
        f.score = arena[(size_t) p].score;
        const auto& last = chords[(size_t) f.seq.back()];
        if (f.seq.size() > 1) f.score += 0.7 * trans (last, chords[(size_t) f.seq.front()], S, A.minor);
        if (func (last, A.minor) != 'T') f.score += 0.15;
        full.push_back (std::move (f));
    }
    std::stable_sort (full.begin(), full.end(), [] (const Full& a, const Full& b) { return a.score > b.score; });

    std::vector<std::vector<int>> out;
    const int minD = std::max (1, A.nSeg / 4);
    for (const int thr : { minD, 1 })
    {
        for (const auto& f : full)
        {
            if (out.size() >= 5) break;
            if (std::find (out.begin(), out.end(), f.seq) != out.end()) continue;
            bool ok = true;
            for (const auto& o : out)
            {
                int diff = 0;
                for (size_t i = 0; i < o.size(); ++i) diff += o[i] != f.seq[i];
                if (diff < thr) { ok = false; break; }
            }
            if (ok) out.push_back (f.seq);
        }
    }
    return out;
}

double moveCost (std::vector<int> a, std::vector<int> b)
{
    std::sort (a.begin(), a.end());
    std::sort (b.begin(), b.end());
    double s = 0;
    if (a.size() == b.size())
    {
        for (size_t i = 0; i < a.size(); ++i) s += std::abs (a[i] - b[i]);
        return s;
    }
    auto near = [] (int v, const std::vector<int>& arr) { int m = 1000; for (auto u : arr) m = std::min (m, std::abs (u - v)); return m; };
    for (auto v : b) s += near (v, a);
    for (auto v : a) s += near (v, b);
    return s / 2;
}

std::vector<Voicing> voiceProg (const State& st, const Analysis& A)
{
    std::vector<Voicing> out;
    std::vector<int> prev;
    const double center = 62 + st.octave * 12;
    const int base = 52 + st.octave * 12;

    for (int i = 0; i < A.nSeg; ++i)
    {
        const auto& c = A.chordAt (i);
        const auto iv = spell (c, st.complexity, A.minor).iv;
        std::vector<int> pcs;
        for (auto x : iv)
            if (! contains (pcs, (c.root + x) % 12)) pcs.push_back ((c.root + x) % 12);
        // five-note chords leave out the fifth, never the root: without the root, rich chords
        // all blur into the same rootless cluster
        const int fifth = (c.root + triad (c)[2]) % 12;
        if (pcs.size() > 4 && contains (pcs, fifth) && c.q != Quality::dim && c.q != Quality::aug)
            pcs.erase (std::remove (pcs.begin(), pcs.end(), fifth), pcs.end());
        std::sort (pcs.begin(), pcs.end(), [&] (int a, int b) { return mod12 (a - c.root) < mod12 (b - c.root); });
        const bool rich = pcs.size() >= 4;

        int melMin = 1000;
        for (const auto& it : A.items[(size_t) i]) melMin = std::min (melMin, it.n.p);

        std::vector<int> best;
        double bestCost = 1e18;
        const size_t rots = st.rootVoicing ? 1 : pcs.size();
        for (size_t k = 0; k < rots; ++k)
        {
            std::vector<int> order (pcs.begin() + (long) k, pcs.end());
            order.insert (order.end(), pcs.begin(), pcs.begin() + (long) k);
            for (int lo = base - 2; lo <= base + 11; ++lo)
            {
                if (lo % 12 != order[0]) continue;
                std::vector<int> close { lo };
                for (size_t j = 1; j < order.size(); ++j) { int n = close.back() + 1; while (n % 12 != order[j]) ++n; close.push_back (n); }

                // rich chords may also use a drop-2 voicing (second-highest note an octave down),
                // which spreads the chord and keeps neighbouring chords from sounding alike
                std::vector<std::vector<int>> shapes { close };
                if (rich)
                {
                    auto drop2 = close;
                    drop2[drop2.size() - 2] -= 12;
                    std::sort (drop2.begin(), drop2.end());
                    shapes.push_back (drop2);
                }

                for (const auto& v : shapes)
                {
                    const double mid = (v.front() + v.back()) / 2.0;
                    double cost = prev.empty() ? std::abs (mid - center) * 0.6 : moveCost (prev, v) + std::abs (mid - center) * 0.15;
                    if (melMin < 1000 && v.back() >= melMin) cost += (v.back() - melMin + 1) * 1.5;
                    if (rich)
                    {
                        if (mod12 (v.front() - c.root) != 0) cost += 6.0; // keep the root at the bottom of rich chords
                        int seconds = 0;
                        for (size_t j = 1; j < v.size(); ++j) seconds += (v[j] - v[j - 1]) <= 2;
                        cost += seconds * 1.5;                            // stacked seconds sound like a smear
                        cost += std::max (0, 11 - (v.back() - v.front())) * 0.6; // prefer a spread chord
                    }
                    if (v.front() < 45) cost += (45 - v.front()) * 2.0;    // avoid muddy low clusters
                    if (! prev.empty() && v == prev) cost += 4.0;
                    if (cost < bestCost) { bestCost = cost; best = v; }
                }
            }
        }
        prev = best;
        out.push_back ({ best, 36 + c.root });
    }
    return out;
}

void rollRange (const State& st, Analysis& A)
{
    if (st.notes.empty()) { A.rangeLo = 57; A.rangeHi = 84; return; }
    int lo = 127, hi = 0;
    for (const auto& n : st.notes) { lo = std::min (lo, n.p); hi = std::max (hi, n.p); }
    lo -= 3; hi += 3;
    while (hi - lo < 23) { --lo; if (hi - lo < 23) ++hi; }
    A.rangeLo = std::max (24, lo);
    A.rangeHi = std::min (108, hi);
}

juce::String prettify (juce::String s)
{
    return s.replace ("b5", flatSign + "5").replace ("b9", flatSign + "9");
}

juce::String transText (const Analysis& A, const Chord& a, const Chord& b, int cx)
{
    const char fa = func (a, A.minor), fb = func (b, A.minor);
    const int iv = mod12 (b.root - a.root);
    const auto ra = roman (a, A), rb = roman (b, A), move = ra + " " + arrow + " " + rb;
    if (a.code() == b.code()) return "Holds " + chordName (a, A, cx) + " from the previous bar.";
    if (a.sec >= 0 && b.semi == a.sec) return move + ": the secondary dominant resolves to its target.";
    if (fa == 'D' && fb == 'T') return move + ": dominant resolves to tonic.";
    if (fa == 'S' && fb == 'D') return move + ": subdominant builds tension toward the dominant.";
    if (iv == 5) return move + ": the root falls a fifth, the strongest root move.";
    if (iv == 2 || iv == 10) return move + ": the root moves by step.";
    if (iv == 8 || iv == 9) return move + ": the root falls a third; the chords share two notes.";
    return move + ".";
}

struct Preset { const char* name; int ts, bars; std::vector<std::array<int, 3>> notes; };
const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p {
        { "A minor pop", 0, 4, { {69,0,2},{72,2,1},{76,3,1},{81,4,2},{79,6,1},{76,7,1},{77,8,2},{76,10,1},{72,11,1},{69,12,3},{72,15,1},{72,16,2},{76,18,2},{79,20,2},{76,22,1},{74,23,1},{71,24,2},{68,26,2},{74,28,2},{71,30,2} } },
        { "C major hook", 0, 4, { {76,0,2},{79,2,2},{76,4,1},{74,5,1},{72,6,2},{74,8,2},{71,10,2},{74,12,1},{79,13,1},{74,14,2},{72,16,2},{76,18,2},{81,20,2},{79,22,1},{76,23,1},{77,24,3},{76,27,1},{72,28,2},{69,30,2} } },
        { "Trap, E Phrygian", 0, 4, { {64,0,2},{64,3,1},{67,4,1},{65,6,2},{64,8,3},{71,12,1},{72,13,1},{71,14,2},{72,16,2},{71,19,1},{67,20,2},{65,22,2},{64,24,4},{65,29,1},{64,30,2} } },
        { "Lo-Fi, D Dorian", 0, 4, { {69,0,2},{72,2,1},{74,3,3},{77,6,2},{71,8,2},{74,10,2},{79,12,3},{77,15,1},{77,16,2},{76,18,1},{74,19,1},{69,20,4},{71,24,2},{69,26,1},{67,27,1},{71,28,2},{74,30,2} } },
        { "Waltz, G major", 1, 4, { {71,0,2},{74,2,2},{79,4,2},{78,6,3},{76,9,1},{74,10,2},{72,12,2},{76,14,2},{74,16,2},{71,18,4},{67,22,2} } },
    };
    return p;
}

struct MidiTrack { juce::String name; std::vector<Event> evs; int channel; int velocity; };

juce::MemoryBlock writeMidi (const std::vector<MidiTrack>& tracks, double bpm, const TimeSig& T)
{
    constexpr int ppq = 480;
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);

    juce::MidiMessageSequence tempo;
    tempo.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / bpm)), 0);
    tempo.addEvent (juce::MidiMessage::timeSignatureMetaEvent (T.num, T.den), 0);
    mf.addTrack (tempo);

    for (const auto& tr : tracks)
    {
        juce::MidiMessageSequence seq;
        seq.addEvent (juce::MidiMessage::textMetaEvent (3, tr.name), 0);
        for (const auto& e : tr.evs)
        {
            seq.addEvent (juce::MidiMessage::noteOn (tr.channel, e.midi, (juce::uint8) tr.velocity), std::round (e.t * ppq));
            seq.addEvent (juce::MidiMessage::noteOff (tr.channel, e.midi), std::round ((e.t + e.d) * ppq));
        }
        seq.sort();
        mf.addTrack (seq);
    }

    juce::MemoryOutputStream out;
    mf.writeTo (out, 1);
    return out.getMemoryBlock();
}

std::vector<Event> ofType (const std::vector<Event>& evs, int type)
{
    std::vector<Event> out;
    for (const auto& e : evs) if (e.type == type) out.push_back (e);
    return out;
}
} // namespace

//==============================================================================
const std::vector<ModeInfo>& modes()
{
    static const std::vector<ModeInfo> m {
        { "major",          { 0, 2, 4, 5, 7, 9, 11 }, false, 0 },
        { "minor",          { 0, 2, 3, 5, 7, 8, 10 }, true, -9 },
        { "harmonic minor", { 0, 2, 3, 5, 7, 8, 11 }, true, -9 },
        { "Dorian",         { 0, 2, 3, 5, 7, 9, 10 }, true, -2 },
        { "Phrygian",       { 0, 1, 3, 5, 7, 8, 10 }, true, -4 },
        { "Mixolydian",     { 0, 2, 4, 5, 7, 9, 10 }, false, -7 },
        { "Lydian",         { 0, 2, 4, 6, 7, 9, 11 }, false, -5 },
    };
    return m;
}

const std::vector<TimeSig>& timeSigs()
{
    static const std::vector<TimeSig> t {
        { "4/4", 4 * stepsPerBeat, 4, 4, 4, { 1.5, .8, 1.05, .8, 1.25, .8, 1.05, .8 } },
        { "3/4", 3 * stepsPerBeat, 3, 3, 4, { 1.5, .8, 1.1, .8, 1.1, .8, 0, 0 } },
        { "6/8", 3 * stepsPerBeat, 3, 6, 8, { 1.5, .85, .85, 1.25, .85, .85, 0, 0 } },
    };
    return t;
}

double accentAt (const TimeSig& T, int step)
{
    constexpr int perEighth = stepsPerBeat / 2;
    const int inBar = ((step % T.steps) + T.steps) % T.steps;
    return inBar % perEighth == 0 ? T.accent[(size_t) (inBar / perEighth)] : 0.7; // 16ths and finer: weak
}

const StyleInfo& styleInfo (Style s)
{
    static const std::vector<StyleInfo> styles {
        { "Pop", 1.0, 1.1, .4, .3, -.5, .45, 1, 2400.0f, false, false,
          { {0,7,9,5}, {9,5,0,7}, {0,9,5,7}, {0,5,9,7} }, { {0,8,3,10}, {0,10,8,10}, {0,8,10,0}, {0,3,10,8} }, {},
          { {0,1.5},{1.5,1.5},{3,1} }, { {0,1.5},{1.5,.5},{2,1},{3.5,.5} }, { {0,1},{1,1},{2,1} }, { {0,1.5},{2,1} } },
        { "Trap", .6, 1.3, .3, .6, -.25, .5, 0, 1300.0f, false, false,
          { {0,4,9,5} }, { {0,8,10,0}, {0,1,0,8}, {0,3,8,10}, {0,8,3,10}, {0,5,8,7} }, { {1,.15}, {8,.15}, {10,.1}, {7,-.15} },
          { {0,4} }, { {0,1.5},{2.5,1.5} }, { {0,3} }, { {0,1.5},{2,1} } },
        { "Lo-Fi", 1.2, 1.0, .6, .5, -.6, .45, 2, 950.0f, true, false,
          { {5,4,2,0}, {2,7,0,9}, {5,7,4,9}, {0,9,2,7} }, { {0,5,10,3}, {5,10,3,8}, {0,8,5,7}, {2,7,0,0} }, {},
          { {0,2.5},{2.5,1.5} }, { {0,1.5},{2.5,1} }, { {0,1.5},{1.5,1.5} }, { {0,2} } },
        { "Jazz", 1.6, .8, 1.0, .8, -.7, .5, 3, 1800.0f, false, true,
          { {2,7,0,0}, {0,9,2,7}, {4,9,2,7} }, { {2,7,0,0}, {0,5,10,3}, {8,7,0,0} }, {},
          { {0,.75},{1.5,1} }, {}, { {0,1},{1.5,1} }, {} },
    };
    return styles[(size_t) s];
}

juce::String pcName (int pc, bool flats, bool pretty)
{
    juce::String n = (flats ? flatNames : sharpNames)[mod12 (pc)];
    return pretty ? n.replace ("b", flatSign).replace ("#", sharpSign) : n;
}

static bool flatsFor (const Key& k)
{
    const int parent = mod12 (k.tonic + modes()[(size_t) k.mode].off);
    return parent == 5 || parent == 10 || parent == 3 || parent == 8 || parent == 1 || parent == 6;
}

juce::String keyTitle (const Key& k)
{
    return pcName (k.tonic, flatsFor (k)) + " " + modes()[(size_t) k.mode].name;
}

juce::String chordName (const Chord& c, const Analysis& A, int cx, bool ascii)
{
    const auto sfx = spell (c, cx, A.minor).sfx;
    if (ascii) return (pcName (c.root, A.flats, false) + sfx).replace ("#", "s").replace ("/", "-");
    return pcName (c.root, A.flats) + prettify (sfx);
}

juce::String roman (const Chord& c, const Analysis& A)
{
    static const char* const majT[] = { "I","bII","II","bIII","III","IV","#IV","V","bVI","VI","bVII","VII" };
    static const char* const minT[] = { "I","bII","II","III","#III","IV","#IV","V","VI","#VI","VII","#VII" };
    if (c.sec >= 0)
    {
        juce::String r = majT[c.sec];
        for (const auto& t : A.chords)
            if (t.semi == c.sec && ! t.borrowed && (t.q == Quality::min || t.q == Quality::dim)) { r = r.toLowerCase(); break; }
        return "V/" + r;
    }
    juce::String r = (A.minor ? minT : majT)[c.semi];
    if (c.q == Quality::min || c.q == Quality::dim) r = r.toLowerCase();
    if (c.q == Quality::dim) r += degree;
    if (c.q == Quality::aug) r += "+";
    if (r.startsWith ("b")) r = flatSign + r.substring (1);
    if (r.startsWith ("#")) r = sharpSign + r.substring (1);
    return r;
}

juce::String segmentLabel (int i, double rhythm)
{
    if (rhythm > 1.5) return "Bar " + juce::String (i / 2 + 1) + "." + juce::String (i % 2 + 1);
    if (rhythm < 0.75) return "Bars " + juce::String (2 * i + 1) + enDash + juce::String (2 * i + 2);
    return "Bar " + juce::String (i + 1);
}

Analysis analyse (State& st, bool resuggest)
{
    Analysis A;
    const auto& T = timeSigs()[(size_t) st.ts];
    A.segSteps = (int) std::lround (T.steps / st.rhythm);
    A.nSeg = std::max (1, (int) std::lround (st.bars * st.rhythm));

    const auto w = pcWeights (st);
    A.keys = st.notes.empty() ? std::vector<KeyCandidate> { { { 9, 1 }, 0.0 } } : detectKeys (st, w);
    A.key = st.keyLocked ? st.keyLock : A.keys.front().key;
    A.minor = modes()[(size_t) A.key.mode].minor;
    A.flats = flatsFor (A.key);
    for (auto x : modes()[(size_t) A.key.mode].iv) A.scale[(size_t) ((x + A.key.tonic) % 12)] = true;
    A.chords = buildChords (A.key, st.borrowed);

    for (int i = 0; i < A.nSeg; ++i)
    {
        A.items.push_back (itemsIn (st, i * A.segSteps, (i + 1) * A.segSteps));
        std::vector<Fit> row;
        for (const auto& c : A.chords) row.push_back (fit (c, A.items.back(), st.complexity, A));
        A.fits.push_back (std::move (row));
    }

    auto indexOf = [&] (int code) { for (size_t j = 0; j < A.chords.size(); ++j) if (A.chords[j].code() == code) return (int) j; return -1; };
    for (auto it = st.locks.begin(); it != st.locks.end();)
        it = (it->first >= A.nSeg || indexOf (it->second) < 0) ? st.locks.erase (it) : std::next (it);

    A.sugs = search (st, A);
    st.sugIdx = juce::jlimit (0, (int) A.sugs.size() - 1, st.sugIdx);

    bool valid = (int) st.prog.size() == A.nSeg;
    for (auto code : st.prog) valid = valid && indexOf (code) >= 0;
    if (resuggest || ! valid)
    {
        st.prog.clear();
        for (auto j : A.sugs[(size_t) st.sugIdx]) st.prog.push_back (A.chords[(size_t) j].code());
    }
    for (auto code : st.prog) A.prog.push_back (indexOf (code));
    A.edited = A.prog != A.sugs[(size_t) st.sugIdx];

    A.voicing = voiceProg (st, A);
    for (int i = 0; i < A.nSeg; ++i)
        for (const auto& n : clashes (st, A, A.chordAt (i), i))
            A.clash.push_back (n);
    rollRange (st, A);
    st.selected = juce::jlimit (0, A.nSeg - 1, st.selected);
    return A;
}

std::vector<int> alternatives (const State& st, const Analysis& A, int i)
{
    const auto& S = styleInfo (st.style);
    const auto& cur = A.chordAt (i);
    std::vector<std::pair<double, int>> scored;
    for (size_t j = 0; j < A.chords.size(); ++j)
    {
        const auto& c = A.chords[j];
        if (c.code() == cur.code()) continue;
        double s = fitWeight * A.fits[(size_t) i][j].score + prior (c, S, st.style, A.minor);
        if (i > 0) s += transWeight * trans (A.chordAt (i - 1), c, S, A.minor);
        if (i + 1 < A.nSeg) s += transWeight * trans (c, A.chordAt (i + 1), S, A.minor);
        scored.push_back ({ s, (int) j });
    }
    std::stable_sort (scored.begin(), scored.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (size_t k = 0; k < scored.size() && k < 3; ++k) out.push_back (scored[k].second);
    return out;
}

std::vector<Note> clashes (const State& st, const Analysis& A, const Chord& c, int seg)
{
    const auto ts = toneSets (c, st.complexity, A.minor);
    std::vector<Note> out;
    for (const auto& it : A.items[(size_t) seg])
    {
        const int rel = mod12 (it.n.p - c.root);
        if (contains (ts.tri, rel) || contains (ts.ext, rel)) continue;
        const bool halfStepAbove = std::any_of (ts.tri.begin(), ts.tri.end(), [rel] (int x) { return (x + 1) % 12 == rel; });
        if (it.n.d >= stepsPerBeat || it.acc >= 1.2 || halfStepAbove) out.push_back (it.n);
    }
    return out;
}

Explanation explain (const State& st, const Analysis& A, int i)
{
    const auto& c = A.chordAt (i);
    const auto& f = A.fitAt (i);
    const auto name = chordName (c, A, st.complexity);
    Explanation e;
    e.text = "Why " + name + "?  ";
    e.text += f.pct < 0 ? juce::String ("No melody here, so the progression decides.")
                        : "It covers " + juce::String (f.pct) + "% of the weighted melody in " + segmentLabel (i, st.rhythm).toLowerCase() + ".";
    if (i > 0) e.text += " " + transText (A, A.chordAt (i - 1), c, st.complexity);
    else if (A.nSeg > 1) e.text += " Loops back from " + chordName (A.chordAt (A.nSeg - 1), A, st.complexity) + ".";
    if (c.borrowed) e.text += " " + c.tag.substring (0, 1).toUpperCase() + c.tag.substring (1) + ".";

    juce::StringArray names;
    for (const auto& n : clashes (st, A, c, i)) names.addIfNotAlreadyThere (pcName (n.p, A.flats));
    if (! names.isEmpty())
        e.clash = "Clash: " + names.joinIntoString (", ") + " against " + name + ". Try an alternative or a higher complexity.";
    return e;
}

int keyMatchPercent (const State& st, const Analysis& A)
{
    if (st.notes.empty()) return 0;
    double r = A.keys.front().r;
    if (st.keyLocked)
    {
        r = 0;
        for (const auto& k : A.keys)
            if (k.key.tonic == A.key.tonic && modes()[(size_t) k.key.mode].minor == A.minor) { r = k.r; break; }
    }
    return juce::jlimit (0, 100, (int) std::lround (r * 100));
}

std::vector<Event> buildEvents (const State& st, const Analysis& A, bool withPattern)
{
    const auto& T = timeSigs()[(size_t) st.ts];
    const auto& S = styleInfo (st.style);
    const double L = T.beats, segB = A.segSteps / (double) stepsPerBeat;
    const auto& chPat = T.beats == 4 ? S.ch4 : S.ch3;
    const auto& bsPat = T.beats == 4 ? S.bs4 : S.bs3;
    auto swing = [&] (double t) { return S.swing && std::abs (std::fmod (t, 1.0) - 0.5) < 1e-6 ? t + 0.16 : t; };
    std::vector<Event> ev;

    for (int i = 0; i < A.nSeg; ++i)
    {
        const auto& c = A.chordAt (i);
        const auto& v = A.voicing[(size_t) i];
        const double a = i * segB, b = a + segB;
        std::vector<std::array<double, 3>> hits, bassHits; // t, d, midi (-1 = root)

        if (! withPattern)
        {
            hits.push_back ({ a, segB, -1 });
            bassHits.push_back ({ a, segB, -1 });
        }
        else
        {
            for (int bar = (int) std::floor (a / L); bar * L < b; ++bar)
            {
                for (const auto& [p, d] : chPat) { const double t = bar * L + p; if (t >= a && t < b) hits.push_back ({ t, std::min (d, b - t), -1 }); }
                for (const auto& [p, d] : bsPat) { const double t = bar * L + p; if (t >= a && t < b) bassHits.push_back ({ t, std::min (d, b - t), -1 }); }
            }
            if (S.walk)
            {
                const auto& next = A.chordAt ((i + 1) % A.nSeg);
                const auto tones = spell (c, std::min (st.complexity, 1), A.minor).iv;
                for (double t = a; t < b; t += 1.0)
                {
                    const int k = (int) (t - a);
                    const bool lastBeat = t + 1 >= b;
                    const int pattern[] = { 0, tones[1], tones[2], tones[1] };
                    const int m = lastBeat && segB > 1 ? v.bass + mod12 (next.root - c.root) + (k % 2 ? -1 : 1)
                                                       : v.bass + pattern[k % 4];
                    bassHits.push_back ({ t, 0.95, (double) m });
                }
            }
            if (hits.empty()) hits.push_back ({ a, segB, -1 });
        }

        for (const auto& h : hits)
            for (auto m : v.notes)
                ev.push_back ({ swing (h[0]), h[1] * 0.95, m, 0, 0.8f });
        if (st.bass)
            for (const auto& h : bassHits)
                ev.push_back ({ swing (h[0]), h[1] * 0.95, h[2] < 0 ? v.bass : (int) h[2], 1, 0.9f });
    }

    if (st.playMelody || ! withPattern)
        for (const auto& n : st.notes)
            ev.push_back ({ swing (n.s / (double) stepsPerBeat), n.d / (double) stepsPerBeat * 0.95, n.p, 2, 0.9f });

    std::stable_sort (ev.begin(), ev.end(), [] (const Event& x, const Event& y) { return x.t < y.t; });
    return ev;
}

juce::MemoryBlock chordsMidi (const State& st, const Analysis& A, double bpm)
{
    const auto ev = buildEvents (st, A, st.patternMidi);
    auto evs = ofType (ev, 0);
    if (st.bass) { auto b = ofType (ev, 1); evs.insert (evs.end(), b.begin(), b.end()); }
    return writeMidi ({ { "ChordGen", evs, 1, 90 } }, bpm, timeSigs()[(size_t) st.ts]);
}

juce::MemoryBlock fullMidi (const State& st, const Analysis& A, double bpm)
{
    const auto ev = buildEvents (st, A, st.patternMidi);
    std::vector<Event> mel;
    for (const auto& n : st.notes) mel.push_back ({ n.s / (double) stepsPerBeat, n.d / (double) stepsPerBeat, n.p, 2, 0.9f });
    std::vector<MidiTrack> tracks { { "Chords", ofType (ev, 0), 1, 90 } };
    if (st.bass) tracks.push_back ({ "Bass", ofType (ev, 1), 2, 100 });
    tracks.push_back ({ "Melody", mel, 3, 96 });
    return writeMidi (tracks, bpm, timeSigs()[(size_t) st.ts]);
}

juce::String midiFileBase (const State& st, const Analysis& A)
{
    juce::String s = "ChordGen_" + pcName (A.key.tonic, A.flats, false).replace ("#", "s") + (A.minor ? "m" : "") + "_";
    for (int i = 0; i < A.nSeg; ++i) s += (i ? "-" : "") + chordName (A.chordAt (i), A, st.complexity, true);
    return s.substring (0, 110);
}

ImportResult importMidi (const juce::File& file)
{
    ImportResult r;
    juce::FileInputStream in (file);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) { r.error = "Not a MIDI file"; return r; }
    const int tpq = mf.getTimeFormat();
    if (tpq <= 0) { r.error = "SMPTE timing is not supported"; return r; }

    juce::MidiMessageSequence meta;
    mf.findAllTempoEvents (meta);
    if (meta.getNumEvents() > 0) r.bpm = 60.0 / meta.getEventPointer (0)->message.getTempoSecondsPerQuarterNote();
    meta.clear();
    mf.findAllTimeSigEvents (meta);
    if (meta.getNumEvents() > 0)
    {
        int num = 4, den = 4;
        meta.getEventPointer (0)->message.getTimeSignatureInfo (num, den);
        for (size_t i = 0; i < timeSigs().size(); ++i)
            if (timeSigs()[i].num == num && timeSigs()[i].den == den) r.ts = (int) i;
    }

    struct Raw { int p; double on, off; };
    std::vector<Raw> best;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        std::vector<Raw> notes;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (! m.isNoteOn() || m.getChannel() == 10) continue;
            auto off = seq.getTimeOfMatchingKeyUp (i);
            if (off <= m.getTimeStamp()) off = m.getTimeStamp() + tpq / 4.0;
            notes.push_back ({ m.getNoteNumber(), m.getTimeStamp(), off });
        }
        if (notes.size() > best.size()) best = notes;
    }
    if (best.empty()) { r.error = "No notes found in this file"; return r; }

    // keep the notes as written: a fine grid (1/24 beat), every voice, original lengths
    const double step = tpq / (double) stepsPerBeat;
    std::vector<Note> out;
    for (const auto& n : best)
    {
        const Note note { n.p, (int) std::lround (n.on / step), std::max (1, (int) std::lround ((n.off - n.on) / step)) };
        if (std::none_of (out.begin(), out.end(), [&] (const Note& o) { return o.p == note.p && o.s == note.s; }))
            out.push_back (note);
    }
    std::sort (out.begin(), out.end(), [] (const Note& a, const Note& b) { return a.s != b.s ? a.s < b.s : a.p < b.p; });
    int lastEnd = 0;
    for (const auto& n : out) lastEnd = std::max (lastEnd, n.s + n.d);

    const int spBar = timeSigs()[(size_t) r.ts].steps;
    const int needed = (int) std::ceil (lastEnd / (double) spBar);
    r.bars = needed <= 4 ? 4 : needed <= 8 ? 8 : 16;
    for (auto n : out)
        if (n.s < r.bars * spBar) { n.d = std::min (n.d, r.bars * spBar - n.s); r.notes.push_back (n); }
    return r;
}

juce::StringArray presetNames()
{
    juce::StringArray s;
    for (const auto& p : presets()) s.add (p.name);
    return s;
}

void loadPreset (State& st, const juce::String& name)
{
    for (const auto& p : presets())
    {
        if (name != p.name) continue;
        st.notes.clear();
        for (const auto& n : p.notes) st.notes.push_back ({ n[0], n[1] * stepsPerBeat / 2, n[2] * stepsPerBeat / 2 }); // written in eighths
        st.ts = p.ts; st.bars = p.bars; st.preset = name;
        st.keyLocked = false; st.locks.clear(); st.selected = 0; st.sugIdx = 0; st.seed = 0;
    }
}

juce::var toVar (const State& st)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> notes;
    for (const auto& n : st.notes) notes.add (juce::Array<juce::var> { n.p, n.s, n.d });
    o->setProperty ("notes", notes);
    o->setProperty ("stepsPerBeat", stepsPerBeat);
    o->setProperty ("bars", st.bars);
    o->setProperty ("ts", st.ts);
    o->setProperty ("rhythm", st.rhythm);
    o->setProperty ("complexity", st.complexity);
    o->setProperty ("style", (int) st.style);
    o->setProperty ("borrowed", st.borrowed);
    o->setProperty ("rootVoicing", st.rootVoicing);
    o->setProperty ("bass", st.bass);
    o->setProperty ("playMelody", st.playMelody);
    o->setProperty ("patternMidi", st.patternMidi);
    o->setProperty ("octave", st.octave);
    o->setProperty ("keyLocked", st.keyLocked);
    o->setProperty ("keyTonic", st.keyLock.tonic);
    o->setProperty ("keyMode", st.keyLock.mode);
    juce::Array<juce::var> locks, prog;
    for (const auto& [seg, code] : st.locks) locks.add (juce::Array<juce::var> { seg, code });
    for (auto c : st.prog) prog.add (c);
    o->setProperty ("locks", locks);
    o->setProperty ("prog", prog);
    o->setProperty ("sugIdx", st.sugIdx);
    o->setProperty ("seed", st.seed);
    o->setProperty ("selected", st.selected);
    o->setProperty ("source", st.source);
    o->setProperty ("preset", st.preset);
    return juce::var (o);
}

State fromVar (const juce::var& v)
{
    State st;
    const int scale = stepsPerBeat / juce::jmax (1, (int) v.getProperty ("stepsPerBeat", 2)); // older versions used eighths
    if (auto* notes = v["notes"].getArray())
        for (const auto& n : *notes)
            if (n.size() >= 3) st.notes.push_back ({ (int) n[0], (int) n[1] * scale, (int) n[2] * scale });
    st.bars = juce::jlimit (1, 16, (int) v.getProperty ("bars", 4));
    st.ts = juce::jlimit (0, 2, (int) v.getProperty ("ts", 0));
    st.rhythm = (double) v.getProperty ("rhythm", 1.0);
    st.rhythm = st.rhythm > 1.5 ? 2.0 : st.rhythm < 0.75 ? 0.5 : 1.0;
    st.complexity = juce::jlimit (0, 3, (int) v.getProperty ("complexity", 1));
    st.style = (Style) juce::jlimit (0, numStyles - 1, (int) v.getProperty ("style", 0));
    st.borrowed = v.getProperty ("borrowed", true);
    st.rootVoicing = v.getProperty ("rootVoicing", false);
    st.bass = v.getProperty ("bass", true);
    st.playMelody = v.getProperty ("playMelody", true);
    st.patternMidi = v.getProperty ("patternMidi", true);
    st.octave = juce::jlimit (-1, 1, (int) v.getProperty ("octave", 0));
    st.keyLocked = v.getProperty ("keyLocked", false);
    st.keyLock = { juce::jlimit (0, 11, (int) v.getProperty ("keyTonic", 9)), juce::jlimit (0, 6, (int) v.getProperty ("keyMode", 1)) };
    if (auto* locks = v["locks"].getArray())
        for (const auto& l : *locks)
            if (l.size() >= 2) st.locks[(int) l[0]] = (int) l[1];
    if (auto* prog = v["prog"].getArray())
        for (const auto& c : *prog) st.prog.push_back ((int) c);
    st.sugIdx = (int) v.getProperty ("sugIdx", 0);
    st.seed = (int) v.getProperty ("seed", 0);
    st.selected = (int) v.getProperty ("selected", 0);
    st.source = juce::jlimit (0, 5, (int) v.getProperty ("source", 0));
    st.preset = v.getProperty ("preset", "").toString();
    return st;
}
} // namespace cg
