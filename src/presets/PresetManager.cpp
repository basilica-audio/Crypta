#include "PresetManager.h"

#include <algorithm>

// JSON preset file format ("basilica-preset-1"):
//
//   {
//     "format": "basilica-preset-1",
//     "plugin": "com.yvesvogl.nave",
//     "pluginVersion": "0.2.0",
//     "name": "Preset Name",
//     "category": "Init|Guitar|...",
//     "parameters": { "<paramID>": <plain (unnormalised) value>, ... }
//   }
//
// See .scaffold/specs/preset-system-m2.md for the full binding spec this
// implements.

namespace basilica::presets
{
    namespace
    {
        constexpr const char* formatKey = "format";
        constexpr const char* pluginKey = "plugin";
        constexpr const char* pluginVersionKey = "pluginVersion";
        constexpr const char* nameKey = "name";
        constexpr const char* categoryKey = "category";
        constexpr const char* parametersKey = "parameters";
        constexpr const char* defaultPresetName = "Default";

        juce::String legalFileStem (const juce::String& name)
        {
            return juce::File::createLegalFileName (name);
        }

        // Compares two dotted numeric version strings component-wise, padding
        // the shorter with zeros ("0.3" == "0.3.0"). Non-numeric trailers are
        // ignored by getIntValue()'s leading-digits parse, so "0.3.0-rc1"
        // compares equal to "0.3.0" - deliberately lenient: this only gates a
        // compatibility back-fill, and treating a pre-release as its release
        // is the safer of the two mistakes.
        //
        // Returns true when `candidate` is strictly older than `reference`.
        // An empty/absent candidate counts as older than anything, which is
        // exactly what an ancient preset with no pluginVersion key should be.
        bool isVersionOlderThan (const juce::String& candidate, const juce::String& reference)
        {
            if (candidate.isEmpty())
                return true;

            juce::StringArray candidateParts, referenceParts;
            candidateParts.addTokens (candidate, ".", {});
            referenceParts.addTokens (reference, ".", {});

            const auto numParts = juce::jmax (candidateParts.size(), referenceParts.size());

            for (int part = 0; part < numParts; ++part)
            {
                const auto candidateValue = part < candidateParts.size() ? candidateParts[part].getIntValue() : 0;
                const auto referenceValue = part < referenceParts.size() ? referenceParts[part].getIntValue() : 0;

                if (candidateValue != referenceValue)
                    return candidateValue < referenceValue;
            }

            return false;
        }
    }

    //==========================================================================
    PresetManager::PresetManager (juce::AudioProcessorValueTreeState& stateToControl,
                                   PresetManagerConfig configToUse,
                                   std::vector<FactoryPresetAsset> factoryAssets)
        : apvts (stateToControl), config (std::move (configToUse))
    {
        for (auto& asset : factoryAssets)
        {
            if (asset.data == nullptr || asset.dataSize <= 0)
                continue;

            const auto jsonText = juce::String::fromUTF8 (asset.data, asset.dataSize);
            juce::String errorMessage;
            const auto parsed = parseAndValidate (jsonText, errorMessage);

            if (parsed.isVoid())
            {
                // A factory preset asset failing to parse/validate is a
                // build-time authoring bug (a malformed presets/factory/*.json
                // file shipped in BinaryData), not a runtime condition a user
                // can hit - surfaced loudly in debug builds, skipped safely
                // in release builds rather than crashing the plugin over a
                // missing preset.
                jassertfalse;
                continue;
            }

            auto* obj = parsed.getDynamicObject();
            factoryPresets.push_back ({ obj->getProperty (nameKey).toString(),
                                         obj->getProperty (categoryKey).toString(),
                                         parsed });
        }

        for (auto* parameter : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                apvts.addParameterListener (ranged->paramID, this);
    }

    PresetManager::~PresetManager()
    {
        for (auto* parameter : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                apvts.removeParameterListener (ranged->paramID, this);
    }

    //==========================================================================
    void PresetManager::parameterChanged (const juce::String&, float)
    {
        // Real-time safe by design - see the class-level docs in
        // PresetManager.h. Nothing else happens in this callback.
        if (! applyingPreset.load (std::memory_order_relaxed))
            dirty.store (true, std::memory_order_relaxed);
    }

    void PresetManager::applyStartupDefault()
    {
        loadPreset (defaultPresetName);
    }

    //==========================================================================
    void PresetManager::resetAllParametersToDefault()
    {
        for (auto* parameter : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                ranged->setValueNotifyingHost (ranged->getDefaultValue());
    }

    void PresetManager::applyPlainValues (const juce::var& parametersObject)
    {
        auto* obj = parametersObject.getDynamicObject();

        if (obj == nullptr)
            return;

        for (auto& property : obj->getProperties())
        {
            // Unknown parameter IDs are silently ignored - forward-tolerant
            // import (a preset saved by a newer plugin version carrying a
            // parameter this build doesn't know about).
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (property.name.toString()));

            if (ranged != nullptr)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (static_cast<float> (property.value)));
        }
    }

    void PresetManager::applyLegacyParameterDefaults (const juce::var& parsed) const
    {
        if (config.legacyParameterCutoffVersion.isEmpty() || config.legacyParameterDefaults.empty())
            return;

        auto* obj = parsed.getDynamicObject();

        if (obj == nullptr)
            return;

        const auto presetVersion = obj->getProperty (pluginVersionKey).toString();

        // A preset saved at or after the cutoff means what it says about
        // these parameters, even if it happens to omit one.
        if (! isVersionOlderThan (presetVersion, config.legacyParameterCutoffVersion))
            return;

        auto* parametersObj = obj->getProperty (parametersKey).getDynamicObject();

        if (parametersObj == nullptr)
            return;

        for (const auto& legacy : config.legacyParameterDefaults)
        {
            // Never override an explicit value: a legacy preset that somehow
            // does name the parameter (hand-edited, or forward-ported) is
            // taken at its word.
            if (parametersObj->hasProperty (legacy.parameterId))
                continue;

            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (legacy.parameterId));

            if (ranged != nullptr)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (legacy.plainValue));
        }
    }

    void PresetManager::applyParsedPreset (const juce::var& parsed, const juce::String& name, bool isFactory)
    {
        applyingPreset.store (true, std::memory_order_relaxed);

        resetAllParametersToDefault();

        auto* obj = parsed.getDynamicObject();
        jassert (obj != nullptr); // parseAndValidate() guarantees this

        // Back-fill legacy values BEFORE the preset's own values are applied,
        // so an explicit value in the preset always wins (belt-and-braces:
        // applyLegacyParameterDefaults() already skips keys the preset sets).
        applyLegacyParameterDefaults (parsed);

        applyPlainValues (obj->getProperty (parametersKey));

        // Plugin-specific extras (Crypta: the optional IR reference), applied
        // last so the hook sees fully settled, already-back-filled parameter
        // values. Called even when the document carries no extra fields -
        // see the hook's docs in PresetManager.h.
        if (config.applyExtraFields != nullptr)
            config.applyExtraFields (parsed);

        currentPresetName = name;
        currentPresetIsFactory = isFactory;

        applyingPreset.store (false, std::memory_order_relaxed);
        dirty.store (false, std::memory_order_relaxed);
    }

    //==========================================================================
    juce::var PresetManager::parseAndValidate (const juce::String& jsonText, juce::String& errorMessage) const
    {
        juce::var parsed;
        const auto parseResult = juce::JSON::parse (jsonText, parsed);

        if (parseResult.failed() || ! parsed.isObject())
        {
            errorMessage = TRANS ("This file is not a valid preset.");
            return {};
        }

        auto* obj = parsed.getDynamicObject();

        if (obj->getProperty (formatKey).toString() != juce::String (presetFormatTag))
        {
            errorMessage = TRANS ("This preset was saved by an incompatible version of the preset format.");
            return {};
        }

        if (obj->getProperty (pluginKey).toString() != config.pluginId)
        {
            errorMessage = TRANS ("This preset file belongs to a different plugin.");
            return {};
        }

        return parsed;
    }

    juce::var PresetManager::buildPresetVar (const juce::String& name, const juce::String& category) const
    {
        auto* parametersObj = new juce::DynamicObject();

        for (auto* parameter : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                parametersObj->setProperty (ranged->paramID, ranged->convertFrom0to1 (ranged->getValue()));

        auto* obj = new juce::DynamicObject();
        obj->setProperty (formatKey, juce::String (presetFormatTag));
        obj->setProperty (pluginKey, config.pluginId);
        obj->setProperty (pluginVersionKey, config.pluginVersion);
        obj->setProperty (nameKey, name);
        obj->setProperty (categoryKey, category);
        obj->setProperty (parametersKey, juce::var (parametersObj));

        // Plugin-specific extras. A hook that adds nothing (or no hook at
        // all) leaves this document exactly as it was before the mechanism
        // existed, which is what keeps presets that carry no extras
        // byte-identical across builds.
        if (config.captureExtraFields != nullptr)
            config.captureExtraFields (*obj);

        return juce::var (obj);
    }

    bool PresetManager::writePresetVarToFile (const juce::var& presetVar, const juce::File& destination) const
    {
        const auto json = juce::JSON::toString (presetVar, false);
        return destination.replaceWithText (json);
    }

    juce::File PresetManager::userPresetFileFor (const juce::String& name) const
    {
        return getUserPresetsDirectory (config).getChildFile (legalFileStem (name) + presetFileExtension);
    }

    //==========================================================================
    juce::File PresetManager::getUserPresetsDirectory (const PresetManagerConfig& presetManagerConfig)
    {
        if (presetManagerConfig.userPresetsDirectoryOverrideForTests != juce::File())
            return presetManagerConfig.userPresetsDirectoryOverrideForTests;

       #if JUCE_MAC
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Library")
            .getChildFile ("Audio")
            .getChildFile ("Presets")
            .getChildFile (presetManagerConfig.manufacturerName)
            .getChildFile (presetManagerConfig.pluginName);
       #elif JUCE_WINDOWS
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile (presetManagerConfig.manufacturerName)
            .getChildFile (presetManagerConfig.pluginName)
            .getChildFile ("Presets");
       #else
        // Linux/other: not a CI or release target for this suite (see
        // CLAUDE.md - CI is macOS + Windows only), but still a sane,
        // discoverable per-user location rather than an unsupported path.
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile (presetManagerConfig.manufacturerName)
            .getChildFile (presetManagerConfig.pluginName)
            .getChildFile ("Presets");
       #endif
    }

    //==========================================================================
    std::vector<PresetManager::PresetEntry> PresetManager::getAllPresets() const
    {
        std::vector<PresetEntry> factoryEntries;

        for (auto& factory : factoryPresets)
            factoryEntries.push_back ({ factory.name, factory.category, true });

        std::sort (factoryEntries.begin(), factoryEntries.end(),
                   [] (const PresetEntry& a, const PresetEntry& b) { return a.name < b.name; });

        std::vector<PresetEntry> userEntries;
        const auto userFiles = getUserPresetsDirectory (config).findChildFiles (
            juce::File::findFiles, false, juce::String ("*") + presetFileExtension);

        for (auto& file : userFiles)
        {
            juce::String errorMessage;
            const auto parsed = parseAndValidate (file.loadFileAsString(), errorMessage);

            if (parsed.isVoid())
                continue;

            auto* obj = parsed.getDynamicObject();
            userEntries.push_back ({ obj->getProperty (nameKey).toString(), obj->getProperty (categoryKey).toString(), false });
        }

        std::sort (userEntries.begin(), userEntries.end(),
                   [] (const PresetEntry& a, const PresetEntry& b) { return a.name < b.name; });

        factoryEntries.insert (factoryEntries.end(), userEntries.begin(), userEntries.end());
        return factoryEntries;
    }

    //==========================================================================
    bool PresetManager::loadPreset (const juce::String& name)
    {
        // User presets win over factory presets of the same name - the same
        // resolution order applyStartupDefault() relies on for "Default".
        const auto userFile = userPresetFileFor (name);

        if (userFile.existsAsFile())
        {
            juce::String errorMessage;
            const auto parsed = parseAndValidate (userFile.loadFileAsString(), errorMessage);

            if (! parsed.isVoid())
            {
                applyParsedPreset (parsed, name, false);
                return true;
            }
        }

        for (auto& factory : factoryPresets)
        {
            if (factory.name == name)
            {
                applyParsedPreset (factory.parsed, name, true);
                return true;
            }
        }

        return false;
    }

    void PresetManager::nextPreset()
    {
        const auto all = getAllPresets();

        if (all.empty())
            return;

        const auto it = std::find_if (all.begin(), all.end(),
                                       [this] (const PresetEntry& e) { return e.name == currentPresetName; });

        const auto currentIndex = (it == all.end()) ? static_cast<size_t> (-1)
                                                      : static_cast<size_t> (std::distance (all.begin(), it));
        const auto nextIndex = (it == all.end()) ? size_t { 0 } : (currentIndex + 1) % all.size();

        loadPreset (all[nextIndex].name);
    }

    void PresetManager::previousPreset()
    {
        const auto all = getAllPresets();

        if (all.empty())
            return;

        const auto it = std::find_if (all.begin(), all.end(),
                                       [this] (const PresetEntry& e) { return e.name == currentPresetName; });

        size_t previousIndex = 0;

        if (it != all.end())
        {
            const auto currentIndex = static_cast<size_t> (std::distance (all.begin(), it));
            previousIndex = (currentIndex == 0) ? all.size() - 1 : currentIndex - 1;
        }
        else
        {
            previousIndex = all.size() - 1;
        }

        loadPreset (all[previousIndex].name);
    }

    //==========================================================================
    bool PresetManager::saveUserPreset (const juce::String& name, const juce::String& category)
    {
        for (auto& factory : factoryPresets)
            if (factory.name == name)
                return false;

        const auto dir = getUserPresetsDirectory (config);

        if (! dir.exists() && ! dir.createDirectory())
            return false;

        const auto presetVar = buildPresetVar (name, category);

        if (! writePresetVarToFile (presetVar, userPresetFileFor (name)))
            return false;

        currentPresetName = name;
        currentPresetIsFactory = false;
        dirty.store (false, std::memory_order_relaxed);
        return true;
    }

    bool PresetManager::saveCurrentUserPreset()
    {
        if (currentPresetIsFactory || currentPresetName.isEmpty())
            return false;

        juce::String category = "Init";
        const auto existingFile = userPresetFileFor (currentPresetName);

        if (existingFile.existsAsFile())
        {
            juce::String errorMessage;
            const auto parsed = parseAndValidate (existingFile.loadFileAsString(), errorMessage);

            if (! parsed.isVoid())
                category = parsed.getDynamicObject()->getProperty (categoryKey).toString();
        }

        return saveUserPreset (currentPresetName, category);
    }

    bool PresetManager::renameUserPreset (const juce::String& oldName, const juce::String& newName)
    {
        const auto oldFile = userPresetFileFor (oldName);

        if (! oldFile.existsAsFile())
            return false;

        juce::String errorMessage;
        const auto parsed = parseAndValidate (oldFile.loadFileAsString(), errorMessage);

        if (parsed.isVoid())
            return false;

        // A rename changes the name and NOTHING else, so the renamed file is
        // the original document with one property replaced rather than a
        // freshly built one. Building it would stamp the CURRENT live APVTS
        // values and this build's pluginVersion over a preset the user only
        // asked to relabel - and, since PresetManagerConfig::captureExtraFields
        // may add plugin-specific keys (Crypta's IR reference), would also have
        // to know which of those keys to carry over. Copying every property
        // needs no such list and cannot silently drop one.
        auto* obj = parsed.getDynamicObject();
        auto* renamedObj = new juce::DynamicObject();

        for (const auto& property : obj->getProperties())
            renamedObj->setProperty (property.name, property.value);

        renamedObj->setProperty (nameKey, newName);

        const juce::var renamed (renamedObj);

        if (! writePresetVarToFile (renamed, userPresetFileFor (newName)))
            return false;

        oldFile.deleteFile();

        if (currentPresetName == oldName)
            currentPresetName = newName;

        return true;
    }

    bool PresetManager::deleteUserPreset (const juce::String& name)
    {
        const auto file = userPresetFileFor (name);

        if (! file.existsAsFile())
            return false;

        const auto deleted = file.deleteFile();

        if (deleted && currentPresetName == name)
            currentPresetName.clear();

        return deleted;
    }

    //==========================================================================
    bool PresetManager::setCurrentAsDefault()
    {
        const auto dir = getUserPresetsDirectory (config);

        if (! dir.exists() && ! dir.createDirectory())
            return false;

        // Deliberately bypasses saveUserPreset()'s "can't shadow a factory
        // name" guard and doesn't touch currentPresetName/dirty - see the
        // header docs.
        const auto presetVar = buildPresetVar (defaultPresetName, "Init");
        return writePresetVarToFile (presetVar, userPresetFileFor (defaultPresetName));
    }

    bool PresetManager::resetDefault()
    {
        const auto file = userPresetFileFor (defaultPresetName);

        if (! file.existsAsFile())
            return true; // nothing to reset is not an error

        return file.deleteFile();
    }

    //==========================================================================
    bool PresetManager::importPresetFile (const juce::File& file, juce::String& errorMessage)
    {
        const auto parsed = parseAndValidate (file.loadFileAsString(), errorMessage);

        if (parsed.isVoid())
            return false;

        applyParsedPreset (parsed, parsed.getDynamicObject()->getProperty (nameKey).toString(), false);
        return true;
    }

    bool PresetManager::exportPreset (const juce::String& name, const juce::File& destination) const
    {
        for (auto& factory : factoryPresets)
            if (factory.name == name)
                return writePresetVarToFile (factory.parsed, destination);

        const auto userFile = userPresetFileFor (name);

        if (! userFile.existsAsFile())
            return false;

        juce::String errorMessage;
        const auto parsed = parseAndValidate (userFile.loadFileAsString(), errorMessage);

        if (parsed.isVoid())
            return false;

        return writePresetVarToFile (parsed, destination);
    }

    //==========================================================================
    bool PresetManager::exportBank (const juce::File& destination, const std::vector<juce::String>& userPresetNamesOnly) const
    {
        const auto dir = getUserPresetsDirectory (config);
        juce::Array<juce::File> filesToInclude;

        if (userPresetNamesOnly.empty())
        {
            filesToInclude = dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + presetFileExtension);
        }
        else
        {
            for (auto& name : userPresetNamesOnly)
            {
                const auto file = userPresetFileFor (name);

                if (file.existsAsFile())
                    filesToInclude.add (file);
            }
        }

        if (filesToInclude.isEmpty())
            return false;

        juce::ZipFile::Builder builder;

        for (auto& file : filesToInclude)
            builder.addFile (file, 9, file.getFileName());

        // FileOutputStream appends to an existing file by default - delete
        // first so a re-export always produces a clean, exact bank rather
        // than a stale one with leftover trailing bytes.
        destination.deleteFile();

        std::unique_ptr<juce::FileOutputStream> outputStream (destination.createOutputStream());

        if (outputStream == nullptr)
            return false;

        return builder.writeToStream (*outputStream, nullptr);
    }

    int PresetManager::importBank (const juce::File& zipFile)
    {
        juce::ZipFile zip (zipFile);
        int importedCount = 0;

        const auto dir = getUserPresetsDirectory (config);

        if (! dir.exists() && ! dir.createDirectory())
            return 0;

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto* entry = zip.getEntry (i);

            if (entry == nullptr || ! entry->filename.endsWithIgnoreCase (presetFileExtension))
                continue;

            const std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (i));

            if (stream == nullptr)
                continue;

            const auto text = stream->readEntireStreamAsString();
            juce::String errorMessage;
            const auto parsed = parseAndValidate (text, errorMessage);

            if (parsed.isVoid())
                continue; // wrong plugin / malformed entry - skip, don't abort the whole bank

            auto* obj = parsed.getDynamicObject();
            const auto name = obj->getProperty (nameKey).toString();

            if (writePresetVarToFile (parsed, userPresetFileFor (name)))
                ++importedCount;
        }

        return importedCount;
    }
}
