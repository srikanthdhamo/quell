// Renders the plugin editor to a PNG after processing some test audio.
// Usage: QuellSnapshot out.png
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outFile (argc > 1 ? juce::String (argv[1]) : juce::String ("quell.png"));

    QuellProcessor proc;
    const double sr = 48000.0;
    const int block = 512;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto set = [&proc] (const juce::String& id, float v)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    // Example curve so the nodes are spread out
    set ("depth", 10.0f);
    set ("lc_freq", 60.0f);
    set ("ls_gain", -6.0f);
    set ("b1_gain", 4.0f);
    set ("b2_freq", 3000.0f); set ("b2_gain", 8.0f); set ("b2_q", 1.5f);
    set ("b3_gain", -5.0f);
    set ("hs_gain", 3.0f);
    set ("hc_freq", 16000.0f);

    // Pink-ish noise with a few resonances
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    float pink = 0.0f;
    double phase[3] = {};
    const double freqs[3] = { 350.0, 2950.0, 7200.0 };
    const float amps[3] = { 0.08f, 0.15f, 0.05f };
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buf (2, block);
    float peak = 0.0f;
    bool finite = true;
    auto processSeconds = [&] (double seconds)
    {
    for (int n = 0; n < (int) (sr * seconds) / block; ++n)
    {
        for (int i = 0; i < block; ++i)
        {
            pink = 0.97f * pink + 0.03f * nd (rng);
            float s = pink * 0.5f;
            for (int k = 0; k < 3; ++k)
            {
                phase[k] += juce::MathConstants<double>::twoPi * freqs[k] / sr;
                s += amps[k] * (float) std::sin (phase[k]);
            }
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
            {
                const float v = buf.getSample (ch, i);
                finite = finite && std::isfinite (v);
                peak = juce::jmax (peak, std::abs (v));
            }
    }
    };

    auto snapshot = [&proc] (const juce::File& file)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (image, os);
        std::printf ("wrote %s (%dx%d)\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    };

    processSeconds (2.0);
    std::printf ("quality mode latency: %d samples\n", proc.getLatencySamples());
    snapshot (outFile);

    // Switch to low latency mid-stream and keep processing
    set ("lowlat", 1.0f);
    processSeconds (2.0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);   // deliver the latency update
    std::printf ("low-latency mode latency: %d samples, output peak %.3f, finite %s\n",
                 proc.getLatencySamples(), peak, finite ? "yes" : "NO");
    snapshot (outFile.getSiblingFile (outFile.getFileNameWithoutExtension() + "-lowlat.png"));

    // And back again
    set ("lowlat", 0.0f);
    processSeconds (1.0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    std::printf ("back to quality: %d samples, output peak %.3f, finite %s\n",
                 proc.getLatencySamples(), peak, finite ? "yes" : "NO");
    return (finite && peak < 2.0f) ? 0 : 1;
}
