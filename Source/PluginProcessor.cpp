#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
PolyHostProcessor::PolyHostProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (gainParam = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Output Gain",
                                                             juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 0.7f));
    formatManager.addDefaultFormats();    // VST3 (+AU on macOS) thanks to JUCE_PLUGINHOST_* flags

    if (auto xml = juce::XmlDocument::parse (getDataDir().getChildFile ("plugins.xml")))
        knownPlugins.recreateFromXml (*xml);
    knownPlugins.addChangeListener (this);

    ccState.fill (-1);
    startTimerHz (10);
}

PolyHostProcessor::~PolyHostProcessor()
{
    stopTimer();
    knownPlugins.removeChangeListener (this);
}

juce::File PolyHostProcessor::getDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("PolyHost");
    d.createDirectory();
    return d;
}

void PolyHostProcessor::saveKnownPlugins()
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (getDataDir().getChildFile ("plugins.xml"));
}

juce::AudioProcessorEditor* PolyHostProcessor::createEditor() { return new PolyHostEditor (*this); }

bool PolyHostProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
// Hosting helpers
//==============================================================================
void PolyHostProcessor::configureInstance (juce::AudioPluginInstance& p) const
{
    auto layout = p.getBusesLayout();
    if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0)  = juce::AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    p.setBusesLayout (layout);                       // best effort
    p.setRateAndBufferSizeDetails (sampleRate, blockSize);
    p.prepareToPlay (sampleRate, blockSize);
}

std::unique_ptr<juce::AudioPluginInstance> PolyHostProcessor::createInstance (const juce::PluginDescription& d, juce::String& err)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto inst = formatManager.createPluginInstance (d, sampleRate, blockSize, err);
    if (inst != nullptr)
        configureInstance (*inst);
    return inst;
}

void PolyHostProcessor::copyState (Slot& s)
{
    juce::MemoryBlock mb;
    s.master->getStateInformation (mb);
    for (auto& c : s.copies)
        c->setStateInformation (mb.getData(), (int) mb.getSize());
}

std::unique_ptr<Slot> PolyHostProcessor::createSlot (const juce::PluginDescription& d, int nCopies, juce::String& err)
{
    auto slot = std::make_unique<Slot>();
    slot->desc   = d;
    slot->master = createInstance (d, err);
    if (slot->master == nullptr) return nullptr;

    for (int i = 0; i < nCopies; ++i)
    {
        auto inst = createInstance (d, err);
        if (inst == nullptr) return nullptr;
        slot->copies.push_back (std::move (inst));
    }

    slot->master->addListener (slot.get());
    return slot;
}

void PolyHostProcessor::updateScratch()      // call with lock held (or before audio starts)
{
    int ch = 2;
    auto look = [&] (Slot* s)
    {
        if (s != nullptr && s->master != nullptr)
            ch = juce::jmax (ch, s->master->getTotalNumInputChannels(), s->master->getTotalNumOutputChannels());
    };
    look (engine.get());
    for (auto& e : effects) look (e.get());
    scratch.setSize (ch, juce::jmax (blockSize, 1), false, true, false);
}

void PolyHostProcessor::updateLatency()
{
    int lat = 0;
    if (engine != nullptr) lat += engine->master->getLatencySamples();
    for (auto& e : effects) lat += e->master->getLatencySamples();
    if (lat != getLatencySamples())
        setLatencySamples (lat);
}

bool PolyHostProcessor::loadEngine (const juce::PluginDescription& d, juce::String& err)
{
    auto slot = createSlot (d, numVoices, err);
    if (slot == nullptr) return false;

    std::unique_ptr<Slot> old;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (engine);
        engine = std::move (slot);
        for (auto& v : voices) v = Voice{};
        updateScratch();
    }
    old.reset();                      // destroy outside the lock
    updateLatency();
    sendChangeMessage();
    return true;
}

bool PolyHostProcessor::addEffect (const juce::PluginDescription& d, juce::String& err)
{
    auto slot = createSlot (d, numVoices, err);
    if (slot == nullptr) return false;
    {
        const juce::ScopedLock sl (lock);
        effects.push_back (std::move (slot));
        updateScratch();
    }
    updateLatency();
    sendChangeMessage();
    return true;
}

void PolyHostProcessor::removeEffect (int index)
{
    std::unique_ptr<Slot> old;
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) effects.size())) return;
        old = std::move (effects[(size_t) index]);
        effects.erase (effects.begin() + index);
    }
    old.reset();
    updateLatency();
    sendChangeMessage();
}

void PolyHostProcessor::setNumVoices (int n)
{
    n = juce::jlimit (1, maxVoices, n);
    const int current = numVoices;
    if (n == current) return;

    std::vector<Slot*> slots;
    if (engine != nullptr) slots.push_back (engine.get());
    for (auto& e : effects) slots.push_back (e.get());

    // Build the new instances OUTSIDE the lock (this can take a while)
    std::vector<std::vector<std::unique_ptr<juce::AudioPluginInstance>>> extra (slots.size());
    if (n > current)
    {
        for (size_t s = 0; s < slots.size(); ++s)
        {
            juce::MemoryBlock mb;
            slots[s]->master->getStateInformation (mb);

            for (int i = current; i < n; ++i)
            {
                juce::String err;
                auto inst = createInstance (slots[s]->desc, err);
                if (inst == nullptr) { sendChangeMessage(); return; }   // abort, nothing changed
                inst->setStateInformation (mb.getData(), (int) mb.getSize());
                extra[s].push_back (std::move (inst));
            }
        }
    }

    std::vector<std::unique_ptr<juce::AudioPluginInstance>> graveyard;
    {
        const juce::ScopedLock sl (lock);
        for (size_t s = 0; s < slots.size(); ++s)
        {
            auto& c = slots[s]->copies;
            if (n > current)
                for (auto& x : extra[s]) c.push_back (std::move (x));
            else
                while ((int) c.size() > n) { graveyard.push_back (std::move (c.back())); c.pop_back(); }
        }
        for (int i = n; i < maxVoices; ++i) voices[(size_t) i] = Voice{};
        numVoices = n;
    }
    graveyard.clear();
    sendChangeMessage();
}

void PolyHostProcessor::syncAllStates()
{
    const juce::ScopedLock sl (lock);
    if (engine != nullptr) copyState (*engine);
    for (auto& e : effects) copyState (*e);
}

void PolyHostProcessor::timerCallback()
{
    // Non-parameter changes (patch loads, sample loads...) are copied to all voices here.
    auto check = [this] (Slot* s)
    {
        if (s != nullptr && s->stateDirty.exchange (false))
        {
            const juce::ScopedLock sl (lock);   // audio thread outputs silence while this runs
            copyState (*s);
        }
    };
    check (engine.get());
    for (auto& e : effects) check (e.get());
}

void PolyHostProcessor::clearAll()
{
    std::unique_ptr<Slot> oldEngine;
    std::vector<std::unique_ptr<Slot>> oldFx;
    {
        const juce::ScopedLock sl (lock);
        oldEngine = std::move (engine);
        oldFx = std::move (effects);
        effects.clear();
        for (auto& v : voices) v = Voice{};
    }
}

//==============================================================================
// Audio
//==============================================================================
void PolyHostProcessor::prepareToPlay (double sr, int bs)
{
    const juce::ScopedLock sl (lock);
    sampleRate = sr;
    blockSize  = bs;
    silenceTimeout = (int) (sr * 0.5);

    auto prep = [&] (Slot* s)
    {
        if (s == nullptr) return;
        auto one = [&] (juce::AudioPluginInstance& i) { i.setRateAndBufferSizeDetails (sr, bs); i.prepareToPlay (sr, bs); };
        one (*s->master);
        for (auto& c : s->copies) one (*c);
    };
    prep (engine.get());
    for (auto& e : effects) prep (e.get());

    updateScratch();
    for (auto& v : voices) v = Voice{};
}

int PolyHostProcessor::allocateVoice()
{
    const int nv = numVoices;

    for (int i = 0; i < nv; ++i)                       // 1. a free voice
        if (! voices[(size_t) i].active) return i;

    int best = -1;                                     // 2. oldest voice that is only releasing
    for (int i = 0; i < nv; ++i)
        if (! voices[(size_t) i].held && (best < 0 || voices[(size_t) i].age < voices[(size_t) best].age))
            best = i;
    if (best >= 0) return best;

    best = 0;                                          // 3. oldest of all
    for (int i = 1; i < nv; ++i)
        if (voices[(size_t) i].age < voices[(size_t) best].age) best = i;
    return best;
}

void PolyHostProcessor::broadcast (const juce::MidiMessage& m, int pos)
{
    const int nv = numVoices;
    for (int i = 0; i < nv; ++i)
        if (voices[(size_t) i].active)
            voices[(size_t) i].midi.addEvent (m, pos);
}

void PolyHostProcessor::routeMidi (const juce::MidiMessage& m, int pos)
{
    const int nv = numVoices;

    if (m.isNoteOn())
    {
        auto& v = voices[(size_t) allocateVoice()];

        if (v.active && v.held)                        // stealing a held note
            v.midi.addEvent (juce::MidiMessage::noteOff (v.channel, v.note), pos);

        v.active = true;  v.held = true;
        v.note = m.getNoteNumber();  v.channel = m.getChannel();
        v.age = ++ageCounter;  v.silentSamples = 0;

        // bring the fresh voice up to date with controllers it never saw
        for (int cc = 0; cc < 120; ++cc)
            if (ccState[(size_t) cc] >= 0)
                v.midi.addEvent (juce::MidiMessage::controllerEvent (v.channel, cc, ccState[(size_t) cc]), pos);
        if (pitchWheel != 8192)
            v.midi.addEvent (juce::MidiMessage::pitchWheel (v.channel, pitchWheel), pos);

        v.midi.addEvent (m, pos);
    }
    else if (m.isNoteOff())
    {
        int best = -1;                                 // release the oldest matching held voice
        for (int i = 0; i < nv; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.active && v.held && v.note == m.getNoteNumber() && v.channel == m.getChannel()
                && (best < 0 || v.age < voices[(size_t) best].age))
                best = i;
        }
        if (best >= 0)
        {
            voices[(size_t) best].midi.addEvent (m, pos);
            voices[(size_t) best].held = false;
        }
    }
    else if (m.isAftertouch())                         // polyphonic pressure -> only that note's voice
    {
        for (int i = 0; i < nv; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.active && v.held && v.note == m.getNoteNumber())
                v.midi.addEvent (m, pos);
        }
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        broadcast (m, pos);
        for (int i = 0; i < nv; ++i) voices[(size_t) i].held = false;
    }
    else
    {
        if (m.isPitchWheel())
            pitchWheel = m.getPitchWheelValue();
        else if (m.isController() && m.getControllerNumber() < 120)
            ccState[(size_t) m.getControllerNumber()] = m.getControllerValue();

        broadcast (m, pos);                            // CCs, pitch bend, channel pressure, ...
    }
}

void PolyHostProcessor::renderVoice (int index, juce::AudioBuffer<float>& view)
{
    auto& v = voices[(size_t) index];
    const int n = view.getNumSamples();
    auto* playHead = getPlayHead();

    // 1. sound engine -> this voice's private signal
    view.clear();
    auto& eng = *engine->copies[(size_t) index];
    eng.setPlayHead (playHead);
    eng.processBlock (view, v.midi);

    // 2. this voice's private effect chain (its own filter/reverb/... instances)
    for (auto& fx : effects)
    {
        for (int c = 2; c < view.getNumChannels(); ++c)   // clear sidechain / extra channels
            view.clear (c, 0, n);

        noMidi.clear();
        auto& inst = *fx->copies[(size_t) index];
        inst.setPlayHead (playHead);
        inst.processBlock (view, noMidi);
    }

    // 3. free the voice when released and silent long enough (lets reverb tails ring out)
    if (! v.held)
    {
        const float mag = juce::jmax (view.getMagnitude (0, 0, n), view.getMagnitude (1, 0, n));
        v.silentSamples = (mag < 3.0e-5f) ? v.silentSamples + n : 0;   // about -90 dB
        if (v.silentSamples > silenceTimeout)
            v.active = false;
    }
}

void PolyHostProcessor::processBlock (juce::AudioBuffer<float>& out, juce::MidiBuffer& midiIn)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = out.getNumSamples();
    out.clear();

    const juce::ScopedTryLock sl (lock);               // never block the audio thread
    if (! sl.isLocked() || engine == nullptr)
        return;

    if (scratch.getNumSamples() < n)                   // host exceeded the promised block size
        scratch.setSize (scratch.getNumChannels(), n, false, false, true);

    const int nv = numVoices;
    for (int i = 0; i < nv; ++i)
        voices[(size_t) i].midi.clear();

    for (const auto meta : midiIn)
        routeMidi (meta.getMessage(), meta.samplePosition);

    juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), n);

    for (int i = 0; i < nv; ++i)
    {
        if (! voices[(size_t) i].active) continue;

        renderVoice (i, view);                         // engine -> fx chain, fully isolated per voice

        for (int c = 0; c < out.getNumChannels(); ++c) // ...and sum the voices at the very end
            out.addFrom (c, 0, view, juce::jmin (c, 1), 0, n);
    }

    const float g = gainParam->get();
    out.applyGainRamp (0, n, lastGain, g);
    lastGain = g;
}

//==============================================================================
// State
//==============================================================================
void PolyHostProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("POLYHOST");
    xml.setAttribute ("voices", (int) numVoices);
    xml.setAttribute ("gain", (double) gainParam->get());

    auto add = [&] (const char* tag, Slot& s)
    {
        auto* e = xml.createNewChildElement (tag);
        e->addChildElement (s.desc.createXml().release());
        juce::MemoryBlock mb;
        s.master->getStateInformation (mb);
        e->setAttribute ("state", mb.toBase64Encoding());
    };

    if (engine != nullptr) add ("ENGINE", *engine);
    for (auto& e : effects) add ("FX", *e);

    copyXmlToBinary (xml, dest);
}

std::unique_ptr<Slot> PolyHostProcessor::slotFromXml (const juce::XmlElement& e, juce::String& err)
{
    juce::PluginDescription d;
    auto* descXml = e.getChildByName ("PLUGIN");
    if (descXml == nullptr || ! d.loadFromXml (*descXml)) { err = "Bad plugin description"; return nullptr; }

    auto slot = createSlot (d, numVoices, err);
    if (slot == nullptr) return nullptr;

    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (e.getStringAttribute ("state")) && mb.getSize() > 0)
    {
        slot->master->setStateInformation (mb.getData(), (int) mb.getSize());
        copyState (*slot);
    }
    return slot;
}

void PolyHostProcessor::setStateInformation (const void* data, int size)
{
    if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::WeakReference<PolyHostProcessor> weak (this);
        juce::MemoryBlock copy (data, (size_t) size);
        juce::MessageManager::callAsync ([weak, copy]
        {
            if (weak != nullptr) weak->setStateInformation (copy.getData(), (int) copy.getSize());
        });
        return;
    }

    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("POLYHOST")) return;

    clearAll();
    {
        const juce::ScopedLock sl (lock);
        numVoices = juce::jlimit (1, maxVoices, xml->getIntAttribute ("voices", 8));
    }
    *gainParam = (float) xml->getDoubleAttribute ("gain", 0.7);

    juce::String err;
    if (auto* e = xml->getChildByName ("ENGINE"))
        if (auto s = slotFromXml (*e, err))
        {
            const juce::ScopedLock sl (lock);
            engine = std::move (s);
        }

    for (auto* e : xml->getChildWithTagNameIterator ("FX"))
        if (auto s = slotFromXml (*e, err))
        {
            const juce::ScopedLock sl (lock);
            effects.push_back (std::move (s));
        }

    { const juce::ScopedLock sl (lock); updateScratch(); }
    updateLatency();
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PolyHostProcessor(); }
