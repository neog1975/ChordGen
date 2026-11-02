#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "Theme.h"

class ChordGenEditor;

// Drag source for the current progression as a .mid file (into FL's piano roll or playlist).
class MidiDragButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    MidiDragButton();
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    juce::File file;
    int accent = 0;

private:
    bool dragging = false;
};

class AccentDots : public juce::Component
{
public:
    std::function<void (int)> onPick;
    int selected = 0;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
};

class KeyPanel : public juce::Component
{
public:
    explicit KeyPanel (ChordGenProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    ChordGenProcessor& proc;
    juce::TextButton alts[3];
    std::vector<cg::Key> altKeys;
    juce::ComboBox tonicBox, modeBox;
    juce::TextButton autoButton { "Auto-detect" };
};

class PianoRoll : public juce::Component
{
public:
    explicit PianoRoll (ChordGenEditor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    double playBeat = 0.0;
    bool playing = false;

private:
    static constexpr int head = 24, snap = cg::stepsPerBeat / 4; // editing grid: 16th notes
    ChordGenEditor& editor;
    int lo = 57, hi = 84;
    int dragIndex = -1;
    float cellW() const;
    float rowH() const;
};

class LockButton : public juce::Button
{
public:
    LockButton() : juce::Button ("lock") {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool locked = false;
    int accent = 0;
};

class ChordCard : public juce::Component
{
public:
    ChordCard (ChordGenEditor&, int index);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void refresh();

    bool playing = false;

private:
    ChordGenEditor& editor;
    int index;
    juce::TextButton alts[3];
    std::vector<int> altCodes;
    LockButton lock;
};

class Keyboard : public juce::Component
{
public:
    explicit Keyboard (ChordGenEditor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    int segment = 0;

private:
    ChordGenEditor& editor;
    static constexpr int lowNote = 48, highNote = 84;
    int noteAt (juce::Point<float>) const;
    juce::Rectangle<float> keyBounds (int note) const;
};

class Toast : public juce::Component, private juce::Timer
{
public:
    void show (const juce::String&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { setVisible (false); stopTimer(); }
    juce::String text;
};

// ChordGen FX: covers the roll while an audio file is analysed, asks for its tempo when the host
// has none (Standalone), or says why the file gave no chords.
class ImportPanel : public juce::Component
{
public:
    explicit ImportPanel (ChordGenProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void update(); // shows itself while there is something to show
    void askTempo (const juce::File&, double bpm);

private:
    void start();

    ChordGenProcessor& proc;
    juce::TextButton button, cancelButton { "Cancel" };
    juce::TextEditor bpmEditor;
    juce::File askFile;
    ChordGenProcessor::ImportStatus status;
};

//==============================================================================
class ChordGenEditor : public juce::AudioProcessorEditor,
                       public juce::FileDragAndDropTarget,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    explicit ChordGenEditor (ChordGenProcessor&);
    ~ChordGenEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void paintOverChildren (juce::Graphics&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragHover = false; repaint(); }

    ChordGenProcessor& proc;
    int playSeg = -1;

    void edit (std::function<void (cg::State&)> change, bool resuggest = true);
    void selectSegment (int seg);
    void importFile (const juce::File&); // .mid, or an audio file in ChordGen FX
    void audition (int note) { proc.auditionNote (note); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();
    void rebuildCards();
    void updatePlaying();
    void writeDragFile();
    void chooseImport();
    void importAudio (const juce::File&);
    void saveMidi();

    theme::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 500 };

    juce::TextButton tabHost { "FL Piano Roll" }, tabDraw { "Draw" }, tabMidi { "MIDI file" }, tabAudio { "Audio" }, tabMic { "Mic" };
    juce::TextButton captureButton;
    AccentDots accentDots;

    KeyPanel keyPanel;
    juce::ComboBox presetBox, barsBox, tsBox;
    juce::TextButton dupButton { "Duplicate" }, clearButton { "Clear" }, importButton { "Import .mid" };
    PianoRoll roll;

    juce::ComboBox rhythmBox, favBox;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, regenButton { "Regenerate" }, saveFavButton { "Save" };
    juce::TextButton prevButton { "<" }, nextButton { ">" };

    juce::Viewport cardsView;
    juce::Component cardsArea;
    std::vector<std::unique_ptr<ChordCard>> cards;

    Keyboard keyboard;

    juce::Slider complexity;
    juce::TextButton styleButtons[cg::numStyles];
    juce::ComboBox voicingBox;
    juce::TextButton octDown { "-" }, octUp { "+" };
    juce::ToggleButton bassToggle { "Bass line" }, borrowToggle { "Borrowed chords" }, melodyToggle { "Melody in preview" },
                       rhythmMidiToggle { "Style rhythm in MIDI" }, followToggle { "Play with FL" },
                       internalToggle { "Built-in sound" }, thruToggle { "Melody thru" },
                       liveToggle { "Live mode (1 bar delay)" }, metronomeToggle { "Metronome in preview" };
    juce::TextButton previewButton { "Preview" }, saveMidiButton { "Save MIDI..." };
    MidiDragButton dragButton;
    Toast toast;
    ImportPanel importPanel;
    bool dragHover = false;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::MemoryBlock lastDragData;
    int lastToastId = 0, ticks = 0;
    juce::String statusText { "Ready" };
    bool statusLive = false;

    // painted layout regions
    juce::Rectangle<int> pitchArea;
    float shownLevel = 0.0f, shownPitch = 0.0f;
    juce::Rectangle<int> headerArea, rollPanelArea, progHeadArea, explainArea, kbInfoArea, controlsArea,
                         complexityLabel, styleLabel, voicingLabel, optionsLabel, octValueArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordGenEditor)
};
