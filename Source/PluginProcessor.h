#pragma once
#include <JuceHeader.h>

//==============================================================================
/** Floating window that shows a hosted plugin's own GUI. Closing only hides it. */
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& p, const juce::String& title, std::function<void()> onHide)
        : DocumentWindow (title, juce::Colours::darkgrey, DocumentWindow::closeButton),
          hiddenCallback (std::move (onHide))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* ed = p.createEditorIfNeeded();
        if (ed == nullptr)
            ed = new juce::GenericAudioProcessorEditor (p);

        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        setVisible (false);
        if (hiddenCallback) hiddenCallback();
    }

private:
    std::function<void()> hiddenCallback;
};

//==============================================================================
/** One hosted plugin "slot" (the engine, or one effect in the chain).

    master : the instance the user sees/edits (never processes audio)
    copies : one independent instance per voice - these do the real audio work

    Parameter moves on `master` are mirrored to every copy.
*/
struct Slot : public juce::AudioProcessorListener
{
    juce::PluginDescription desc;
    std::unique_ptr<juce::AudioProcessor> master;
    std::vector<std::unique_ptr<juce::AudioProcessor>> copies;
    std::atomic<bool> stateDirty { false };
    std::unique_ptr<PluginWindow> window;           // keep LAST: destroyed first

    ~Slot() override
    {
        window.reset();
        if (master != nullptr) master->removeListener (this);
    }

    void showEditor()
    {
        if (master == nullptr) return;
        if (window == nullptr)
            window = std::make_unique<PluginWindow> (*master, desc.name, [this] { stateDirty = true; });

        window->setVisible (true);
        window->toFront (true);
    }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override
    {
        for (auto& c : copies)
            if (auto* p = c->getParameters()[index])
                p->setValue (value);
    }

    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
    {
        if (d.nonParameterStateChanged || d.programChanged)
            stateDirty = true;
    }
};

//==============================================================================
class PolyHostProcessor : public juce::AudioProcessor,
                          public juce::ChangeBroadcaster,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    static constexpr int maxVoices = 32;

    // Declared first so they are destroyed LAST (hosted plugins need the format manager alive)
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioParameterFloat* gainParam = nullptr;

    PolyHostProcessor();
    ~PolyHostProcessor() override;

    //==== AudioProcessor ======================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PolyHost"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==== Hosting API (message thread only) ===================================
    bool loadEngine (const juce::PluginDescription&, juce::String& error);
    bool addEffect  (const juce::PluginDescription&, juce::String& error);
    void removeEffect (int index);
    void setNumVoices (int n);
    void syncAllStates();                 // copy every master's full state to all voices

    Slot* getEngine() const                { return engine.get(); }
    int   getNumEffects() const            { return (int) effects.size(); }
    Slot* getEffect (int i) const          { return juce::isPositiveAndBelow (i, (int) effects.size()) ? effects[(size_t) i].get() : nullptr; }
    int   getNumVoices() const             { return numVoices; }

    static juce::File getDataDir();

private:
    struct Voice
    {
        juce::MidiBuffer midi;
        int  note = -1, channel = 1;
        bool active = false;       // being rendered (note held, or tail still audible)
        bool held   = false;       // key is physically down
        uint64_t age = 0;
        int  silentSamples = 0;
    };

    // creation helpers
    std::unique_ptr<juce::AudioProcessor> createInstance (const juce::PluginDescription&, juce::String& err);
    void configureInstance (juce::AudioProcessor&) const;
    std::unique_ptr<Slot> createSlot (const juce::PluginDescription&, int nCopies, juce::String& err);
    std::unique_ptr<Slot> slotFromXml (const juce::XmlElement&, juce::String& err);
    static void copyState (Slot&);
    void clearAll();
    void updateScratch();
    void updateLatency();
    void saveKnownPlugins();

    // audio-thread helpers
    void routeMidi (const juce::MidiMessage&, int samplePos);
    int  allocateVoice();
    void broadcast (const juce::MidiMessage&, int pos);
    void renderVoice (int index, juce::AudioBuffer<float>& view);

    void changeListenerCallback (juce::ChangeBroadcaster*) override { saveKnownPlugins(); }
    void timerCallback() override;

    //==== data ================================================================
    std::unique_ptr<Slot> engine;
    std::vector<std::unique_ptr<Slot>> effects;

    std::atomic<int> numVoices { 8 };
    std::array<Voice, maxVoices> voices;

    juce::AudioBuffer<float> scratch;          // one voice is rendered at a time
    juce::MidiBuffer noMidi;                   // fed to effects
    juce::CriticalSection lock;                // structure lock (audio thread only try-locks)

    double sampleRate = 44100.0;
    int    blockSize  = 512;
    int    silenceTimeout = 22050;
    uint64_t ageCounter = 0;
    float  lastGain = 0.7f;

    std::array<int, 120> ccState {};           // last controller values, replayed to new voices
    int pitchWheel = 8192;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PolyHostProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyHostProcessor)
};
