#include "PluginEditor.h"

namespace
{
    const juce::Colour bg       { 0xff15171c };
    const juce::Colour panel    { 0xff1d2027 };
    const juce::Colour accent   { 0xff6fd3c4 };
    const juce::Colour reduce   { 0xffff7a5c };
    const juce::Colour curveCol { 0xfff2e6c9 };
    const juce::Colour textCol  { 0xffc9ccd4 };
    const juce::Colour dimText  { 0xff7c818c };

    const juce::Colour bandColours[SensitivityCurve::NumBands] = {
        juce::Colour (0xff8f9bb3), juce::Colour (0xffe0b25a), juce::Colour (0xff7fd38b),
        juce::Colour (0xff5fb8f0), juce::Colour (0xffb58cf0), juce::Colour (0xfff07fae),
        juce::Colour (0xff8f9bb3)
    };

    constexpr float specMinDb = -90.0f, specMaxDb = 6.0f;   // spectrum range
    constexpr float maxRedDb = 24.0f;                       // reduction range
    constexpr float curveRangeDb = 15.0f;                   // curve shows +/- this
    constexpr float nodeRadius = 9.0f;
    constexpr float labelStrip = 18.0f;                     // frequency labels at the bottom

    juce::String formatFreq (float f)
    {
        return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String (juce::roundToInt (f)) + " Hz";
    }
}

//==============================================================================
QuellLookAndFeel::QuellLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, textCol);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
    setColour (juce::Label::textColourId, dimText);
    setColour (juce::TextButton::buttonColourId, panel);
    setColour (juce::TextButton::buttonOnColourId, reduce);
    setColour (juce::TextButton::textColourOffId, textCol);
    setColour (juce::TextButton::textColourOnId, bg);
}

void QuellLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider&)
{
    auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float lineW = 3.5f;
    const float arcR = radius - lineW;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (panel.brighter (0.25f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float knobR = arcR - 6.0f;
    g.setColour (panel.brighter (0.08f));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -knobR + 3.0f, 3.0f, knobR * 0.45f, 1.5f);
    g.setColour (textCol);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

//==============================================================================
CurveDisplay::CurveDisplay (QuellProcessor& p) : proc (p)
{
    startTimerHz (30);
}

float CurveDisplay::xForFreq (float f) const
{
    const float t = std::log (f / SensitivityCurve::minFreq) / std::log (SensitivityCurve::maxFreq / SensitivityCurve::minFreq);
    return t * (float) getWidth();
}

float CurveDisplay::freqForX (float x) const
{
    const float t = juce::jlimit (0.0f, 1.0f, x / (float) getWidth());
    return SensitivityCurve::minFreq * std::pow (SensitivityCurve::maxFreq / SensitivityCurve::minFreq, t);
}

float CurveDisplay::yForDb (float db) const
{
    const float top = nodeRadius + 4.0f, bottom = (float) getHeight() - labelStrip - nodeRadius;
    return juce::jmap (juce::jlimit (-curveRangeDb, curveRangeDb, db), curveRangeDb, -curveRangeDb, top, bottom);
}

float CurveDisplay::dbForY (float y) const
{
    const float top = nodeRadius + 4.0f, bottom = (float) getHeight() - labelStrip - nodeRadius;
    return juce::jmap (y, top, bottom, curveRangeDb, -curveRangeDb);
}

SensitivityCurve CurveDisplay::currentCurve() const
{
    SensitivityCurve c;
    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        const auto& info = SensitivityCurve::info[(size_t) b];
        auto& band = c.bands[(size_t) b];
        band.freq = proc.apvts.getRawParameterValue (QuellProcessor::freqId (b))->load();
        if (info.hasGain) band.gainDb = proc.apvts.getRawParameterValue (QuellProcessor::gainId (b))->load();
        if (info.hasQ)    band.q      = proc.apvts.getRawParameterValue (QuellProcessor::qId (b))->load();
    }
    return c;
}

juce::Point<float> CurveDisplay::nodePosition (int band, const SensitivityCurve& c) const
{
    const auto& v = c.bands[(size_t) band];
    const float x = juce::jlimit (nodeRadius, (float) getWidth() - nodeRadius, xForFreq (v.freq));
    const float y = SensitivityCurve::info[(size_t) band].hasGain ? yForDb (v.gainDb) : yForDb (0.0f);
    return { x, y };
}

int CurveDisplay::hitTestNode (juce::Point<float> pos) const
{
    const auto c = currentCurve();
    int best = -1;
    float bestDist = nodeRadius + 4.0f;
    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        const float d = nodePosition (b, c).getDistanceFrom (pos);
        if (d < bestDist) { bestDist = d; best = b; }
    }
    return best;
}

void CurveDisplay::setParam (juce::RangedAudioParameter* p, float value)
{
    if (p != nullptr)
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void CurveDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int h = hitTestNode (e.position);
    if (h != hovered)
    {
        hovered = h;
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void CurveDisplay::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

void CurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragging = hitTestNode (e.position);
    if (dragging < 0)
        return;

    if (auto* p = param (QuellProcessor::freqId (dragging))) p->beginChangeGesture();
    if (SensitivityCurve::info[(size_t) dragging].hasGain)
        if (auto* p = param (QuellProcessor::gainId (dragging))) p->beginChangeGesture();
}

void CurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    setParam (param (QuellProcessor::freqId (dragging)), freqForX (e.position.x));
    if (SensitivityCurve::info[(size_t) dragging].hasGain)
        setParam (param (QuellProcessor::gainId (dragging)),
                  juce::jlimit (-SensitivityCurve::maxGainDb, SensitivityCurve::maxGainDb, dbForY (e.position.y)));
    repaint();
}

void CurveDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragging < 0)
        return;

    if (auto* p = param (QuellProcessor::freqId (dragging))) p->endChangeGesture();
    if (SensitivityCurve::info[(size_t) dragging].hasGain)
        if (auto* p = param (QuellProcessor::gainId (dragging))) p->endChangeGesture();
    dragging = -1;
}

void CurveDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = hitTestNode (e.position);
    if (b < 0)
        return;

    const auto& info = SensitivityCurve::info[(size_t) b];
    auto reset = [this] (const juce::String& id)
    {
        if (auto* p = param (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
    };

    // Bells and shelves reset their sensitivity; cuts park back at the edge
    if (info.hasGain) reset (QuellProcessor::gainId (b));
    else              reset (QuellProcessor::freqId (b));
    if (info.hasQ)    reset (QuellProcessor::qId (b));
}

void CurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int b = dragging >= 0 ? dragging : hitTestNode (e.position);
    if (b < 0 || ! SensitivityCurve::info[(size_t) b].hasQ)
        return;

    if (auto* p = param (QuellProcessor::qId (b)))
    {
        const float q = p->convertFrom0to1 (p->getValue()) * std::exp2 (wheel.deltaY * 2.0f);
        p->beginChangeGesture();
        setParam (p, juce::jlimit (SensitivityCurve::minQ, SensitivityCurve::maxQ, q));
        p->endChangeGesture();
    }
}

void CurveDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const float w = area.getWidth(), h = area.getHeight();
    const float plotH = h - labelStrip;

    g.setColour (panel);
    g.fillRoundedRectangle (area, 6.0f);

    // Frequency grid
    g.setFont (11.0f);
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForFreq (f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawVerticalLine ((int) x, 0.0f, plotH);
        g.setColour (dimText);
        g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f) + "k" : juce::String ((int) f),
                    (int) x + 3, (int) (h - labelStrip), 40, (int) labelStrip, juce::Justification::centredLeft);
    }

    // Sensitivity grid lines
    for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
    {
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        g.drawHorizontalLine ((int) yForDb (db), 0.0f, w);
    }
    const float zeroY = yForDb (0.0f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    const float dashes[] = { 4.0f, 4.0f };
    g.drawDashedLine ({ 0.0f, zeroY, w, zeroY }, dashes, 2, 1.0f);

    constexpr int n = SpectralSuppressor::displayPoints;
    auto xAt = [w] (int i) { return (float) i / (float) (SpectralSuppressor::displayPoints - 1) * w; };

    // Input spectrum
    juce::Path spec;
    spec.startNewSubPath (0.0f, plotH);
    for (int i = 0; i < n; ++i)
    {
        const float db = juce::jlimit (specMinDb, specMaxDb, proc.activeEngine().displayInputDb[(size_t) i].load (std::memory_order_relaxed));
        spec.lineTo (xAt (i), juce::jmap (db, specMinDb, specMaxDb, plotH, 0.0f));
    }
    spec.lineTo (w, plotH);
    spec.closeSubPath();
    g.setColour (accent.withAlpha (0.14f));
    g.fillPath (spec);
    g.setColour (accent.withAlpha (0.55f));
    g.strokePath (spec, juce::PathStrokeType (1.1f));

    // Reduction, hanging down from the top
    juce::Path red;
    red.startNewSubPath (0.0f, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float r = juce::jlimit (0.0f, maxRedDb, proc.activeEngine().displayReductionDb[(size_t) i].load (std::memory_order_relaxed));
        red.lineTo (xAt (i), r / maxRedDb * plotH * 0.5f);
    }
    red.lineTo (w, 0.0f);
    red.closeSubPath();
    g.setColour (reduce.withAlpha (0.4f));
    g.fillPath (red);
    g.setColour (reduce);
    g.strokePath (red, juce::PathStrokeType (1.3f));

    // Sensitivity curve
    const auto curve = currentCurve();
    juce::Path line, fill;
    const int steps = juce::jmax (64, (int) w / 2);
    for (int i = 0; i <= steps; ++i)
    {
        const float x = w * (float) i / (float) steps;
        const float y = yForDb (curve.displayDb (freqForX (x)));
        if (i == 0) { line.startNewSubPath (x, y); fill.startNewSubPath (x, zeroY); }
        line.lineTo (x, y);
        fill.lineTo (x, y);
    }
    fill.lineTo (w, zeroY);
    fill.closeSubPath();
    g.setColour (curveCol.withAlpha (0.08f));
    g.fillPath (fill);
    g.setColour (curveCol.withAlpha (0.9f));
    g.strokePath (line, juce::PathStrokeType (2.0f));

    // Selected bell width indicator
    const int active = dragging >= 0 ? dragging : hovered;
    if (active >= 0 && SensitivityCurve::info[(size_t) active].hasQ)
    {
        const auto& band = curve.bands[(size_t) active];
        const float halfOct = 0.6f / band.q * 1.1774f;   // half width at half height
        const float x0 = xForFreq (band.freq * std::exp2 (-halfOct)), x1 = xForFreq (band.freq * std::exp2 (halfOct));
        g.setColour (bandColours[active].withAlpha (0.08f));
        g.fillRect (x0, 0.0f, x1 - x0, plotH);
    }

    // Nodes
    for (int b = 0; b < SensitivityCurve::NumBands; ++b)
    {
        const auto pos = nodePosition (b, curve);
        const auto col = bandColours[b];
        const bool hot = (b == active);
        const float r = hot ? nodeRadius + 2.0f : nodeRadius;

        g.setColour (bg.withAlpha (0.8f));
        g.fillEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f);
        g.setColour (col.withAlpha (hot ? 1.0f : 0.85f));
        g.drawEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f, hot ? 2.5f : 1.8f);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (SensitivityCurve::info[(size_t) b].shortName,
                    juce::Rectangle<float> (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f), juce::Justification::centred);
    }

    // Readout for the active node, or a usage hint
    g.setFont (juce::FontOptions (12.0f));
    if (active >= 0)
    {
        const auto& info = SensitivityCurve::info[(size_t) active];
        const auto& band = curve.bands[(size_t) active];
        juce::String text = juce::String (info.name) + "   " + formatFreq (band.freq);
        if (info.hasGain) text << "   " << (band.gainDb >= 0.0f ? "+" : "") << juce::String (band.gainDb, 1) << " dB";
        if (info.hasQ)    text << "   Q " << juce::String (band.q, 2);
        g.setColour (bandColours[active]);
        g.drawText (text, 10, 6, 400, 16, juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (dimText);
        g.drawText ("drag nodes to shape sensitivity  -  scroll: Q  -  double-click: reset",
                    10, 6, 500, 16, juce::Justification::centredLeft);
    }
    g.setColour (dimText);
    g.drawText ("reduction", getLocalBounds().withHeight (22).reduced (10, 0), juce::Justification::centredRight);
}

//==============================================================================
QuellEditor::QuellEditor (QuellProcessor& p)
    : AudioProcessorEditor (&p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    addKnob ("depth", "DEPTH");
    addKnob ("sharpness", "SHARPNESS");
    addKnob ("selectivity", "SELECTIVITY");
    addKnob ("attack", "ATTACK");
    addKnob ("release", "RELEASE");
    addKnob ("link", "LINK");
    addKnob ("mix", "MIX");
    addKnob ("output", "OUTPUT");

    deltaButton.setClickingTogglesState (true);
    deltaButton.setTooltip ("Listen only to what is being removed");
    addAndMakeVisible (deltaButton);
    deltaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "delta", deltaButton);

    lowLatButton.setClickingTogglesState (true);
    lowLatButton.setTooltip ("Zero-latency mode: the audio is filtered live by a minimum-phase filter (0 ms instead of ~43 ms). Reduction reacts slightly later since there is no look-ahead.");
    lowLatButton.setColour (juce::TextButton::buttonOnColourId, accent);
    addAndMakeVisible (lowLatButton);
    lowLatAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "lowlat", lowLatButton);

    setResizable (true, true);
    setResizeLimits (720, 480, 1600, 1100);
    shownLatency = latencyText();
    setSize (920, 580);
    startTimerHz (4);
}

juce::String QuellEditor::latencyText() const
{
    const double sr = proc.getSampleRate();
    const int samples = proc.activeEngine().getLatencySamples();
    if (sr <= 0.0)
        return {};
    return (proc.isLowLatencyActive() ? "low latency  " : "quality  ")
         + juce::String (1000.0 * samples / sr, 1) + " ms  (" + juce::String (samples) + " smp)";
}

void QuellEditor::timerCallback()
{
    const auto text = latencyText();
    if (text != shownLatency)
    {
        shownLatency = text;
        repaint (getLocalBounds().withHeight (46));
    }
}

QuellEditor::~QuellEditor()
{
    for (auto& k : knobs)
        k->slider.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void QuellEditor::addKnob (const juce::String& id, const juce::String& name)
{
    auto k = std::make_unique<Knob>();
    k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setLookAndFeel (&lnf);
    k->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k->slider.setColour (juce::Slider::textBoxTextColourId, textCol);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    k->label.setText (name, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    knobs.push_back (std::move (k));
}

void QuellEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (textCol);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("QUELL", 16, 8, 200, 30, juce::Justification::centredLeft);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("dynamic resonance suppressor   v" JucePlugin_VersionString, 100, 8, 300, 30, juce::Justification::centredLeft);

    g.drawText (shownLatency, lowLatButton.getX() - 260, 8, 250, 30, juce::Justification::centredRight);
}

void QuellEditor::resized()
{
    auto area = getLocalBounds().reduced (14);
    auto header = area.removeFromTop (32);
    lowLatButton.setBounds (header.removeFromRight (120).withSizeKeepingCentre (120, 26).translated (0, -4));

    auto controls = area.removeFromBottom (112);
    area.removeFromBottom (10);
    display.setBounds (area);

    const int cells = (int) knobs.size() + 1;
    const int cellW = controls.getWidth() / cells;

    for (int i = 0; i < (int) knobs.size(); ++i)
    {
        auto cell = juce::Rectangle<int> (controls.getX() + i * cellW, controls.getY(), cellW, controls.getHeight());
        auto& k = *knobs[(size_t) i];
        k.label.setBounds (cell.removeFromTop (16));
        k.slider.setBounds (cell.reduced (4, 0));
    }

    deltaButton.setBounds (juce::Rectangle<int> (controls.getX() + (int) knobs.size() * cellW, controls.getY(), cellW, controls.getHeight())
                               .withSizeKeepingCentre (cellW - 20, 34));
}
