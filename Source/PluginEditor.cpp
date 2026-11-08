#include "PluginEditor.h"
#include "InternalOscillator.h"

//==============================================================================
class PolyHostEditor::Row : public juce::Component
{
public:
    Row (const juce::String& title, std::function<void()> openUi, std::function<void()> remove,
         std::function<void (bool)> onTrigger = {}, bool triggerOn = false)
        : hasRemove ((bool) remove), hasTrigger ((bool) onTrigger)
    {
        baseTitle = title;
        name.setText (title, juce::dontSendNotification);
        addAndMakeVisible (name);

        uiBtn.onClick = std::move (openUi);
        addAndMakeVisible (uiBtn);

        if (hasRemove)
        {
            rmBtn.onClick = std::move (remove);
            addAndMakeVisible (rmBtn);
        }

        if (hasTrigger)
        {
            scBtn.setToggleState (triggerOn, juce::dontSendNotification);
            scBtn.onClick = [this, cb = std::move (onTrigger)] { cb (scBtn.getToggleState()); };
            addAndMakeVisible (scBtn);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        if (hasRemove) rmBtn.setBounds (r.removeFromRight (70).reduced (2));
        uiBtn.setBounds (r.removeFromRight (70).reduced (2));
        if (hasTrigger) scBtn.setBounds (r.removeFromRight (84).reduced (2));
        name.setBounds (r);
    }

    void setSuffix (const juce::String& s)
    {
        name.setText (baseTitle + "     " + s, juce::dontSendNotification);
    }

private:
    bool hasRemove, hasTrigger;
    juce::String baseTitle;
    juce::Label name;
    juce::TextButton uiBtn { "Open UI" }, rmBtn { "Remove" };
    juce::ToggleButton scBtn { "Trig SC" };
};

//==============================================================================
PolyHostEditor::PolyHostEditor (PolyHostProcessor& p)
    : AudioProcessorEditor (&p), proc (p), gainAttach (*p.gainParam, gainSlider)
{
    for (auto* b : { &scanBtn, &engineBtn, &fxBtn, &syncBtn })
        addAndMakeVisible (*b);

    scanBtn.onClick   = [this] { showScanner(); };
    engineBtn.onClick = [this] { showPicker (true); };
    fxBtn.onClick     = [this] { showPicker (false); };
    syncBtn.onClick   = [this] { proc.syncAllStates(); };

    for (int n : { 1, 2, 4, 6, 8, 12, 16, 24, 32 })
        voiceBox.addItem (juce::String (n), n);
    voiceBox.onChange = [this] { proc.setNumVoices (voiceBox.getSelectedId()); };
    addAndMakeVisible (voiceBox);
    addAndMakeVisible (voiceLabel);

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    addAndMakeVisible (gainSlider);
    addAndMakeVisible (gainLabel);

    proc.addChangeListener (this);
    refreshRows();
    startTimerHz (4);
}

PolyHostEditor::~PolyHostEditor()
{
    stopTimer();
    proc.removeChangeListener (this);
}

void PolyHostEditor::refreshRows()
{
    engineRow.reset();
    fxRows.clear();

    if (auto* e = proc.getEngine())
    {
        engineRow = std::make_unique<Row> (e->desc.name,
                                           [this] { if (auto* s = proc.getEngine()) s->showEditor(); },
                                           std::function<void()>{});
        addAndMakeVisible (*engineRow);
    }

    for (int i = 0; i < proc.getNumEffects(); ++i)
    {
        auto* fx = proc.getEffect (i);

        std::function<void (bool)> scCallback;
        if (fx->hasSidechain())
            scCallback = [this, i] (bool on) { if (auto* s = proc.getEffect (i)) s->scTrigger = on; };

        auto* r = fxRows.add (new Row (fx->desc.name,
                                       [this, i] { if (auto* s = proc.getEffect (i)) s->showEditor(); },
                                       [this, i] { proc.removeEffect (i); },
                                       scCallback, fx->scTrigger.load()));
        addAndMakeVisible (r);
    }

    voiceBox.setSelectedId (proc.getNumVoices(), juce::dontSendNotification);
    setSize (680, 210 + 34 * juce::jmax (1, fxRows.size()));
    resized();
    repaint();
}

void PolyHostEditor::timerCallback()
{
    // Diagnostics: does each effect offer a MIDI input, and has the host sent it any events?
    for (int i = 0; i < fxRows.size(); ++i)
        if (auto* s = proc.getEffect (i))
            fxRows[i]->setSuffix (juce::String (s->acceptsMidi() ? "[MIDI in: yes" : "[MIDI in: NO")
                                  + ", events: " + juce::String (s->midiEventsSent.load())
                                  + ", sidechain: " + (s->hasSidechain() ? (s->scTrigger.load() ? "gate" : "off") : "none") + "]");
}

void PolyHostEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202225));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    g.drawText ("SOUND ENGINE  (one instance per voice)", 12, 88, 400, 18, juce::Justification::left);
    g.drawText ("EFFECT CHAIN  (one chain per voice)",    12, 88 + 56, 400, 18, juce::Justification::left);

    g.setColour (juce::Colours::grey);
    g.setFont (12.0f);
    if (proc.getEngine() == nullptr)
        g.drawText ("none loaded", 12, 108, 300, 24, juce::Justification::left);
    if (proc.getNumEffects() == 0)
        g.drawText ("none", 12, 108 + 56, 300, 24, juce::Justification::left);
}

void PolyHostEditor::resized()
{
    auto r = getLocalBounds().reduced (8);

    auto top = r.removeFromTop (28);
    const int w = top.getWidth() / 4;
    scanBtn.setBounds   (top.removeFromLeft (w).reduced (2));
    engineBtn.setBounds (top.removeFromLeft (w).reduced (2));
    fxBtn.setBounds     (top.removeFromLeft (w).reduced (2));
    syncBtn.setBounds   (top.reduced (2));

    r.removeFromTop (6);
    auto row2 = r.removeFromTop (26);
    voiceLabel.setBounds (row2.removeFromLeft (50));
    voiceBox.setBounds   (row2.removeFromLeft (70));
    row2.removeFromLeft (16);
    gainLabel.setBounds  (row2.removeFromLeft (40));
    gainSlider.setBounds (row2);

    r.removeFromTop (28);                         // section title
    auto engArea = r.removeFromTop (34);
    if (engineRow != nullptr) engineRow->setBounds (engArea);

    r.removeFromTop (22);                         // section title
    for (auto* fx : fxRows)
        fx->setBounds (r.removeFromTop (34));
}

//==============================================================================
void PolyHostEditor::showPicker (bool instruments)
{
    auto types = proc.knownPlugins.getTypes();     // copy
    juce::PopupMenu menu;
    juce::Array<juce::PluginDescription> choices;

    if (instruments)                                   // built-in engine always available
    {
        choices.add (InternalOscillator::makeDescription());
        menu.addItem (choices.size(), "Built-in Oscillator (sine / triangle / saw / pulse / noise)");
        menu.addSeparator();
    }

    for (auto& d : types)
        if (d.isInstrument == instruments)
        {
            choices.add (d);
            menu.addItem (choices.size(), d.name + "  (" + d.manufacturerName + ")");
        }

    if (choices.size() == (instruments ? 1 : 0))
        menu.addItem (-1, "No hosted plugins found - press 'Scan plugins...' first", false);

    auto* target = instruments ? &engineBtn : &fxBtn;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [this, choices, instruments] (int result)
    {
        if (result <= 0) return;
        juce::String err;
        const auto& d = choices.getReference (result - 1);
        const bool ok = instruments ? proc.loadEngine (d, err) : proc.addEffect (d, err);

        if (! ok)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Could not load plugin", err);
    });
}

void PolyHostEditor::showScanner()
{
    auto* list = new juce::PluginListComponent (proc.formatManager, proc.knownPlugins,
                                                PolyHostProcessor::getDataDir().getChildFile ("scan.dead"),
                                                nullptr, true);
    list->setSize (720, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "Plugin list";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = juce::Colour (0xff303236);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}
