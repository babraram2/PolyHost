#pragma once
#include <JuceHeader.h>

/** Built-in sound engine: one monophonic oscillator with an ADSR envelope.

    PolyHost creates one of these PER VOICE, so polyphony comes from the voice
    pool, exactly like with a hosted VSTi. It is a normal juce::AudioProcessor,
    so parameter mirroring, state saving and the generic editor all just work.
*/
class InternalOscillator : public juce::AudioProcessor
{
public:
    static inline const juce::String formatName { "PolyHostInternal" };

    static juce::PluginDescription makeDescription()
    {
        juce::PluginDescription d;
        d.name             = "PolyHost Oscillator";
        d.descriptiveName  = "Built-in oscillator";
        d.pluginFormatName = formatName;
        d.category         = "Generator";
        d.manufacturerName = "PolyHost";
        d.version          = "1.0";
        d.fileOrIdentifier = "polyhost.oscillator";
        d.uniqueId = d.deprecatedUid = 0x50484f31;
        d.isInstrument     = true;
        d.numInputChannels = 0;
        d.numOutputChannels = 2;
        return d;
    }

    InternalOscillator()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        using PID = juce::ParameterID;
        using NR  = juce::NormalisableRange<float>;

        addParameter (wave       = new juce::AudioParameterChoice (PID { "wave", 1 }, "Waveform",
                                        juce::StringArray { "Sine", "Triangle", "Saw", "Pulse", "Noise" }, 2));
        addParameter (pulseWidth = new juce::AudioParameterFloat (PID { "pw", 1 },      "Pulse Width", NR (0.02f, 0.98f), 0.5f));
        addParameter (attack     = new juce::AudioParameterFloat (PID { "attack", 1 },  "Attack",      NR (0.001f, 5.0f, 0.0f, 0.3f), 0.005f));
        addParameter (decay      = new juce::AudioParameterFloat (PID { "decay", 1 },   "Decay",       NR (0.001f, 5.0f, 0.0f, 0.3f), 0.3f));
        addParameter (sustain    = new juce::AudioParameterFloat (PID { "sustain", 1 }, "Sustain",     NR (0.0f, 1.0f), 0.8f));
        addParameter (release    = new juce::AudioParameterFloat (PID { "release", 1 }, "Release",     NR (0.001f, 8.0f, 0.0f, 0.3f), 0.3f));
        addParameter (level      = new juce::AudioParameterFloat (PID { "level", 1 },   "Level",       NR (0.0f, 1.0f), 0.5f));
        addParameter (tune       = new juce::AudioParameterFloat (PID { "tune", 1 },    "Tune (cents)", NR (-1200.0f, 1200.0f, 1.0f), 0.0f));
    }

    //==== AudioProcessor ======================================================
    const juce::String getName() const override { return "PolyHost Oscillator"; }

    void prepareToPlay (double sampleRate, int) override
    {
        sr = sampleRate;
        phase = 0.0; env = 0.0f; stage = Idle; held = false;
        sustainDown = false; pendingRelease = false; bendSemis = 0.0;
    }
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        const int n = buf.getNumSamples();
        buf.clear();
        auto* out = buf.getWritePointer (0);

        int pos = 0;
        for (const auto meta : midi)                       // sample-accurate MIDI
        {
            const int p = juce::jlimit (pos, n, meta.samplePosition);
            renderSegment (out, pos, p);
            pos = p;
            handleMidi (meta.getMessage());
        }
        renderSegment (out, pos, n);

        for (int c = 1; c < buf.getNumChannels(); ++c)
            buf.copyFrom (c, 0, buf, 0, 0, n);
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }    // host window falls back to the generic editor

    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return (double) release->get(); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::XmlElement xml ("OSC");
        for (auto* p : getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                xml.setAttribute (r->paramID, (double) r->convertFrom0to1 (r->getValue()));
        copyXmlToBinary (xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            for (auto* p : getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (xml->hasAttribute (r->paramID))
                        r->setValueNotifyingHost (r->convertTo0to1 ((float) xml->getDoubleAttribute (r->paramID)));
    }

private:
    enum Stage { Idle, Attack, Decay, Sustain, Release };

    //---- band-limited step correction (PolyBLEP) for saw and pulse ----------
    static float polyBlep (double t, double dt)
    {
        if (t < dt)         { t /= dt;               return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt)   { t = (t - 1.0) / dt;    return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }

    float oscSample (int w, float pw, double inc)
    {
        switch (w)
        {
            case 0:  return (float) std::sin (juce::MathConstants<double>::twoPi * phase);       // sine
            case 1:  return (float) (4.0 * std::abs (phase - 0.5) - 1.0);                        // triangle
            case 2:  return (float) (2.0 * phase - 1.0) - polyBlep (phase, inc);                 // saw
            case 3:                                                                              // pulse / rectangle
            {
                float s = phase < (double) pw ? 1.0f : -1.0f;
                s += polyBlep (phase, inc);
                s -= polyBlep (std::fmod (phase + 1.0 - (double) pw, 1.0), inc);
                return s;
            }
            default: return rng.nextFloat() * 2.0f - 1.0f;                                       // noise
        }
    }

    float timeToCoef (float seconds) const
    {
        return (float) (1.0 - std::exp (-4.6 / (juce::jmax (seconds, 0.001f) * sr)));
    }

    void renderSegment (float* out, int start, int end)
    {
        if (stage == Idle || end <= start) return;           // buffer is already zeroed

        const double inc = baseFreq * std::pow (2.0, (double) tune->get() / 1200.0 + bendSemis / 12.0) / sr;
        const int   w   = wave->getIndex();
        const float pw  = pulseWidth->get();
        const float sus = sustain->get();
        const float lvl = level->get();
        const float cD  = timeToCoef (decay->get());
        const float cR  = timeToCoef (release->get());
        const float aInc = 1.0f / (juce::jmax (attack->get(), 0.001f) * (float) sr);

        for (int i = start; i < end; ++i)
        {
            switch (stage)
            {
                case Attack:  env += aInc;               if (env >= 1.0f) { env = 1.0f; stage = Decay; } break;
                case Decay:   env += (sus - env) * cD;   if (std::abs (env - sus) < 1.0e-4f) { env = sus; stage = Sustain; } break;
                case Sustain: env += (sus - env) * cD;   break;
                case Release: env -= env * cR;           if (env < 1.0e-4f) { env = 0.0f; stage = Idle; } break;
                case Idle:    break;
            }
            if (stage == Idle) break;

            out[i] = oscSample (w, pw, inc) * env * velocity * lvl;

            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
        }
    }

    void releaseNote()
    {
        held = false;
        if (stage != Idle) stage = Release;
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            note = m.getNoteNumber();
            baseFreq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            velocity = m.getFloatVelocity();
            if (stage == Idle) { phase = 0.0; env = 0.0f; }   // retrigger without a click if still sounding
            stage = Attack;
            held = true;
            pendingRelease = false;
        }
        else if (m.isNoteOff())
        {
            if (held && m.getNoteNumber() == note)
            {
                if (sustainDown) pendingRelease = true;
                else             releaseNote();
            }
        }
        else if (m.isController() && m.getControllerNumber() == 64)    // sustain pedal
        {
            sustainDown = m.getControllerValue() >= 64;
            if (! sustainDown && pendingRelease) { pendingRelease = false; releaseNote(); }
        }
        else if (m.isPitchWheel())
        {
            bendSemis = (m.getPitchWheelValue() - 8192) / 8192.0 * 2.0;   // +/- 2 semitones
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            pendingRelease = false;
            releaseNote();
        }
    }

    //---- parameters (owned by the AudioProcessor) ----------------------------
    juce::AudioParameterChoice* wave = nullptr;
    juce::AudioParameterFloat *pulseWidth = nullptr, *attack = nullptr, *decay = nullptr, *sustain = nullptr,
                              *release = nullptr, *level = nullptr, *tune = nullptr;

    //---- state ---------------------------------------------------------------
    double sr = 44100.0, phase = 0.0, baseFreq = 440.0, bendSemis = 0.0;
    float  env = 0.0f, velocity = 1.0f;
    int    note = -1;
    Stage  stage = Idle;
    bool   held = false, sustainDown = false, pendingRelease = false;
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InternalOscillator)
};
