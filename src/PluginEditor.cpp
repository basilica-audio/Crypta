#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

#include <algorithm>
#include <cmath>

namespace
{
    // ----- Gilded typography styles (PlateTypography, EB Garamond) --------
    // Raised warm-gold lettering with a soft drop shadow below - the
    // tenebrae "gilded lettering on dark ground" convention (its
    // EditorTypographyTests fixture uses the same ink), correct for this
    // near-black gloss plate; requiem's dark-ink engraved read is for
    // bright metal grounds only.
    constexpr juce::uint32 wordmarkInk = 0xf0d9ae62;
    constexpr juce::uint32 captionInk = 0xb3c9a05a;
    constexpr juce::uint32 sectionInk = 0xe6c9a05a;
    constexpr juce::uint32 controlInk = 0xd8c39a55;
    constexpr juce::uint32 letterShadow = 0x99000000;

    constexpr float controlLabelHeight1x = 10.5f;

    // Engraved section rules: a dark incision line with a lit gold lip
    // below, the same lighting logic as the lettering.
    constexpr juce::uint32 ruleInk = 0xcc10100e;
    constexpr juce::uint32 ruleLip = 0x59c9a05a;

    // Peak metering floor for the VU pair: a silent block maps here and
    // the needle rests on the dial's -20 stop.
    constexpr float peakMeterFloorDb = -60.0f;

    // M2 i18n frame: selects German (resources/i18n/de.txt) or falls
    // through to English, once, at editor construction. `presetBar` is a
    // member initialised via the constructor's initialiser list, and its
    // own constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order, so this helper (called from
    // presetBar's own initialiser expression below) is what guarantees
    // installLocalisation() runs before presetBar exists.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (CryptaAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }

    float linearPeakToDb (float linearPeak)
    {
        return juce::Decibels::gainToDecibels (linearPeak, peakMeterFloorDb);
    }
}

//==============================================================================
const juce::Identifier& CryptaAudioProcessorEditor::getScaleStatePropertyId() noexcept
{
    static const juce::Identifier id ("editorScale");
    return id;
}

int CryptaAudioProcessorEditor::readPersistedScaleStepIndex (const juce::ValueTree& state) noexcept
{
    if (! state.hasProperty (getScaleStatePropertyId()))
        return defaultScaleStepIndex;

    const auto stored = (double) state.getProperty (getScaleStatePropertyId());

    if (! std::isfinite (stored) || stored <= 0.0)
        return defaultScaleStepIndex;

    // Snap to the nearest step - the previous editor generation persisted
    // a continuous scale in the same slot.
    int best = defaultScaleStepIndex;
    double bestDistance = std::numeric_limits<double>::max();

    for (size_t i = 0; i < scaleSteps.size(); ++i)
    {
        const auto distance = std::abs (stored - (double) scaleSteps[i]);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = (int) i;
        }
    }

    return best;
}

//==============================================================================
CryptaAudioProcessorEditor::Manifest CryptaAudioProcessorEditor::parseLayoutManifest()
{
    Manifest result;

    int jsonSize = 0;
    const auto* jsonData = BinaryData::getNamedResource ("layoutmanifest_json", jsonSize);
    jassert (jsonData != nullptr);

    if (jsonData == nullptr)
        return result;

    const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (jsonData, jsonSize));
    jassert (parsed.isObject());

    if (! parsed.isObject())
        return result;

    if (const auto* plate = parsed.getProperty ("plate", {}).getDynamicObject())
    {
        result.plateBinary = plate->getProperty ("binary").toString();
        result.plateWidth1x = (int) plate->getProperty ("width1x");
        result.plateHeight1x = (int) plate->getProperty ("height1x");
    }

    if (const auto* sprites = parsed.getProperty ("sprites", {}).getDynamicObject())
    {
        for (const auto& [name, value] : sprites->getProperties())
        {
            if (const auto* obj = value.getDynamicObject())
            {
                SpriteSpec spec;
                spec.binary = obj->getProperty ("binary").toString();
                spec.width = (float) obj->getProperty ("width");
                spec.height = (float) obj->getProperty ("height");
                spec.knobCx = (float) obj->getProperty ("knobCx");
                spec.knobCy = (float) obj->getProperty ("knobCy");
                spec.knobRadius = (float) obj->getProperty ("knobRadius");
                spec.contentDiameter = (float) obj->getProperty ("contentDiameter");
                spec.pivotXFrac = (float) obj->getProperty ("pivotXFrac");
                spec.pivotYFrac = (float) obj->getProperty ("pivotYFrac");
                spec.needleLengthFrac = (float) obj->getProperty ("needleLengthFrac");
                result.sprites[name.toString()] = spec;
            }
        }
    }

    if (const auto* ticks = parsed.getProperty ("vuTicks", {}).getArray())
        for (const auto& pair : *ticks)
            if (const auto* entry = pair.getArray(); entry != nullptr && entry->size() == 2)
                result.vuTicks.push_back ({ (float) (*entry)[0], (float) (*entry)[1] });

    if (const auto* controls = parsed.getProperty ("controls", {}).getArray())
    {
        for (const auto& value : *controls)
        {
            if (const auto* obj = value.getDynamicObject())
            {
                ControlSpec spec;
                spec.id = obj->getProperty ("id").toString();
                spec.type = obj->getProperty ("type").toString();
                spec.label = obj->getProperty ("label").toString();
                spec.tap = obj->getProperty ("tap").toString();
                spec.cx = (float) obj->getProperty ("cx");
                spec.cy = (float) obj->getProperty ("cy");
                spec.size = (float) obj->getProperty ("size");
                spec.sweep = obj->hasProperty ("sweep") ? (float) obj->getProperty ("sweep") : 270.0f;
                result.controls.push_back (std::move (spec));
            }
        }
    }

    if (const auto* labels = parsed.getProperty ("labels", {}).getArray())
    {
        for (const auto& value : *labels)
        {
            if (const auto* obj = value.getDynamicObject())
            {
                LabelSpec spec;
                spec.text = obj->getProperty ("text").toString();
                spec.style = obj->getProperty ("style").toString();
                spec.cx = (float) obj->getProperty ("cx");
                spec.cy = (float) obj->getProperty ("cy");
                spec.h = (float) obj->getProperty ("h");
                result.labels.push_back (std::move (spec));
            }
        }
    }

    if (const auto* rules = parsed.getProperty ("rules", {}).getArray())
        for (const auto& value : *rules)
            if (const auto* obj = value.getDynamicObject())
                result.rules.push_back ({ (float) obj->getProperty ("x1"), (float) obj->getProperty ("y1"),
                                          (float) obj->getProperty ("x2"), (float) obj->getProperty ("y2") });

    return result;
}

juce::Image CryptaAudioProcessorEditor::imageForBinary (const juce::String& binaryName) const
{
    int dataSize = 0;
    const auto* data = BinaryData::getNamedResource (binaryName.toRawUTF8(), dataSize);
    jassert (data != nullptr);

    if (data == nullptr)
        return {};

    return juce::ImageCache::getFromMemory (data, dataSize);
}

//==============================================================================
CryptaAudioProcessorEditor::CryptaAudioProcessorEditor (CryptaAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (processorToEdit),
      audioProcessor (processorToEdit),
      manifest (parseLayoutManifest()),
      plateImage (imageForBinary (manifest.plateBinary)),
      typography (BinaryData::EBGaramondRegular_ttf, BinaryData::EBGaramondRegular_ttfSize,
                  BinaryData::EBGaramondSemiBold_ttf, BinaryData::EBGaramondSemiBold_ttfSize),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    jassert (plateImage.isValid());
    jassert (manifest.plateWidth1x > 0 && manifest.plateHeight1x > 0);

    designWidth = manifest.plateWidth1x;
    designHeight = topStripHeight1x + manifest.plateHeight1x;

    // FOCUS ORDER: preset bar and scale button first, then every control
    // in manifest (signal-flow) order.
    addAndMakeVisible (presetBar);

    scaleButton.setComponentID ("scaleButton");
    scaleButton.onClick = [this] { cycleScale(); };
    addAndMakeVisible (scaleButton);

    buildControlsFromManifest();

    applyScaleStep (readPersistedScaleStepIndex (audioProcessor.apvts.state));

    startTimerHz (meterRefreshHz);
}

CryptaAudioProcessorEditor::~CryptaAudioProcessorEditor() = default;

void CryptaAudioProcessorEditor::buildControlsFromManifest()
{
    auto& apvts = audioProcessor.apvts;

    // Sprite images decode once and are shared by every control that uses
    // the same sheet (juce::Image is COW-shared internally).
    std::map<juce::String, juce::Image> spriteImages;

    for (const auto& [name, spec] : manifest.sprites)
        spriteImages[name] = imageForBinary (spec.binary);

    for (const auto& spec : manifest.controls)
    {
        if (spec.type == "knob" || spec.type == "selector")
        {
            const auto& sprite = manifest.sprites.at (spec.type == "knob" ? "knob" : "selector");
            const auto halfSweep = spec.sweep * 0.5f;

            KnobControl control;
            control.spec = spec;
            control.slider = std::make_unique<basilica::gui::SpriteKnob> (
                spriteImages.at (spec.type == "knob" ? "knob" : "selector"),
                juce::Point<float> (sprite.knobCx, sprite.knobCy), sprite.knobRadius,
                -halfSweep, halfSweep);

            control.slider->setTitle (spec.label);
            control.slider->setName (spec.id);

            if (auto* parameter = apvts.getParameter (spec.id))
                control.slider->textFromValueFunction = [parameter] (double value)
                {
                    return parameter->getText (parameter->convertTo0to1 ((float) value), 0);
                };

            addAndMakeVisible (*control.slider);
            control.attachment = std::make_unique<SliderAttachment> (apvts, spec.id, *control.slider);
            knobs.push_back (std::move (control));
        }
        else if (spec.type == "toggle")
        {
            ToggleControl control;
            control.spec = spec;
            control.button = std::make_unique<basilica::gui::SpriteToggle> (spriteImages.at ("toggle"));
            control.button->setTitle (spec.label);
            control.button->setName (spec.id);

            addAndMakeVisible (*control.button);
            control.attachment = std::make_unique<ButtonAttachment> (apvts, spec.id, *control.button);
            toggles.push_back (std::move (control));
        }
        else if (spec.type == "vu")
        {
            const auto& sprite = manifest.sprites.at ("vu");

            MeterControl control;
            control.spec = spec;
            control.dial = std::make_unique<basilica::gui::NeedleDial> (
                spriteImages.at ("vu"),
                spec.tap == "inputPeak" ? juce::String ("Input peak level meter")
                                        : juce::String ("Output peak level meter"),
                sprite.pivotXFrac, sprite.pivotYFrac, sprite.needleLengthFrac, manifest.vuTicks);

            addAndMakeVisible (*control.dial);
            meters.push_back (std::move (control));
        }
        else
        {
            jassertfalse; // unknown control type in the manifest
        }
    }

    // Seed each needle's initial pose from the live tap so the editor
    // opens with honest readings instead of a ramp from the floor.
    updateMetersFromProcessor (1.0e6f);
}

//==============================================================================
void CryptaAudioProcessorEditor::applyScaleStep (int newStepIndex)
{
    scaleStepIndex = juce::jlimit (0, (int) scaleSteps.size() - 1, newStepIndex);

    const auto percentText = juce::String ((int) std::lround (scaleSteps[(size_t) scaleStepIndex] * 100.0f)) + "%";
    scaleButton.setButtonText (percentText);
    scaleButton.setTitle ("Window scale, " + percentText);

    audioProcessor.apvts.state.setProperty (getScaleStatePropertyId(),
                                            (double) scaleSteps[(size_t) scaleStepIndex], nullptr);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    setSize ((int) std::lround ((float) designWidth * scale),
             (int) std::lround ((float) designHeight * scale));
}

void CryptaAudioProcessorEditor::cycleScale()
{
    applyScaleStep ((scaleStepIndex + 1) % (int) scaleSteps.size());
}

//==============================================================================
void CryptaAudioProcessorEditor::resized()
{
    const auto scale = getEditorScale();
    const auto s = [scale] (float value1x) { return (int) std::lround (value1x * scale); };

    auto topStrip = getLocalBounds().removeFromTop (s ((float) topStripHeight1x));
    scaleButton.setBounds (topStrip.removeFromRight (s (64.0f)).reduced (0, s (4.0f)));
    presetBar.setBounds (topStrip.reduced (0, s (2.0f)));

    const auto plateOriginY = (float) s ((float) topStripHeight1x);

    const auto place = [&] (const ControlSpec& spec, float frameW, float frameH,
                            float anchorX, float anchorY, float drawScale1x)
    {
        // drawScale1x: sprite px -> design px. anchorX/anchorY: the point
        // within the sprite frame that must land on (cx, cy).
        const auto totalScale = drawScale1x * scale;
        const auto x = spec.cx * scale - anchorX * totalScale;
        const auto y = spec.cy * scale + plateOriginY - anchorY * totalScale;

        return juce::Rectangle<int> ((int) std::lround (x), (int) std::lround (y),
                                     (int) std::lround (frameW * totalScale),
                                     (int) std::lround (frameH * totalScale));
    };

    for (auto& control : knobs)
    {
        const auto& sprite = manifest.sprites.at (control.spec.type == "knob" ? "knob" : "selector");
        const auto drawScale = control.spec.size / (2.0f * sprite.knobRadius);
        control.slider->setBounds (place (control.spec, sprite.width, sprite.height,
                                          sprite.knobCx, sprite.knobCy, drawScale));
    }

    for (auto& control : toggles)
    {
        const auto& sprite = manifest.sprites.at ("toggle");
        const auto drawScale = control.spec.size / sprite.height;
        control.button->setBounds (place (control.spec, sprite.width, sprite.height,
                                          sprite.width * 0.5f, sprite.height * 0.5f, drawScale));
    }

    for (auto& control : meters)
    {
        const auto& sprite = manifest.sprites.at ("vu");
        const auto drawScale = control.spec.size / sprite.contentDiameter;
        control.dial->setBounds (place (control.spec, sprite.width, sprite.height,
                                        sprite.width * 0.5f, sprite.height * 0.5f, drawScale));
    }
}

//==============================================================================
void CryptaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0a09));

    const auto scale = getEditorScale();
    const auto plateOriginY = std::lround ((float) topStripHeight1x * scale);

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (plateImage,
                 juce::Rectangle<float> (0.0f, (float) plateOriginY,
                                         (float) manifest.plateWidth1x * scale,
                                         (float) manifest.plateHeight1x * scale));

    drawPlateTypography (g, scale, (float) plateOriginY);
}

void CryptaAudioProcessorEditor::drawPlateTypography (juce::Graphics& g, float scale, float plateOriginY) const
{
    using basilica::gui::EngravedTextStyle;

    const auto boxAt = [&] (float cx, float cy, float w1x, float h1x)
    {
        return juce::Rectangle<float> (cx * scale - w1x * scale * 0.5f,
                                       cy * scale + plateOriginY - h1x * scale * 0.5f,
                                       w1x * scale, h1x * scale);
    };

    for (const auto& label : manifest.labels)
    {
        EngravedTextStyle style { juce::Colour (sectionInk), juce::Colour (letterShadow), label.h, 0.24f, true };

        if (label.style == "wordmark")
            style = { juce::Colour (wordmarkInk), juce::Colour (letterShadow), label.h, 0.32f, true };
        else if (label.style == "caption")
            style = { juce::Colour (captionInk), juce::Colour (letterShadow), label.h, 0.46f, false };

        typography.drawEngraved (g, label.text, boxAt (label.cx, label.cy, 420.0f, label.h + 8.0f), scale, style);
    }

    const EngravedTextStyle controlStyle { juce::Colour (controlInk), juce::Colour (letterShadow),
                                           controlLabelHeight1x, 0.16f, false };

    for (const auto& control : manifest.controls)
    {
        if (control.label.isEmpty())
            continue;

        const auto labelGap1x = control.type == "vu" ? 16.0f : 13.0f;
        const auto labelCy = control.cy + control.size * 0.5f + labelGap1x;
        typography.drawEngraved (g, control.label, boxAt (control.cx, labelCy, 110.0f, 14.0f), scale, controlStyle);
    }

    // Engraved section rules: incision line + lit lip below (the plate's
    // own pinstripe language - separators only, never decoration without
    // grouping meaning).
    for (const auto& rule : manifest.rules)
    {
        const auto x1 = rule.x1 * scale;
        const auto y1 = rule.y1 * scale + plateOriginY;
        const auto x2 = rule.x2 * scale;
        const auto y2 = rule.y2 * scale + plateOriginY;
        const auto lip = juce::jmax (1.0f, scale);

        g.setColour (juce::Colour (ruleLip));

        if (std::abs (x2 - x1) < std::abs (y2 - y1))
            g.drawLine (x1 + lip, y1, x2 + lip, y2, juce::jmax (1.0f, scale * 0.8f));
        else
            g.drawLine (x1, y1 + lip, x2, y2 + lip, juce::jmax (1.0f, scale * 0.8f));

        g.setColour (juce::Colour (ruleInk));
        g.drawLine (x1, y1, x2, y2, juce::jmax (1.0f, scale * 0.8f));
    }
}

//==============================================================================
void CryptaAudioProcessorEditor::updateMetersFromProcessor (float dtSeconds)
{
    const auto& taps = audioProcessor.getMeterTaps();

    for (auto& meter : meters)
    {
        float db = peakMeterFloorDb;

        if (meter.spec.tap == "inputPeak")
            db = linearPeakToDb (juce::jmax (taps.inputPeakLeft.load (std::memory_order_relaxed),
                                             taps.inputPeakRight.load (std::memory_order_relaxed)));
        else if (meter.spec.tap == "outputPeak")
            db = linearPeakToDb (juce::jmax (taps.outputPeakLeft.load (std::memory_order_relaxed),
                                             taps.outputPeakRight.load (std::memory_order_relaxed)));

        meter.dial->setTargetDb (db);
        meter.dial->tick (dtSeconds);
    }
}

void CryptaAudioProcessorEditor::timerCallback()
{
    updateMetersFromProcessor (1.0f / (float) meterRefreshHz);
}
