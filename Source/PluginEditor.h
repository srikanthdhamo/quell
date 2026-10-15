#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class QuellLookAndFeel : public juce::LookAndFeel_V4
{
public:
    QuellLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
};

// Spectrum + reduction display with a draggable EQ-style sensitivity curve on top
class CurveDisplay : public juce::Component, private juce::Timer
{
public:
    explicit CurveDisplay (QuellProcessor&);
    void paint (juce::Graphics&) override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override { repaint(); }

    float xForFreq (float f) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;

    SensitivityCurve currentCurve() const;
    juce::Point<float> nodePosition (int band, const SensitivityCurve&) const;
    int hitTestNode (juce::Point<float>) const;

    juce::RangedAudioParameter* param (const juce::String& id) const { return proc.apvts.getParameter (id); }
    static void setParam (juce::RangedAudioParameter*, float value);

    QuellProcessor& proc;
    int hovered = -1, dragging = -1;
};

class QuellEditor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    explicit QuellEditor (QuellProcessor&);
    ~QuellEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void addKnob (const juce::String& id, const juce::String& name);
    void timerCallback() override;
    juce::String latencyText() const;

    QuellProcessor& proc;
    QuellLookAndFeel lnf;
    CurveDisplay display;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::TextButton deltaButton { "DELTA" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deltaAttachment;
    juce::TextButton lowLatButton { "LOW LATENCY" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lowLatAttachment;
    juce::String shownLatency;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuellEditor)
};
