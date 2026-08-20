#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

// Suite convention (docs/branding.md, "GUI preview"): the editor preview
// committed at docs/gui-preview.png is GENERATED, never mocked up - the GUI
// test suite takes an offscreen snapshot of the real editor, writes it to
// build/gui-preview.png and asserts it is not blank. That file is what gets
// committed.
//
// Component::createComponentSnapshot() renders through JUCE's software
// renderer into an offscreen juce::Image, so it needs no native peer and
// works in this headless console binary (juce::ScopedJuceInitialiser_GUI is
// installed for the whole binary in tests/TestMain.cpp).
TEST_CASE ("The editor renders a non-blank offscreen preview", "[gui][preview]")
{
    CryptaAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    CryptaAudioProcessorEditor editor (processor);

    // A couple of switched-on lamps and a moved needle, so the preview shows
    // the editor in a live state rather than at bare defaults.
    if (auto* gateEnabled = processor.apvts.getParameter ("gateEnabled"))
        gateEnabled->setValueNotifyingHost (1.0f);

    if (auto* eqEnabled = processor.apvts.getParameter ("eqEnabled"))
        eqEnabled->setValueNotifyingHost (1.0f);

    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true);

    REQUIRE (image.isValid());
    CHECK (image.getWidth() == editor.getDesignWidth());
    CHECK (image.getHeight() == editor.getDesignHeight());

    // Non-blank: a snapshot of a failed render is a single flat colour, so
    // count distinct colours over a coarse grid and require real variety.
    std::map<juce::uint32, int> colourHistogram;

    for (int y = 0; y < image.getHeight(); y += 4)
        for (int x = 0; x < image.getWidth(); x += 4)
            ++colourHistogram[image.getPixelAt (x, y).getARGB()];

    INFO ("distinct colours in the snapshot: " << colourHistogram.size());
    CHECK (colourHistogram.size() > 100);

    // ...and no single colour may cover the whole image (a flat fill with a
    // few stray pixels would still clear the count above).
    int mostCommon = 0;

    for (const auto& entry : colourHistogram)
        mostCommon = juce::jmax (mostCommon, entry.second);

    const auto sampledPixels = (int) ((image.getWidth() / 4 + 1) * (image.getHeight() / 4 + 1));
    INFO ("most common colour covers " << (100.0 * mostCommon / sampledPixels) << "% of the sampled pixels");
    CHECK (mostCommon < sampledPixels * 9 / 10);

    // Written next to the test binary (i.e. build/gui-preview.png), not
    // relative to the working directory - so it lands in the same place
    // whether the suite is run through `ctest --test-dir build` or by
    // invoking ./build/Tests from the repository root, and never drops a
    // stray file into the source tree.
    const auto previewFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                 .getParentDirectory()
                                 .getChildFile ("gui-preview.png");
    previewFile.deleteFile();

    juce::FileOutputStream stream (previewFile);
    REQUIRE (stream.openedOk());

    juce::PNGImageFormat pngFormat;
    CHECK (pngFormat.writeImageToStream (image, stream));

    stream.flush();
    CHECK (previewFile.getSize() > 0);
}
