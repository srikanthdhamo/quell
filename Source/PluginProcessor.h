#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "SpectralSuppressor.h"

class QuellProcessor : public juce::AudioProcessor,
                       private juce::AsyncUpdater
{
public:
    QuellProcessor();
    ~QuellProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    static juce::String freqId (int band) { return juce::String (SensitivityCurve::info[(size_t) band].id) + "_freq"; }
    static juce::String gainId (int band) { return juce::String (SensitivityCurve::info[(size_t) band].id) + "_gain"; }
    static juce::String qId    (int band) { return juce::String (SensitivityCurve::info[(size_t) band].id) + "_q"; }

    juce::AudioProcessorValueTreeState apvts;

    enum Mode { Quality = 0, LowLatency = 1 };

    // The engine currently producing audio (the GUI reads its display data)
    const SpectralSuppressor& activeEngine() const noexcept { return engines[activeMode.load()]; }
    bool isLowLatencyActive() const noexcept { return activeMode.load() == LowLatency; }

private:
    void handleAsyncUpdate() override;

    // Both engines stay prepared so switching modes never allocates on the audio thread
    SpectralSuppressor engines[2];
    std::atomic<int> activeMode { Quality };
    int switchFadeLength = 240, fadeInRemaining = 0;   // audio thread only

    struct
    {
        std::atomic<float>* depth; std::atomic<float>* sharpness; std::atomic<float>* selectivity;
        std::atomic<float>* attack; std::atomic<float>* release; std::atomic<float>* link;
        std::atomic<float>* mix; std::atomic<float>* output; std::atomic<float>* delta;
        std::atomic<float>* lowLatency;
        std::array<std::atomic<float>*, SensitivityCurve::NumBands> bandFreq {}, bandGain {}, bandQ {};
    } raw {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuellProcessor)
};
