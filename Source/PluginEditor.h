#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class PolyHostEditor : public juce::AudioProcessorEditor,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    explicit PolyHostEditor (PolyHostProcessor&);
    ~PolyHostEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshRows(); }
    void timerCallback() override;
    void refreshRows();
    void showPicker (bool instruments);
    void showScanner();

    PolyHostProcessor& proc;

    juce::TextButton scanBtn   { "Scan plugins..." };
    juce::TextButton engineBtn { "Load sound engine..." };
    juce::TextButton fxBtn     { "Add effect..." };
    juce::TextButton syncBtn   { "Sync all voices" };

    juce::Label voiceLabel { {}, "Voices" }, gainLabel { {}, "Gain" };
    juce::ComboBox voiceBox;
    juce::Slider gainSlider;
    juce::SliderParameterAttachment gainAttach;

    std::unique_ptr<Row> engineRow;
    juce::OwnedArray<Row> fxRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyHostEditor)
};
