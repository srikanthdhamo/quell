#include "PluginProcessor.h"
#include "PluginEditor.h"

QuellProcessor::QuellProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    raw.depth       = apvts.getRawParameterValue ("depth");
    raw.sharpness   = apvts.getRawParameterValue ("sharpness");
    raw.selectivity = apvts.getRawParameterValue ("selectivity");
    raw.attack      = apvts.getRawParameterValue ("attack");
    raw.release     = apvts.getRawParameterValue ("release");
    raw.link        = apvts.getRawParameterValue ("link");
    raw.mix         = apvts.getRawParameterValue ("mix");
    raw.output      = apvts.getRawParameterValue ("output");
    raw.delta       = apvts.getRawParameterValue ("delta");
    raw.lowLatency  = apvts.getRawParameterValue ("lowlat");

    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        const auto& info = SensitivityCurve::info[(size_t) b];
        raw.bandFreq[(size_t) b] = apvts.getRawParameterValue (freqId (b));
        if (info.hasGain) raw.bandGain[(size_t) b] = apvts.getRawParameterValue (gainId (b));
        if (info.hasQ)    raw.bandQ[(size_t) b]    = apvts.getRawParameterValue (qId (b));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout QuellProcessor::createLayout()
{
    using namespace juce;
    using P = AudioParameterFloat;

    // Sensible display precision for every float parameter
    struct Attr : AudioParameterFloatAttributes
    {
        Attr() : AudioParameterFloatAttributes (AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        {
            const float a = std::abs (v);
            return String (v, a >= 100.0f ? 0 : (a >= 10.0f ? 1 : 2));
        })) {}
        Attr (const AudioParameterFloatAttributes& o) : AudioParameterFloatAttributes (o) {}
        Attr withLabel (const String& l) const { return Attr (AudioParameterFloatAttributes::withLabel (l)); }
    };

    auto freqRange = [] (float lo, float hi)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (1000.0f);
        return r;
    };
    auto msRange = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    };

    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<P> (ParameterID { "depth", 1 }, "Depth", NormalisableRange<float> (0.0f, 24.0f, 0.01f), 6.0f, Attr().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "sharpness", 1 }, "Sharpness", NormalisableRange<float> (1.0f, 10.0f, 0.01f), 4.0f));
    layout.add (std::make_unique<P> (ParameterID { "selectivity", 1 }, "Selectivity", NormalisableRange<float> (0.0f, 10.0f, 0.01f), 3.5f));
    layout.add (std::make_unique<P> (ParameterID { "attack", 1 }, "Attack", msRange (0.5f, 200.0f, 10.0f), 5.0f, Attr().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "release", 1 }, "Release", msRange (5.0f, 1000.0f, 80.0f), 60.0f, Attr().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "link", 1 }, "Stereo Link", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, Attr().withLabel ("%")));
    layout.add (std::make_unique<P> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, Attr().withLabel ("%")));
    layout.add (std::make_unique<P> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, Attr().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "delta", 1 }, "Delta", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "lowlat", 1 }, "Low Latency", false,
                                                      AudioParameterBoolAttributes().withAutomatable (false)));

    // Sensitivity EQ bands
    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        const auto& info = SensitivityCurve::info[(size_t) b];
        const String name (info.name);
        layout.add (std::make_unique<P> (ParameterID { freqId (b), 1 }, name + " Freq",
                                         freqRange (SensitivityCurve::minFreq, SensitivityCurve::maxFreq), info.defaultFreq, Attr().withLabel ("Hz")));
        if (info.hasGain)
            layout.add (std::make_unique<P> (ParameterID { gainId (b), 1 }, name + " Sensitivity",
                                             NormalisableRange<float> (-SensitivityCurve::maxGainDb, SensitivityCurve::maxGainDb, 0.01f), 0.0f, Attr().withLabel ("dB")));
        if (info.hasQ)
        {
            NormalisableRange<float> qr (SensitivityCurve::minQ, SensitivityCurve::maxQ);
            qr.setSkewForCentre (1.0f);
            layout.add (std::make_unique<P> (ParameterID { qId (b), 1 }, name + " Q", qr, 1.0f, Attr()));
        }
    }
    return layout;
}

void QuellProcessor::prepareToPlay (double sampleRate, int)
{
    const int chans = juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels());
    engines[Quality].prepare (sampleRate, chans, SpectralSuppressor::Mode::Quality);
    engines[LowLatency].prepare (sampleRate, chans, SpectralSuppressor::Mode::ZeroLatency);
    switchFadeLength = juce::roundToInt (sampleRate * 0.005);   // 5 ms
    fadeInRemaining = 0;

    activeMode.store (raw.lowLatency->load() > 0.5f ? LowLatency : Quality);
    setLatencySamples (engines[activeMode.load()].getLatencySamples());
}

void QuellProcessor::handleAsyncUpdate()
{
    // Tell the host about the new delay (message thread)
    setLatencySamples (engines[activeMode.load()].getLatencySamples());
}

bool QuellProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void QuellProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    SuppressorParams p;
    p.depthDb      = raw.depth->load();
    p.sharpness    = raw.sharpness->load();
    p.selectivity  = raw.selectivity->load();
    p.attackMs     = raw.attack->load();
    p.releaseMs    = raw.release->load();
    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        auto& band = p.curve.bands[(size_t) b];
        band.freq = raw.bandFreq[(size_t) b]->load();
        if (auto* g = raw.bandGain[(size_t) b]) band.gainDb = g->load();
        if (auto* q = raw.bandQ[(size_t) b])    band.q = q->load();
    }
    p.link         = raw.link->load() * 0.01f;
    p.mix          = raw.mix->load() * 0.01f;
    p.outGainDb    = raw.output->load();
    p.delta        = raw.delta->load() > 0.5f;

    const int wanted = raw.lowLatency->load() > 0.5f ? LowLatency : Quality;
    const int current = activeMode.load();

    if (wanted != current)
    {
        // Finish this block on the old engine with a fade-out, then hand over
        engines[current].process (buffer, p);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.applyGainRamp (ch, 0, buffer.getNumSamples(), 1.0f, 0.0f);

        engines[wanted].reset();
        activeMode.store (wanted);
        fadeInRemaining = switchFadeLength;
        triggerAsyncUpdate();
        return;
    }

    engines[current].process (buffer, p);

    // Fade the new engine in after a mode switch
    if (fadeInRemaining > 0)
    {
        const int n = buffer.getNumSamples();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                const int k = switchFadeLength - fadeInRemaining + i;
                d[i] *= k >= switchFadeLength ? 1.0f : (float) k / (float) switchFadeLength;
            }
        }
        fadeInRemaining = juce::jmax (0, fadeInRemaining - n);
    }
}

void QuellProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void QuellProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* QuellProcessor::createEditor()
{
    return new QuellEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new QuellProcessor();
}
