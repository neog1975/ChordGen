#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ChordGen look: dark panels, one accent colour the user can switch (cyan, violet, orange, green).
namespace theme
{
const juce::Colour bg     { 0xff0c0f15 };
const juce::Colour panel  { 0xff131720 };
const juce::Colour panel2 { 0xff191e29 };
const juce::Colour line   { 0xff252c3a };
const juce::Colour line2  { 0xff323a4b };
const juce::Colour fg     { 0xffe8ecf3 };
const juce::Colour soft   { 0xffc5cbd8 };
const juce::Colour muted  { 0xff8a93a7 };
const juce::Colour dim    { 0xff5d6578 };
const juce::Colour clash  { 0xffff6b6b };
const juce::Colour ok     { 0xff4ade80 };
const juce::Colour rollBg { 0xff0f131b };

inline juce::Colour accent (int i)
{
    static const juce::Colour c[] = { juce::Colour (0xff5ee6f0), juce::Colour (0xffb49cff), juce::Colour (0xffffa45c), juce::Colour (0xff5ef0a4) };
    return c[juce::jlimit (0, 3, i)];
}
inline juce::Colour ink (int i)
{
    static const juce::Colour c[] = { juce::Colour (0xff05222a), juce::Colour (0xff1a1033), juce::Colour (0xff2a1300), juce::Colour (0xff04261a) };
    return c[juce::jlimit (0, 3, i)];
}
inline juce::Colour chord (int i) { return i == 1 ? juce::Colour (0xff5ee6f0) : juce::Colour (0xffa98bfa); }

inline juce::Font ui (float size, bool bold = false) { return juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain); }
inline juce::Font mono (float size) { return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain); }
inline float textWidth (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

// Small uppercase label with letter spacing, like "DETECTED KEY".
inline void label (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, juce::Colour c = muted)
{
    g.setColour (c);
    g.setFont (mono (11.0f).withExtraKerningFactor (0.14f));
    g.drawText (text.toUpperCase(), r, juce::Justification::centredLeft, false);
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel() { setAccent (0); }

    void setAccent (int index)
    {
        accentIndex = index;
        const auto a = accent (index);
        setColour (juce::ResizableWindow::backgroundColourId, bg);
        setColour (juce::TextButton::buttonColourId, panel2);
        setColour (juce::TextButton::buttonOnColourId, a);
        setColour (juce::TextButton::textColourOffId, soft);
        setColour (juce::TextButton::textColourOnId, ink (index));
        setColour (juce::ComboBox::backgroundColourId, panel2);
        setColour (juce::ComboBox::outlineColourId, line2);
        setColour (juce::ComboBox::textColourId, fg);
        setColour (juce::ComboBox::arrowColourId, muted);
        setColour (juce::ComboBox::focusedOutlineColourId, a);
        setColour (juce::PopupMenu::backgroundColourId, panel2);
        setColour (juce::PopupMenu::textColourId, fg);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, a.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId, fg);
        setColour (juce::ToggleButton::textColourId, soft);
        setColour (juce::ToggleButton::tickColourId, a);
        setColour (juce::ToggleButton::tickDisabledColourId, line2);
        setColour (juce::Slider::thumbColourId, a);
        setColour (juce::Slider::trackColourId, a);
        setColour (juce::Slider::backgroundColourId, line);
        setColour (juce::TooltipWindow::backgroundColourId, panel2);
        setColour (juce::TooltipWindow::textColourId, fg);
        setColour (juce::TooltipWindow::outlineColourId, line2);
        setColour (juce::ScrollBar::thumbColourId, line2);
    }

    int accentIndex = 0;

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const float radius = b.getProperties().getWithDefault ("radius", 8.0f);
        const bool on = b.getToggleState();
        const auto a = accent (accentIndex);
        const auto kind = b.getProperties().getWithDefault ("kind", "").toString();

        if (kind == "flat")
        {
            if (on) { g.setColour (a); g.fillRoundedRectangle (r, radius); }
            else if (over) { g.setColour (panel2); g.fillRoundedRectangle (r, radius); }
            return;
        }
        if (kind == "style" && on)
        {
            g.setColour (panel.interpolatedWith (a, 0.10f));
            g.fillRoundedRectangle (r, radius);
            g.setColour (a);
            g.drawRoundedRectangle (r, radius, 1.2f);
            return;
        }

        g.setColour (on ? a : (kind == "panel" ? panel : panel2).brighter (down ? 0.08f : 0.0f));
        g.fillRoundedRectangle (r, radius);
        g.setColour (on ? a : (over && b.isEnabled() ? a.withAlpha (0.7f) : line2));
        g.drawRoundedRectangle (r, radius, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        const auto kind = b.getProperties().getWithDefault ("kind", "").toString();
        const bool on = b.getToggleState();
        auto c = on ? (kind == "style" ? accent (accentIndex) : ink (accentIndex)) : soft;
        if (! b.isEnabled()) c = dim;
        g.setColour (c);
        g.setFont (ui ((float) b.getProperties().getWithDefault ("fontSize", 13.5f), true));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 0), juce::Justification::centred, 1, 0.8f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        const auto a = accent (accentIndex);
        const auto box = juce::Rectangle<float> (0.0f, (float) b.getHeight() * 0.5f - 7.0f, 14.0f, 14.0f);
        g.setColour (b.getToggleState() ? a : panel2);
        g.fillRoundedRectangle (box, 3.5f);
        g.setColour (b.getToggleState() ? a : (over ? a.withAlpha (0.7f) : line2));
        g.drawRoundedRectangle (box, 3.5f, 1.0f);
        if (b.getToggleState())
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
            tick.lineTo (box.getX() + 6.0f, box.getBottom() - 3.5f);
            tick.lineTo (box.getRight() - 3.0f, box.getY() + 3.5f);
            g.setColour (ink (accentIndex));
            g.strokePath (tick, juce::PathStrokeType (1.8f));
        }
        g.setColour (b.isEnabled() ? soft : dim);
        g.setFont (ui (13.0f));
        g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (21), juce::Justification::centredLeft, true);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (panel2);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (box.hasKeyboardFocus (false) ? accent (accentIndex) : line2);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);
        juce::Path arrow;
        const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
        arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
        arrow.lineTo (cx, cy + 2.0f);
        arrow.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (muted);
        g.strokePath (arrow, juce::PathStrokeType (1.5f));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return ui (13.0f); }
    juce::Font getPopupMenuFont() override { return ui (13.5f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& l) override
    {
        l.setBounds (8, 1, box.getWidth() - 28, box.getHeight() - 2);
        l.setFont (getComboBoxFont (box));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override
    {
        const float cy = (float) y + (float) h * 0.5f;
        const auto track = juce::Rectangle<float> ((float) x, cy - 3.0f, (float) w, 6.0f);
        g.setColour (line);
        g.fillRoundedRectangle (track, 3.0f);
        g.setColour (accent (accentIndex));
        g.fillRoundedRectangle (track.withRight (pos), 3.0f);
        g.fillEllipse (pos - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        g.setColour (bg);
        g.drawEllipse (pos - 9.0f, cy - 9.0f, 18.0f, 18.0f, 2.0f);
    }
};
} // namespace theme
