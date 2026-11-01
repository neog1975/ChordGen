#include "PluginEditor.h"

namespace
{
const juce::String dot = juce::String::fromUTF8 ("\xc2\xb7");

juce::File midiTempFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ChordGen");
    dir.createDirectory();
    return dir;
}

bool isBlack (int note) { const int pc = note % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

juce::String noteWithOctave (int m, bool flats) { return cg::pcName (m, flats) + juce::String (m / 12 - 1); }

void styleSmall (juce::TextButton& b, float fontSize = 12.5f)
{
    b.getProperties().set ("fontSize", fontSize);
    b.getProperties().set ("radius", 7.0f);
}
} // namespace

//==============================================================================
MidiDragButton::MidiDragButton()
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setTooltip ("Drag the chords into an FL piano roll or the playlist");
}

void MidiDragButton::paint (juce::Graphics& g)
{
    const auto a = theme::accent (accent);
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (theme::panel.interpolatedWith (a, 0.08f));
    g.fillRoundedRectangle (r, 12.0f);

    juce::Path outline, dashed;
    outline.addRoundedRectangle (r, 12.0f);
    const float dashes[] = { 6.0f, 4.0f };
    juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (a);
    g.fillPath (dashed);

    g.setColour (file.existsAsFile() ? theme::fg : theme::muted);
    g.setFont (theme::ui (15.0f, true));
    g.drawText ("Drag MIDI " + juce::String::fromUTF8 ("\xe2\x86\x92") + " Piano Roll", getLocalBounds(), juce::Justification::centred);
}

void MidiDragButton::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 4 || ! file.existsAsFile())
        return;

    dragging = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [this] { dragging = false; });
}

//==============================================================================
void AccentDots::paint (juce::Graphics& g)
{
    for (int i = 0; i < 4; ++i)
    {
        const auto r = juce::Rectangle<float> ((float) i * 21.0f + 2.0f, (float) getHeight() * 0.5f - 8.0f, 16.0f, 16.0f);
        g.setColour (theme::accent (i));
        g.fillEllipse (r.reduced (2.0f));
        if (i == selected)
        {
            g.setColour (theme::fg);
            g.drawEllipse (r.reduced (0.5f), 1.5f);
        }
    }
}

void AccentDots::mouseUp (const juce::MouseEvent& e)
{
    const int i = juce::jlimit (0, 3, (int) (e.position.x / 21.0f));
    if (onPick) onPick (i);
}

//==============================================================================
KeyPanel::KeyPanel (ChordGenProcessor& p) : proc (p)
{
    for (int i = 0; i < 3; ++i)
    {
        styleSmall (alts[i]);
        alts[i].setTooltip ("Use this key instead");
        alts[i].onClick = [this, i]
        {
            if (i >= (int) altKeys.size()) return;
            proc.pushHistory();
            proc.state.keyLocked = true;
            proc.state.keyLock = altKeys[(size_t) i];
            proc.commit (true);
        };
        addAndMakeVisible (alts[i]);
    }

    const char* tonics[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    for (int i = 0; i < 12; ++i)
        tonicBox.addItem (juce::String (tonics[i]).replace ("b", juce::String::fromUTF8 ("\xe2\x99\xad")).replace ("#", juce::String::fromUTF8 ("\xe2\x99\xaf")), i + 1);
    for (int i = 0; i < (int) cg::modes().size(); ++i)
    {
        juce::String n = cg::modes()[(size_t) i].name;
        modeBox.addItem (n.substring (0, 1).toUpperCase() + n.substring (1), i + 1);
    }
    auto lockFromBoxes = [this]
    {
        proc.pushHistory();
        proc.state.keyLocked = true;
        proc.state.keyLock = { tonicBox.getSelectedId() - 1, modeBox.getSelectedId() - 1 };
        proc.commit (true);
    };
    tonicBox.onChange = lockFromBoxes;
    modeBox.onChange = lockFromBoxes;
    styleSmall (autoButton);
    autoButton.onClick = [this]
    {
        proc.pushHistory();
        proc.state.keyLocked = false;
        proc.commit (true);
    };
    addAndMakeVisible (tonicBox);
    addAndMakeVisible (modeBox);
    addAndMakeVisible (autoButton);
}

void KeyPanel::refresh()
{
    const auto& A = proc.analysis;
    const auto& st = proc.state;

    altKeys.clear();
    std::vector<double> rs;
    auto seen = [&] (const cg::Key& k) { return k == A.key || std::find (altKeys.begin(), altKeys.end(), k) != altKeys.end(); };
    if (st.keyLocked && ! seen (A.keys.front().key)) { altKeys.push_back (A.keys.front().key); rs.push_back (A.keys.front().r); }
    for (const auto& k : A.keys)
    {
        if (altKeys.size() >= 3) break;
        if (seen (k.key)) continue;
        altKeys.push_back (k.key);
        rs.push_back (k.r);
    }
    for (int i = 0; i < 3; ++i)
    {
        const bool has = i < (int) altKeys.size() && ! st.notes.empty();
        alts[i].setVisible (has);
        if (has)
            alts[i].setButtonText (cg::keyTitle (altKeys[(size_t) i]) + " " + dot + " " + juce::String (juce::jmax (0, (int) std::lround (rs[(size_t) i] * 100))) + "%");
    }
    tonicBox.setSelectedId (A.key.tonic + 1, juce::dontSendNotification);
    modeBox.setSelectedId (A.key.mode + 1, juce::dontSendNotification);
    autoButton.setEnabled (st.keyLocked);
    resized();
    repaint();
}

void KeyPanel::resized()
{
    auto r = getLocalBounds().reduced (20, 18);
    int x = r.getX(), y = r.getY() + 124;
    for (auto& b : alts)
    {
        if (! b.isVisible()) continue;
        const int w = (int) theme::textWidth (theme::ui (12.5f, true), b.getButtonText()) + 22;
        if (x + w > r.getRight()) { x = r.getX(); y += 32; }
        b.setBounds (x, y, w, 26);
        x += w + 6;
    }
    auto bottom = r.removeFromBottom (30);
    tonicBox.setBounds (bottom.removeFromLeft (64));
    bottom.removeFromLeft (8);
    modeBox.setBounds (bottom.removeFromLeft (124));
    bottom.removeFromLeft (8);
    autoButton.setBounds (bottom);
}

void KeyPanel::paint (juce::Graphics& g)
{
    const auto& A = proc.analysis;
    const auto& st = proc.state;
    const auto a = theme::accent (proc.accent);

    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 16.0f);
    g.setColour (theme::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 16.0f, 1.0f);

    auto r = getLocalBounds().reduced (20, 18);
    theme::label (g, st.keyLocked ? "Key" : "Detected key", r.withHeight (16));
    if (st.keyLocked)
    {
        const auto pill = juce::Rectangle<float> ((float) r.getX() + 38.0f, (float) r.getY() + 1.0f, 58.0f, 15.0f);
        g.setColour (a);
        g.fillRoundedRectangle (pill, 4.0f);
        g.setColour (theme::ink (proc.accent));
        g.setFont (theme::mono (10.0f));
        g.drawText ("LOCKED", pill, juce::Justification::centred);
    }

    // key name, shrunk to fit long names like "C# harmonic minor"
    const auto tonic = cg::pcName (A.key.tonic, A.flats) + " ";
    const juce::String mode = cg::modes()[(size_t) A.key.mode].name;
    float size = 46.0f;
    while (size > 22.0f && theme::textWidth (theme::ui (size, true), tonic + mode) > (float) r.getWidth()) size -= 2.0f;
    const auto fBold = theme::ui (size, true);
    const auto nameArea = juce::Rectangle<int> (r.getX(), r.getY() + 24, r.getWidth(), 58);
    g.setFont (fBold);
    g.setColour (theme::fg);
    g.drawText (tonic, nameArea, juce::Justification::centredLeft);
    g.setColour (theme::soft);
    g.drawText (mode, nameArea.withTrimmedLeft ((int) theme::textWidth (fBold, tonic)), juce::Justification::centredLeft);

    const int pct = cg::keyMatchPercent (st, A);
    const auto bar = juce::Rectangle<float> ((float) r.getX(), (float) r.getY() + 92.0f, (float) r.getWidth() - 50.0f, 7.0f);
    g.setColour (theme::line);
    g.fillRoundedRectangle (bar, 3.5f);
    g.setColour (a);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) pct / 100.0f), 3.5f);
    g.setColour (theme::fg);
    g.setFont (theme::mono (13.0f));
    g.drawText (juce::String (pct) + "%", juce::Rectangle<float> (bar.getRight(), bar.getY() - 6.0f, 50.0f, 19.0f), juce::Justification::centredRight);

    g.setColour (theme::muted);
    g.setFont (theme::ui (13.0f));
    g.drawText ("Alternatives", r.getX(), r.getY() + 104, r.getWidth(), 18, juce::Justification::centredLeft);
}

//==============================================================================
PianoRoll::PianoRoll (ChordGenEditor& e) : editor (e)
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

float PianoRoll::cellW() const
{
    const auto& st = editor.proc.state;
    return (float) getWidth() / (float) (st.bars * cg::timeSigs()[(size_t) st.ts].steps);
}

float PianoRoll::rowH() const { return (float) (getHeight() - head) / (float) (hi - lo + 1); }

void PianoRoll::paint (juce::Graphics& g)
{
    const auto& st = editor.proc.state;
    const auto& A = editor.proc.analysis;
    const auto& T = cg::timeSigs()[(size_t) st.ts];
    if (dragIndex < 0) { lo = A.rangeLo; hi = A.rangeHi; }

    const int cols = st.bars * T.steps;
    const float cw = cellW(), rh = rowH(), W = (float) getWidth(), H = (float) getHeight();
    const auto a = theme::accent (editor.proc.accent);

    juce::Path clip;
    clip.addRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    g.saveState();
    g.reduceClipRegion (clip);
    g.fillAll (theme::rollBg);

    for (int p = lo; p <= hi; ++p)
    {
        const float y = (float) head + (float) (hi - p) * rh;
        if (isBlack (p)) { g.setColour (juce::Colour (0xff0b0e14)); g.fillRect (0.0f, y, W, rh); }
        if (p % 12 == 0)
        {
            g.setColour (juce::Colour (0xff232a37));
            g.fillRect (0.0f, y + rh - 1.0f, W, 1.0f);
            g.setColour (juce::Colour (0xff4b5366));
            g.setFont (theme::mono (9.0f));
            g.drawText ("C" + juce::String (p / 12 - 1), juce::Rectangle<float> (3.0f, y, 30.0f, rh), juce::Justification::bottomLeft);
        }
    }

    g.setColour (a.withAlpha (0.07f));
    g.fillRect ((float) (st.selected * A.segSteps) * cw, (float) head, (float) A.segSteps * cw, H - (float) head);

    for (int s = 0; s <= cols; ++s)
    {
        const bool bar = s % T.steps == 0, beat = st.ts == 2 ? s % 3 == 0 : s % 2 == 0;
        g.setColour (juce::Colour (bar ? 0xff384154 : beat ? 0xff1f2531 : 0xff161b24));
        g.fillRect (std::round ((float) s * cw), (float) head, 1.0f, H - (float) head);
    }

    // chord names along the top
    g.setColour (theme::panel);
    g.fillRect (0.0f, 0.0f, W, (float) head);
    g.setFont (theme::ui (12.0f, true));
    for (int i = 0; i < A.nSeg; ++i)
    {
        const float x0 = (float) (i * A.segSteps) * cw, w = (float) A.segSteps * cw;
        if (i == st.selected) { g.setColour (a.withAlpha (0.18f)); g.fillRect (x0, 0.0f, w, (float) head); }
        g.setColour (theme::line);
        g.fillRect (x0, 0.0f, 1.0f, (float) head);
        g.setColour (i == st.selected ? a : theme::soft);
        g.drawText (cg::chordName (A.chordAt (i), A, st.complexity), juce::Rectangle<float> (x0 + 6.0f, 0.0f, w - 8.0f, (float) head),
                    juce::Justification::centredLeft, true);
    }
    g.setColour (theme::line);
    g.fillRect (0.0f, (float) head - 1.0f, W, 1.0f);

    auto drawNote = [&] (const cg::Note& n, juce::Colour c)
    {
        if (n.p < lo || n.p > hi) return;
        g.setColour (c);
        g.fillRoundedRectangle ((float) n.s * cw + 1.0f, (float) head + (float) (hi - n.p) * rh + 1.0f,
                                (float) n.d * cw - 2.0f, rh - 2.0f, 2.0f);
    };
    for (const auto& n : st.notes)
        drawNote (n, std::find (A.clash.begin(), A.clash.end(), n) != A.clash.end() ? theme::clash : a);
    for (const auto& n : editor.proc.pending)
        drawNote (n, a.withAlpha (0.35f));

    if (playing)
    {
        g.setColour (juce::Colours::white);
        g.fillRect ((float) playBeat * 2.0f * cw, 0.0f, 1.5f, H);
    }
    g.restoreState();
    g.setColour (theme::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 10.0f, 1.0f);
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    auto& st = editor.proc.state;
    const auto& A = editor.proc.analysis;
    const int cols = st.bars * cg::timeSigs()[(size_t) st.ts].steps;
    const int step = (int) std::floor (e.position.x / cellW());
    if (step < 0 || step >= cols) return;

    if (e.position.y < (float) head)
    {
        editor.selectSegment (juce::jmin (A.nSeg - 1, step / A.segSteps));
        return;
    }

    const int p = hi - (int) std::floor ((e.position.y - (float) head) / rowH());
    editor.proc.pushHistory();
    st.preset = {};
    st.source = 1;

    for (size_t i = 0; i < st.notes.size(); ++i)
    {
        const auto& n = st.notes[i];
        if (n.p == p && step >= n.s && step < n.s + n.d)
        {
            st.notes.erase (st.notes.begin() + (long) i);
            editor.proc.commit (true);
            return;
        }
    }
    st.notes.push_back ({ p, step, 1 });
    dragIndex = (int) st.notes.size() - 1;
    editor.audition (p);
    repaint();
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex < 0) return;
    auto& st = editor.proc.state;
    auto& n = st.notes[(size_t) dragIndex];
    const int cols = st.bars * cg::timeSigs()[(size_t) st.ts].steps;
    const int step = (int) std::floor (e.position.x / cellW());
    const int d = juce::jlimit (1, cols - n.s, step - n.s + 1);
    if (d != n.d) { n.d = d; repaint(); }
}

void PianoRoll::mouseUp (const juce::MouseEvent&)
{
    if (dragIndex < 0) return;
    dragIndex = -1;
    editor.proc.commit (true);
}

bool PianoRoll::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && (files[0].endsWithIgnoreCase (".mid") || files[0].endsWithIgnoreCase (".midi"));
}

void PianoRoll::filesDropped (const juce::StringArray& files, int, int)
{
    editor.importFile (juce::File (files[0]));
}

//==============================================================================
void LockButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto a = theme::accent (accent);
    if (over || locked)
    {
        g.setColour (locked ? a.withAlpha (0.5f) : theme::line2);
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
    }
    const auto c = r.getCentre();
    g.setColour (locked ? a : (over ? theme::fg : theme::dim));
    g.drawRoundedRectangle (c.x - 5.0f, c.y - 1.0f, 10.0f, 7.0f, 1.5f, 1.4f);
    juce::Path shackle;
    shackle.startNewSubPath (c.x - 3.0f, c.y - 1.0f);
    shackle.lineTo (c.x - 3.0f, c.y - 3.5f);
    shackle.addCentredArc (c.x, c.y - 3.5f, 3.0f, 3.0f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi);
    if (locked) shackle.lineTo (c.x + 3.0f, c.y - 1.0f);
    g.strokePath (shackle, juce::PathStrokeType (1.4f));
}

//==============================================================================
ChordCard::ChordCard (ChordGenEditor& e, int i) : editor (e), index (i)
{
    for (int k = 0; k < 3; ++k)
    {
        styleSmall (alts[k], 12.0f);
        alts[k].setTooltip ("Swap in and lock");
        alts[k].onClick = [this, k]
        {
            if (k >= (int) altCodes.size()) return;
            const int code = altCodes[(size_t) k], seg = index;
            editor.edit ([code, seg] (cg::State& s) { s.prog[(size_t) seg] = code; s.locks[seg] = code; s.selected = seg; }, false);
        };
        addAndMakeVisible (alts[k]);
    }
    lock.onClick = [this]
    {
        const int seg = index;
        const int code = editor.proc.state.prog[(size_t) seg];
        editor.edit ([seg, code] (cg::State& s)
        {
            if (s.locks.count (seg)) s.locks.erase (seg);
            else s.locks[seg] = code;
        }, false);
    };
    addAndMakeVisible (lock);
}

void ChordCard::refresh()
{
    const auto& A = editor.proc.analysis;
    const auto& st = editor.proc.state;
    altCodes.clear();
    const auto alt = cg::alternatives (st, A, index);
    for (int k = 0; k < 3; ++k)
    {
        const bool has = k < (int) alt.size();
        alts[k].setVisible (has);
        if (has)
        {
            const auto& c = A.chords[(size_t) alt[(size_t) k]];
            altCodes.push_back (c.code());
            alts[k].setButtonText (cg::chordName (c, A, st.complexity));
        }
    }
    lock.locked = st.locks.count (index) > 0;
    lock.accent = editor.proc.accent;
    lock.setTooltip (lock.locked ? "Locked: Regenerate keeps this chord" : "Lock this chord");
    resized();
    repaint();
}

void ChordCard::resized()
{
    auto r = getLocalBounds().reduced (14, 12);
    lock.setBounds (r.getRight() - 62, r.getY() - 4, 26, 26);
    int x = r.getX();
    const int y = r.getY() + 92;
    for (auto& b : alts)
    {
        if (! b.isVisible()) continue;
        const int w = (int) theme::textWidth (theme::ui (12.0f, true), b.getButtonText()) + 18;
        if (x + w > r.getRight()) { b.setVisible (false); continue; }
        b.setBounds (x, y, w, 24);
        x += w + 5;
    }
}

void ChordCard::paint (juce::Graphics& g)
{
    const auto& A = editor.proc.analysis;
    const auto& st = editor.proc.state;
    const auto a = theme::accent (editor.proc.accent);
    const bool sel = st.selected == index;
    const auto& c = A.chordAt (index);
    const auto& f = A.fitAt (index);
    const auto box = getLocalBounds().toFloat().reduced (2.0f);

    if (sel)
    {
        g.setColour (a.withAlpha (0.18f));
        g.fillRoundedRectangle (box.expanded (2.0f), 16.0f);
    }
    g.setColour (sel ? juce::Colour (0xff162029) : theme::panel);
    g.fillRoundedRectangle (box, 14.0f);
    g.setColour (sel ? a : theme::line);
    g.drawRoundedRectangle (box, 14.0f, sel ? 1.5f : 1.0f);

    auto r = getLocalBounds().reduced (16, 12);
    g.setFont (theme::ui (13.0f));
    g.setColour (theme::muted);
    g.drawText (cg::segmentLabel (index, st.rhythm), r.withHeight (18), juce::Justification::centredLeft);
    g.setFont (theme::mono (13.0f));
    g.setColour (theme::soft);
    g.drawText (cg::roman (c, A), r.withHeight (18), juce::Justification::centredRight);

    g.setColour (theme::fg);
    g.setFont (theme::ui (32.0f, true));
    g.drawFittedText (cg::chordName (c, A, st.complexity), r.getX(), r.getY() + 24, r.getWidth(), 40, juce::Justification::centredLeft, 1, 0.6f);

    g.setFont (theme::mono (12.0f));
    g.setColour (f.pct >= 0 && f.pct < 50 ? theme::clash : theme::muted);
    g.drawText (f.pct < 0 ? juce::String ("no melody") : "fits " + juce::String (f.pct) + "%", r.getX(), r.getY() + 68, r.getWidth(), 16, juce::Justification::centredLeft);
    if (c.borrowed)
    {
        g.setColour (theme::chord (editor.proc.accent));
        g.setFont (theme::mono (10.5f));
        g.drawText (c.sec >= 0 ? "sec. dom" : "borrowed", r.getX(), r.getY() + 68, r.getWidth(), 16, juce::Justification::centredRight);
    }

    if (playing)
    {
        g.setColour (a);
        g.fillRoundedRectangle (14.0f, (float) getHeight() - 4.0f, (float) getWidth() - 28.0f, 3.0f, 1.5f);
    }
}

void ChordCard::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked())
        editor.selectSegment (index);
}

//==============================================================================
Keyboard::Keyboard (ChordGenEditor& e) : editor (e) {}

juce::Rectangle<float> Keyboard::keyBounds (int note) const
{
    int whites = 0, whiteIndex = 0;
    for (int m = lowNote; m <= highNote; ++m)
        if (! isBlack (m)) { if (m < note) ++whiteIndex; ++whites; }
    const float ww = (float) getWidth() / (float) whites;
    if (! isBlack (note))
        return { (float) whiteIndex * ww, 0.0f, ww, (float) getHeight() };
    return { (float) whiteIndex * ww - ww * 0.3f, 0.0f, ww * 0.6f, (float) getHeight() * 0.62f };
}

int Keyboard::noteAt (juce::Point<float> p) const
{
    for (int m = lowNote; m <= highNote; ++m)
        if (isBlack (m) && keyBounds (m).contains (p)) return m;
    for (int m = lowNote; m <= highNote; ++m)
        if (! isBlack (m) && keyBounds (m).contains (p)) return m;
    return -1;
}

void Keyboard::paint (juce::Graphics& g)
{
    const auto& A = editor.proc.analysis;
    const auto& st = editor.proc.state;
    const int seg = juce::jlimit (0, A.nSeg - 1, segment);
    const auto& v = A.voicing[(size_t) seg];
    std::vector<int> mel, cl;
    for (const auto& it : A.items[(size_t) seg]) mel.push_back (it.n.p);
    for (const auto& n : cg::clashes (st, A, A.chordAt (seg), seg)) cl.push_back (n.p);
    auto has = [] (const std::vector<int>& v2, int m) { return std::find (v2.begin(), v2.end(), m) != v2.end(); };
    const auto a = theme::accent (editor.proc.accent), ch = theme::chord (editor.proc.accent);

    juce::Path clip;
    clip.addRoundedRectangle (getLocalBounds().toFloat(), 12.0f);
    g.saveState();
    g.reduceClipRegion (clip);

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int m = lowNote; m <= highNote; ++m)
        {
            if (isBlack (m) != (pass == 1)) continue;
            const auto r = keyBounds (m);
            const bool isChord = has (v.notes, m), isMel = has (mel, m), isClash = has (cl, m);
            auto c = pass == 0 ? juce::Colour (0xffeef1f6) : juce::Colour (0xff1b1f29);
            if (isChord) c = ch;
            if (isMel) c = a;
            if (isClash) c = theme::clash;
            g.setColour (c);
            if (pass == 0)
            {
                g.fillRect (r);
                g.setColour (juce::Colour (0xffc9ceda));
                g.fillRect (r.getRight() - 1.0f, 0.0f, 1.0f, r.getHeight());
                if (m % 12 == 0)
                {
                    g.setColour (juce::Colour (0xff8b93a5));
                    g.setFont (theme::mono (9.0f));
                    g.drawText ("C" + juce::String (m / 12 - 1), r.withTrimmedBottom (6.0f), juce::Justification::centredBottom);
                }
            }
            else
                g.fillRoundedRectangle (r.withTrimmedTop (-4.0f), 4.0f);

            if (isMel && isChord && ! isClash)
            {
                g.setColour (ch);
                g.fillRoundedRectangle (r.getX() + r.getWidth() * 0.2f, r.getBottom() - 13.0f, r.getWidth() * 0.6f, 5.0f, 2.5f);
            }
        }
    }
    g.restoreState();
    g.setColour (theme::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 12.0f, 1.0f);
}

void Keyboard::mouseDown (const juce::MouseEvent& e)
{
    const int m = noteAt (e.position);
    if (m >= 0) editor.audition (m);
}

//==============================================================================
void Toast::show (const juce::String& t)
{
    text = t;
    setVisible (true);
    toFront (false);
    repaint();
    startTimer (2600);
}

void Toast::paint (juce::Graphics& g)
{
    g.setColour (theme::panel2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
    g.setColour (theme::line2);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 10.0f, 1.0f);
    g.setColour (theme::fg);
    g.setFont (theme::ui (13.0f));
    g.drawText (text, getLocalBounds(), juce::Justification::centred, true);
}

//==============================================================================
ChordGenEditor::ChordGenEditor (ChordGenProcessor& p)
    : AudioProcessorEditor (p), proc (p), keyPanel (p), roll (*this), keyboard (*this)
{
    lnf.setAccent (proc.accent);
    setLookAndFeel (&lnf);

    // header
    for (auto* t : { &tabHost, &tabDraw, &tabMidi, &tabAudio, &tabMic })
    {
        t->getProperties().set ("kind", "flat");
        t->getProperties().set ("fontSize", 14.5f);
        addAndMakeVisible (t);
    }
    tabHost.setTooltip ("Melody from the FL piano roll (Capture)");
    tabDraw.setTooltip ("Draw the melody in the roll below");
    tabMidi.setTooltip ("Load a melody from a .mid file");
    tabAudio.setEnabled (false);
    tabMic.setEnabled (false);
    tabAudio.setTooltip ("Coming in Phase 2: melody from an audio track");
    tabMic.setTooltip ("Coming in Phase 2: melody from a microphone");
    tabHost.onClick = [this] { proc.state.source = 0; refresh(); };
    tabDraw.onClick = [this] { proc.state.source = 1; refresh(); };
    tabMidi.onClick = [this] { chooseImport(); };

    styleSmall (captureButton, 13.0f);
    captureButton.setTooltip ("Collect the melody each time FL plays the pattern");
    captureButton.onClick = [this] { proc.setOption ("capture", ! proc.optCapture); };
    addAndMakeVisible (captureButton);

    accentDots.onPick = [this] (int i)
    {
        proc.accent = i;
        lnf.setAccent (i);
        sendLookAndFeelChange();
        refresh();
    };
    addAndMakeVisible (accentDots);

    // key + melody input
    addAndMakeVisible (keyPanel);
    presetBox.addItem ("Custom melody", 1);
    presetBox.setItemEnabled (1, false);
    const auto names = cg::presetNames();
    for (int i = 0; i < names.size(); ++i) presetBox.addItem (names[i], i + 2);
    presetBox.setTooltip ("Load an example melody");
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id < 2) return;
        const auto name = cg::presetNames()[id - 2];
        edit ([name] (cg::State& s) { cg::loadPreset (s, name); s.source = 1; });
    };
    for (int b : { 4, 8, 16 }) barsBox.addItem (juce::String (b) + " bars", b);
    barsBox.setTooltip ("Length of the loop; match your FL pattern");
    barsBox.onChange = [this]
    {
        const int b = barsBox.getSelectedId();
        edit ([b] (cg::State& s)
        {
            const int max = b * cg::timeSigs()[(size_t) s.ts].steps;
            s.bars = b;
            std::vector<cg::Note> kept;
            for (auto n : s.notes) if (n.s < max) { n.d = std::min (n.d, max - n.s); kept.push_back (n); }
            s.notes = kept;
            s.locks.clear();
        });
    };
    for (int i = 0; i < (int) cg::timeSigs().size(); ++i) tsBox.addItem (cg::timeSigs()[(size_t) i].name, i + 1);
    tsBox.onChange = [this]
    {
        const int ts = tsBox.getSelectedId() - 1;
        edit ([ts] (cg::State& s)
        {
            s.ts = ts;
            const int max = s.bars * cg::timeSigs()[(size_t) ts].steps;
            std::vector<cg::Note> kept;
            for (auto n : s.notes) if (n.s < max) { n.d = std::min (n.d, max - n.s); kept.push_back (n); }
            s.notes = kept;
            s.locks.clear();
            s.preset = {};
        });
    };
    styleSmall (dupButton);
    dupButton.setTooltip ("Copy the melody to double the length");
    dupButton.onClick = [this]
    {
        if (proc.state.bars >= 16) { toast.show ("16 bars is the maximum."); return; }
        edit ([] (cg::State& s)
        {
            const int len = s.bars * cg::timeSigs()[(size_t) s.ts].steps;
            const auto copy = s.notes;
            for (auto n : copy) { n.s += len; s.notes.push_back (n); }
            s.bars *= 2;
            s.locks.clear();
        });
    };
    styleSmall (clearButton);
    clearButton.onClick = [this]
    {
        edit ([] (cg::State& s) { s.notes.clear(); s.locks.clear(); s.preset = {}; s.keyLocked = false; });
    };
    styleSmall (importButton);
    importButton.onClick = [this] { chooseImport(); };
    for (auto* c : { (juce::Component*) &presetBox, (juce::Component*) &barsBox, (juce::Component*) &tsBox,
                     (juce::Component*) &dupButton, (juce::Component*) &clearButton, (juce::Component*) &importButton,
                     (juce::Component*) &roll })
        addAndMakeVisible (c);

    // progression header
    rhythmBox.addItem ("2 chords / bar", 1);
    rhythmBox.addItem ("1 chord / bar", 2);
    rhythmBox.addItem ("1 chord / 2 bars", 3);
    rhythmBox.onChange = [this]
    {
        const double r = rhythmBox.getSelectedId() == 1 ? 2.0 : rhythmBox.getSelectedId() == 3 ? 0.5 : 1.0;
        edit ([r] (cg::State& s) { s.rhythm = r; s.locks.clear(); s.selected = 0; });
    };
    favBox.setTextWhenNothingSelected ("Saved progressions");
    favBox.setTextWhenNoChoicesAvailable ("Nothing saved yet");
    favBox.onChange = [this]
    {
        const int id = favBox.getSelectedId();
        if (id == 1000) proc.clearFavorites();
        else if (id > 0) proc.restoreFavorite (id - 1);
    };
    for (auto* b : { &undoButton, &redoButton, &regenButton, &saveFavButton, &prevButton, &nextButton }) styleSmall (*b, 13.0f);
    undoButton.setTooltip ("Undo (Cmd/Ctrl+Z)");
    redoButton.setTooltip ("Redo (Shift+Cmd/Ctrl+Z)");
    regenButton.setTooltip ("New ideas; locked chords stay");
    saveFavButton.setTooltip ("Save this progression");
    undoButton.onClick = [this] { proc.undo(); };
    redoButton.onClick = [this] { proc.redo(); };
    regenButton.onClick = [this] { proc.regenerate(); };
    saveFavButton.onClick = [this] { proc.saveFavorite(); };
    prevButton.getProperties().set ("radius", 10.0f);
    nextButton.getProperties().set ("radius", 10.0f);
    prevButton.onClick = [this]
    {
        const int n = (int) proc.analysis.sugs.size();
        edit ([n] (cg::State& s) { s.sugIdx = (s.sugIdx - 1 + n) % n; });
    };
    nextButton.onClick = [this]
    {
        const int n = (int) proc.analysis.sugs.size();
        edit ([n] (cg::State& s) { s.sugIdx = (s.sugIdx + 1) % n; });
    };
    for (auto* c : { (juce::Component*) &rhythmBox, (juce::Component*) &favBox, (juce::Component*) &undoButton,
                     (juce::Component*) &redoButton, (juce::Component*) &regenButton, (juce::Component*) &saveFavButton,
                     (juce::Component*) &prevButton, (juce::Component*) &nextButton })
        addAndMakeVisible (c);

    cardsView.setViewedComponent (&cardsArea, false);
    cardsView.setScrollBarsShown (true, false);
    cardsView.setScrollBarThickness (8);
    addAndMakeVisible (cardsView);
    addAndMakeVisible (keyboard);

    // control bar
    complexity.setSliderStyle (juce::Slider::LinearHorizontal);
    complexity.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    complexity.setRange (0, 3, 1);
    complexity.onValueChange = [this]
    {
        const int v = (int) complexity.getValue();
        if (v != proc.state.complexity) edit ([v] (cg::State& s) { s.complexity = v; }, false);
    };
    addAndMakeVisible (complexity);
    for (int i = 0; i < cg::numStyles; ++i)
    {
        auto& b = styleButtons[i];
        b.setButtonText (cg::styleInfo ((cg::Style) i).label);
        b.getProperties().set ("kind", "style");
        b.getProperties().set ("radius", 10.0f);
        b.getProperties().set ("fontSize", 14.0f);
        b.onClick = [this, i]
        {
            edit ([i] (cg::State& s) { s.style = (cg::Style) i; s.complexity = cg::styleInfo ((cg::Style) i).defaultComplexity; s.sugIdx = 0; });
        };
        addAndMakeVisible (b);
    }
    voicingBox.addItem ("Smooth voice leading", 1);
    voicingBox.addItem ("Root position", 2);
    voicingBox.onChange = [this]
    {
        const bool root = voicingBox.getSelectedId() == 2;
        edit ([root] (cg::State& s) { s.rootVoicing = root; }, false);
    };
    addAndMakeVisible (voicingBox);
    styleSmall (octDown, 15.0f);
    styleSmall (octUp, 15.0f);
    octDown.setTooltip ("Chords an octave lower");
    octUp.setTooltip ("Chords an octave higher");
    octDown.onClick = [this] { if (proc.state.octave > -1) edit ([] (cg::State& s) { --s.octave; }, false); };
    octUp.onClick = [this] { if (proc.state.octave < 1) edit ([] (cg::State& s) { ++s.octave; }, false); };
    addAndMakeVisible (octDown);
    addAndMakeVisible (octUp);

    bassToggle.onClick = [this] { const bool v = bassToggle.getToggleState(); edit ([v] (cg::State& s) { s.bass = v; }, false); };
    borrowToggle.onClick = [this] { const bool v = borrowToggle.getToggleState(); edit ([v] (cg::State& s) { s.borrowed = v; }); };
    melodyToggle.onClick = [this] { const bool v = melodyToggle.getToggleState(); edit ([v] (cg::State& s) { s.playMelody = v; }, false); };
    rhythmMidiToggle.onClick = [this] { const bool v = rhythmMidiToggle.getToggleState(); edit ([v] (cg::State& s) { s.patternMidi = v; }, false); };
    followToggle.onClick = [this] { proc.setOption ("follow", followToggle.getToggleState()); };
    internalToggle.onClick = [this] { proc.setOption ("internal", internalToggle.getToggleState()); };
    thruToggle.onClick = [this] { proc.setOption ("thru", thruToggle.getToggleState()); };
    followToggle.setTooltip ("Play the chords while FL plays");
    internalToggle.setTooltip ("Turn off to send only MIDI to another synth");
    thruToggle.setTooltip ("Pass the incoming melody through to the output");
    borrowToggle.setTooltip ("Allow chords from outside the key, like the major V in minor");
    for (auto* t : { &bassToggle, &borrowToggle, &melodyToggle, &rhythmMidiToggle, &followToggle, &internalToggle, &thruToggle })
        addAndMakeVisible (t);

    for (auto* b : { &previewButton, &saveMidiButton })
    {
        b->getProperties().set ("fontSize", 15.0f);
        b->getProperties().set ("radius", 12.0f);
        b->getProperties().set ("kind", "panel");
        addAndMakeVisible (b);
    }
    previewButton.setTooltip ("Loop the progression while FL is stopped (Space)");
    previewButton.onClick = [this] { proc.setOption ("preview", ! proc.optPreview); };
    saveMidiButton.onClick = [this] { saveMidi(); };
    addAndMakeVisible (dragButton);
    addChildComponent (toast);

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (1200, 900, 1900, 1400);
    setSize (1220, 900);

    proc.addChangeListener (this);
    refresh();
    startTimerHz (20);
}

ChordGenEditor::~ChordGenEditor()
{
    stopTimer();
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void ChordGenEditor::edit (std::function<void (cg::State&)> change, bool resuggest)
{
    proc.pushHistory();
    change (proc.state);
    proc.commit (resuggest);
}

void ChordGenEditor::selectSegment (int seg)
{
    proc.state.selected = juce::jlimit (0, proc.analysis.nSeg - 1, seg);
    for (auto& c : cards) c->repaint();
    if (seg < (int) cards.size())
    {
        const auto b = cards[(size_t) seg]->getBounds();
        if (b.getY() < cardsView.getViewPositionY() || b.getBottom() > cardsView.getViewPositionY() + cardsView.getHeight())
            cardsView.setViewPosition (0, b.getY());
    }
    keyboard.segment = playSeg >= 0 ? playSeg : proc.state.selected;
    keyboard.repaint();
    roll.repaint();
    repaint (explainArea);
    repaint (kbInfoArea);
}

void ChordGenEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void ChordGenEditor::refresh()
{
    const auto& st = proc.state;

    tabHost.setToggleState (st.source == 0, juce::dontSendNotification);
    tabDraw.setToggleState (st.source == 1, juce::dontSendNotification);
    tabMidi.setToggleState (st.source == 2, juce::dontSendNotification);
    captureButton.setButtonText (proc.optCapture ? "Capture on" : "Capture off");
    captureButton.setToggleState (proc.optCapture, juce::dontSendNotification);
    accentDots.selected = proc.accent;
    accentDots.repaint();

    keyPanel.refresh();
    const int presetIndex = cg::presetNames().indexOf (st.preset);
    presetBox.setSelectedId (presetIndex >= 0 ? presetIndex + 2 : 1, juce::dontSendNotification);
    barsBox.setSelectedId (st.bars, juce::dontSendNotification);
    tsBox.setSelectedId (st.ts + 1, juce::dontSendNotification);
    rhythmBox.setSelectedId (st.rhythm > 1.5 ? 1 : st.rhythm < 0.75 ? 3 : 2, juce::dontSendNotification);

    favBox.clear (juce::dontSendNotification);
    const auto favs = proc.favoriteNames();
    for (int i = 0; i < favs.size(); ++i) favBox.addItem (favs[i], i + 1);
    if (! favs.isEmpty()) { favBox.addSeparator(); favBox.addItem ("Clear saved", 1000); }

    undoButton.setEnabled (proc.canUndo());
    redoButton.setEnabled (proc.canRedo());

    if ((int) cards.size() != proc.analysis.nSeg) rebuildCards();
    for (auto& c : cards) c->refresh();

    complexity.setValue (st.complexity, juce::dontSendNotification);
    for (int i = 0; i < cg::numStyles; ++i) styleButtons[i].setToggleState ((int) st.style == i, juce::dontSendNotification);
    voicingBox.setSelectedId (st.rootVoicing ? 2 : 1, juce::dontSendNotification);
    bassToggle.setToggleState (st.bass, juce::dontSendNotification);
    borrowToggle.setToggleState (st.borrowed, juce::dontSendNotification);
    melodyToggle.setToggleState (st.playMelody, juce::dontSendNotification);
    rhythmMidiToggle.setToggleState (st.patternMidi, juce::dontSendNotification);
    followToggle.setToggleState (proc.optFollow, juce::dontSendNotification);
    internalToggle.setToggleState (proc.optInternal, juce::dontSendNotification);
    thruToggle.setToggleState (proc.optThru, juce::dontSendNotification);
    previewButton.setButtonText (proc.optPreview ? "Stop" : "Preview");
    previewButton.setToggleState (proc.optPreview, juce::dontSendNotification);

    dragButton.accent = proc.accent;
    writeDragFile();
    dragButton.repaint();

    keyboard.segment = playSeg >= 0 ? playSeg : st.selected;
    roll.repaint();
    keyboard.repaint();
    repaint();

    if (proc.toastId != lastToastId)
    {
        lastToastId = proc.toastId;
        const int w = (int) theme::textWidth (theme::ui (13.0f), proc.toastText) + 40;
        toast.setBounds ((getWidth() - w) / 2, getHeight() - 64, w, 40);
        toast.show (proc.toastText);
    }
}

void ChordGenEditor::rebuildCards()
{
    cards.clear();
    for (int i = 0; i < proc.analysis.nSeg; ++i)
    {
        cards.push_back (std::make_unique<ChordCard> (*this, i));
        cardsArea.addAndMakeVisible (*cards.back());
    }
    resized();
}

void ChordGenEditor::updatePlaying()
{
    const auto info = proc.getHostInfo();
    const auto& A = proc.analysis;
    const int seg = info.active ? juce::jmin (A.nSeg - 1, (int) std::floor (info.loopPos * 2.0 / A.segSteps)) : -1;
    roll.playing = info.active;
    roll.playBeat = info.loopPos;
    if (info.active) roll.repaint();
    if (seg != playSeg)
    {
        playSeg = seg;
        for (size_t i = 0; i < cards.size(); ++i)
        {
            cards[i]->playing = (int) i == playSeg;
            cards[i]->repaint();
        }
        keyboard.segment = playSeg >= 0 ? playSeg : proc.state.selected;
        keyboard.repaint();
        roll.repaint();
        repaint (kbInfoArea);
    }

    const int bpm = (int) std::lround (info.bpm);
    juce::String s = info.hostPlaying ? (proc.optCapture ? "Listening" : "Playing") : proc.optPreview ? "Preview" : "Ready";
    s += " " + dot + " " + juce::String (bpm) + " BPM";
    if (s != statusText || info.active != statusLive)
    {
        statusText = s;
        statusLive = info.active;
        repaint (headerArea);
    }
}

void ChordGenEditor::timerCallback()
{
    updatePlaying();

    // Development aid: CHORDGEN_SNAPSHOT=/path/shot.png saves a picture of the editor once.
    static bool snapshotTaken = false;
    if (! snapshotTaken && ++ticks > 30)
    {
        snapshotTaken = true;
        const auto path = juce::SystemStats::getEnvironmentVariable ("CHORDGEN_SNAPSHOT", {});
        if (path.isNotEmpty())
        {
            juce::FileOutputStream out { juce::File (path) };
            if (out.openedOk())
            {
                out.setPosition (0);
                out.truncate();
                juce::PNGImageFormat().writeImageToStream (createComponentSnapshot (getLocalBounds(), true, 1.0f), out);
            }
        }
    }
}

void ChordGenEditor::writeDragFile()
{
    const auto data = cg::chordsMidi (proc.state, proc.analysis, proc.getHostInfo().bpm);
    if (data == lastDragData && dragButton.file.existsAsFile()) return;
    lastDragData = data;
    for (const auto& old : midiTempFolder().findChildFiles (juce::File::findFiles, false, "*.mid"))
        old.deleteFile();
    const auto file = midiTempFolder().getChildFile (juce::File::createLegalFileName (cg::midiFileBase (proc.state, proc.analysis)) + ".mid");
    if (file.replaceWithData (data.getData(), data.getSize()))
        dragButton.file = file;
}

void ChordGenEditor::importFile (const juce::File& f)
{
    const auto r = cg::importMidi (f);
    if (r.error.isNotEmpty()) { toast.show ("Could not read that file: " + r.error); return; }
    edit ([r] (cg::State& s)
    {
        s.notes = r.notes; s.ts = r.ts; s.bars = r.bars;
        s.keyLocked = false; s.locks.clear(); s.preset = {}; s.source = 2; s.selected = 0;
    });
    proc.showToast ("Imported " + juce::String ((int) r.notes.size()) + " notes from " + f.getFileName());
}

void ChordGenEditor::chooseImport()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a melody", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) importFile (f);
                          });
}

void ChordGenEditor::saveMidi()
{
    const auto data = cg::fullMidi (proc.state, proc.analysis, proc.getHostInfo().bpm);
    const auto name = juce::File::createLegalFileName (cg::midiFileBase (proc.state, proc.analysis)) + ".mid";
    chooser = std::make_unique<juce::FileChooser> ("Save MIDI file", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (name), "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, data] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f != juce::File {} && f.replaceWithData (data.getData(), data.getSize()))
                                  proc.showToast ("Saved " + f.getFileName());
                          });
}

bool ChordGenEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z'))
    {
        if (mods.isShiftDown()) proc.redo(); else proc.undo();
        return true;
    }
    if (key == juce::KeyPress::spaceKey) { proc.setOption ("preview", ! proc.optPreview); return true; }
    if (key == juce::KeyPress::rightKey) { selectSegment (proc.state.selected + 1); return true; }
    if (key == juce::KeyPress::leftKey) { selectSegment (proc.state.selected - 1); return true; }
    return false;
}

//==============================================================================
void ChordGenEditor::resized()
{
    auto r = getLocalBounds().reduced (16, 0);

    // header
    headerArea = r.removeFromTop (64);
    {
        auto h = headerArea.reduced (0, 14);
        accentDots.setBounds (h.removeFromRight (84));
        h.removeFromRight (190); // status text
        captureButton.setBounds (h.removeFromRight (104).reduced (0, 4));
        const int widths[] = { 124, 66, 90, 82, 66 };
        juce::TextButton* tabs[] = { &tabHost, &tabDraw, &tabMidi, &tabAudio, &tabMic };
        const int total = 124 + 66 + 90 + 82 + 66 + 8;
        int x = juce::jlimit (260, juce::jmax (260, h.getRight() - total - 16), getWidth() / 2 - total / 2);
        for (int i = 0; i < 5; ++i) { tabs[i]->setBounds (x + 4, h.getY() + 4, widths[i], h.getHeight() - 8); x += widths[i]; }
    }

    r.removeFromTop (14);
    const int extra = juce::jmax (0, getHeight() - 900);
    auto top = r.removeFromTop (250 + extra);
    keyPanel.setBounds (top.removeFromLeft (330));
    top.removeFromLeft (16);
    rollPanelArea = top;
    {
        auto rp = top.reduced (18, 14);
        auto title = rp.removeFromTop (28);
        importButton.setBounds (title.removeFromRight (100));
        title.removeFromRight (6);
        clearButton.setBounds (title.removeFromRight (60));
        title.removeFromRight (6);
        dupButton.setBounds (title.removeFromRight (86));
        title.removeFromRight (6);
        tsBox.setBounds (title.removeFromRight (70));
        title.removeFromRight (6);
        barsBox.setBounds (title.removeFromRight (88));
        title.removeFromRight (6);
        presetBox.setBounds (title.removeFromRight (156));
        rp.removeFromTop (10);
        rp.removeFromBottom (22); // hint
        roll.setBounds (rp);
    }

    r.removeFromTop (16);
    progHeadArea = r.removeFromTop (36);
    {
        auto ph = progHeadArea;
        ph.removeFromLeft (230);
        rhythmBox.setBounds (ph.removeFromLeft (150).reduced (0, 3));
        nextButton.setBounds (ph.removeFromRight (36));
        ph.removeFromRight (160); // "Suggestion 1 / 5"
        prevButton.setBounds (ph.removeFromRight (36));
        ph.removeFromRight (14);
        favBox.setBounds (ph.removeFromRight (180).reduced (0, 3));
        ph.removeFromRight (8);
        saveFavButton.setBounds (ph.removeFromRight (64).reduced (0, 3));
        ph.removeFromRight (8);
        regenButton.setBounds (ph.removeFromRight (106).reduced (0, 3));
        ph.removeFromRight (8);
        redoButton.setBounds (ph.removeFromRight (62).reduced (0, 3));
        ph.removeFromRight (8);
        undoButton.setBounds (ph.removeFromRight (62).reduced (0, 3));
    }

    r.removeFromTop (10);
    cardsView.setBounds (r.removeFromTop (158));
    {
        const int n = (int) cards.size();
        if (n > 0)
        {
            int vw = cardsView.getWidth();
            const int maxCols = juce::jmax (1, (vw + 12) / 182);
            if (n > maxCols) vw -= cardsView.getScrollBarThickness() + 4;
            int cols = juce::jmin (n, maxCols);
            const int rows = (n + cols - 1) / cols;
            cols = (n + rows - 1) / rows;
            const int cw = (vw - (cols - 1) * 12) / cols, chh = 152;
            cardsArea.setSize (vw, rows * chh + (rows - 1) * 12);
            for (int k = 0; k < n; ++k)
                cards[(size_t) k]->setBounds ((k % cols) * (cw + 12), (k / cols) * (chh + 12), cw, chh);
        }
    }

    r.removeFromTop (6);
    explainArea = r.removeFromTop (40);
    r.removeFromTop (8);
    keyboard.setBounds (r.removeFromTop (120));
    kbInfoArea = r.removeFromTop (28);
    r.removeFromTop (8);
    controlsArea = r.removeFromTop (118);
    {
        auto c = controlsArea.withTrimmedTop (14);
        auto rowA = c.removeFromTop (56);
        c.removeFromTop (10);
        auto rowB = c.removeFromTop (44);

        auto g1 = rowA.removeFromLeft (210);
        complexityLabel = g1.removeFromTop (18);
        complexity.setBounds (g1.reduced (0, 4));
        rowA.removeFromLeft (24);

        auto g2 = rowA.removeFromLeft (4 * 72 + 3 * 6);
        styleLabel = g2.removeFromTop (18);
        g2.removeFromTop (2);
        for (auto& b : styleButtons) { b.setBounds (g2.removeFromLeft (72)); g2.removeFromLeft (6); }
        rowA.removeFromLeft (24);

        auto g3 = rowA.removeFromLeft (180 + 8 + 30 + 32 + 30);
        voicingLabel = g3.removeFromTop (18);
        g3.removeFromTop (2);
        voicingBox.setBounds (g3.removeFromLeft (180));
        g3.removeFromLeft (8);
        octDown.setBounds (g3.removeFromLeft (30));
        octValueArea = g3.removeFromLeft (32);
        octUp.setBounds (g3.removeFromLeft (30));

        saveMidiButton.setBounds (rowA.removeFromRight (130).withTrimmedTop (16));
        rowA.removeFromRight (10);
        previewButton.setBounds (rowA.removeFromRight (130).withTrimmedTop (16));

        dragButton.setBounds (rowB.removeFromRight (270));
        rowB.removeFromRight (16);
        auto t1 = rowB.removeFromTop (22), t2 = rowB;
        for (auto* t : { &bassToggle, &borrowToggle, &melodyToggle, &rhythmMidiToggle }) t->setBounds (t1.removeFromLeft (172));
        for (auto* t : { &followToggle, &internalToggle, &thruToggle }) t->setBounds (t2.removeFromLeft (172));
    }

    if (toast.isVisible())
        toast.setBounds (toast.getBounds().withPosition ((getWidth() - toast.getWidth()) / 2, getHeight() - 64));
}

void ChordGenEditor::paint (juce::Graphics& g)
{
    const auto& st = proc.state;
    const auto& A = proc.analysis;
    const auto a = theme::accent (proc.accent);
    g.fillAll (theme::bg);

    // header: logo, tab strip, status
    {
        auto h = headerArea.reduced (0, 14);
        const auto logo = juce::Rectangle<float> ((float) h.getX(), (float) h.getCentreY() - 16.0f, 32.0f, 32.0f);
        g.setColour (a);
        g.drawRoundedRectangle (logo.reduced (1.25f), 8.0f, 2.5f);
        for (int i = 0; i < 3; ++i)
        {
            const float x = logo.getX() + 10.0f + (float) i * 6.0f, len = i == 1 ? 15.0f : i == 0 ? 11.0f : 8.0f;
            g.drawLine (x, logo.getCentreY() - len / 2, x, logo.getCentreY() + len / 2, 2.5f);
        }
        g.setFont (theme::ui (22.0f, true));
        const float chordW = theme::textWidth (theme::ui (22.0f, true), "Chord");
        g.setColour (theme::fg);
        g.drawText ("Chord", (int) logo.getRight() + 10, h.getY(), 80, h.getHeight(), juce::Justification::centredLeft);
        g.setColour (a);
        g.drawText ("Gen", (int) (logo.getRight() + 10.0f + chordW), h.getY(), 60, h.getHeight(), juce::Justification::centredLeft);
        const auto ver = juce::Rectangle<float> (logo.getRight() + 14.0f + chordW + theme::textWidth (theme::ui (22.0f, true), "Gen"), (float) h.getCentreY() - 10.0f, 44.0f, 20.0f);
        g.setColour (theme::line2);
        g.drawRoundedRectangle (ver, 6.0f, 1.0f);
        g.setColour (theme::muted);
        g.setFont (theme::mono (11.0f));
        g.drawText ("v0.3", ver, juce::Justification::centred);

        const auto tabsBox = tabHost.getBounds().getUnion (tabMic.getBounds()).expanded (4).toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (tabsBox, 12.0f);
        g.setColour (theme::line);
        g.drawRoundedRectangle (tabsBox.reduced (0.5f), 12.0f, 1.0f);

        auto status = juce::Rectangle<int> (captureButton.getRight() + 12, h.getY(), 178, h.getHeight());
        g.setColour (statusLive ? theme::ok : theme::dim);
        g.fillEllipse ((float) status.getX(), (float) status.getCentreY() - 4.5f, 9.0f, 9.0f);
        g.setColour (theme::muted);
        g.setFont (theme::ui (14.0f));
        g.drawText (statusText, status.withTrimmedLeft (16), juce::Justification::centredLeft);

        g.setColour (theme::line);
        g.fillRect (0, headerArea.getBottom() - 1, getWidth(), 1);
    }

    // melody panel
    {
        const auto box = rollPanelArea.toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (box, 16.0f);
        g.setColour (theme::line);
        g.drawRoundedRectangle (box.reduced (0.5f), 16.0f, 1.0f);
        auto rp = rollPanelArea.reduced (18, 14);
        const juce::String src = st.source == 0 ? "FL piano roll" : st.source == 2 ? "MIDI file" : "Drawn";
        theme::label (g, "Melody " + dot + " " + src, rp.removeFromTop (28));
        g.setColour (theme::dim);
        g.setFont (theme::ui (12.0f));
        g.drawText ("Click to add a note, drag right to lengthen, click a note to delete. Click a chord name to select it. Drop a .mid file here. Red notes clash.",
                    rp.removeFromBottom (18), juce::Justification::centredLeft, true);
    }

    // progression header
    theme::label (g, "Suggested progression", progHeadArea.withWidth (220));
    {
        g.setColour (theme::soft);
        g.setFont (theme::ui (14.0f));
        const auto area = juce::Rectangle<int> (prevButton.getRight(), progHeadArea.getY(), nextButton.getX() - prevButton.getRight(), progHeadArea.getHeight());
        g.drawText ("Suggestion " + juce::String (st.sugIdx + 1) + " / " + juce::String ((int) A.sugs.size()) + (A.edited ? " " + dot + " edited" : ""),
                    area, juce::Justification::centred);
    }

    // why this chord
    {
        const auto e = cg::explain (st, A, st.selected);
        juce::AttributedString as;
        as.setWordWrap (juce::AttributedString::byWord);
        const int q = e.text.indexOf ("?");
        as.append (e.text.substring (0, q + 1), theme::ui (13.5f, true), theme::fg);
        as.append (e.text.substring (q + 1), theme::ui (13.5f), theme::soft);
        if (e.clash.isNotEmpty()) as.append (" " + e.clash, theme::ui (13.5f), theme::clash);
        as.draw (g, explainArea.toFloat());
    }

    // keyboard legend and info
    {
        auto k = kbInfoArea.withTrimmedTop (8);
        g.setFont (theme::ui (13.0f));
        int x = k.getX();
        const std::pair<juce::Colour, juce::String> legend[] = { { a, "Melody" }, { theme::chord (proc.accent), "Chord tones" }, { theme::clash, "Clash" } };
        for (const auto& [c, t] : legend)
        {
            g.setColour (c);
            g.fillRoundedRectangle ((float) x, (float) k.getCentreY() - 6.0f, 12.0f, 12.0f, 3.0f);
            g.setColour (theme::soft);
            g.drawText (t, x + 18, k.getY(), 100, k.getHeight(), juce::Justification::centredLeft);
            x += 18 + (int) theme::textWidth (theme::ui (13.0f), t) + 18;
        }
        const int seg = juce::jlimit (0, A.nSeg - 1, keyboard.segment);
        const auto& v = A.voicing[(size_t) seg];
        juce::String names;
        for (auto m : v.notes) names += (names.isEmpty() ? "" : " ") + noteWithOctave (m, A.flats);
        juce::String info = "Showing " + cg::segmentLabel (seg, st.rhythm) + " " + dot + " " + (playSeg >= 0 ? "playing" : "selected") + " "
                          + dot + " " + cg::chordName (A.chordAt (seg), A, st.complexity) + ": " + names;
        if (st.bass) info += " " + dot + " bass " + noteWithOctave (v.bass, A.flats);
        g.setColour (theme::soft);
        g.drawText (info, k.withTrimmedLeft (x - k.getX()), juce::Justification::centredRight, true);
    }

    // control bar
    g.setColour (theme::line);
    g.fillRect (0, controlsArea.getY(), getWidth(), 1);
    theme::label (g, "Complexity " + dot + " " + juce::String (juce::StringArray { "Triads", "7ths", "9ths", "Jazz" }[st.complexity]), complexityLabel);
    theme::label (g, "Style", styleLabel);
    theme::label (g, "Voicing", voicingLabel);
    g.setColour (theme::fg);
    g.setFont (theme::mono (13.0f));
    g.drawText ((st.octave > 0 ? "+" : "") + juce::String (st.octave), octValueArea, juce::Justification::centred);
}
