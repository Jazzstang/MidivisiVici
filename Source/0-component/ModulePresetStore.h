/**
 * @file ModulePresetStore.h
 * @brief Helpers for saving/loading per-module preset payloads as XML files.
 *
 * Threading:
 * - Message thread only (uses file I/O and XML parsing).
 */
#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <vector>

namespace ModulePresetStore
{
    static const juce::Identifier kRootType       { "MODULE_PRESET" };
    static const juce::Identifier kPayloadType    { "PAYLOAD" };
    static const juce::Identifier kPresetNameProp { "presetName" };
    static const juce::Identifier kModuleKeyProp  { "moduleKey" };
    static const juce::Identifier kVersionProp    { "version" };

    static const juce::Identifier kParamStateType { "PARAM_STATE" };
    static const juce::Identifier kParamNodeType  { "PARAM" };
    static const juce::Identifier kParamIdProp    { "id" };
    static const juce::Identifier kParamValueProp { "value" };

    struct Entry
    {
        juce::File file;
        juce::String displayName;
    };

    inline bool loadPresetPayload(const juce::File& file,
                                  juce::ValueTree& payloadState,
                                  juce::String* outPresetName,
                                  juce::String* outModuleKey,
                                  juce::String* errorMessage);
    inline std::vector<Entry> listPresets(const juce::String& moduleKey);
    inline bool renamePresetFile(const juce::File& existingPresetFile,
                                 const juce::String& expectedModuleKey,
                                 const juce::String& newPresetName,
                                 juce::File* outRenamedFile,
                                 juce::String* errorMessage);

    inline juce::File getModuleDirectory(const juce::String& moduleKey)
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MidivisiVici")
            .getChildFile("ModulePresets")
            .getChildFile(moduleKey);
    }

    inline juce::String sanitizeNameForFile(const juce::String& name)
    {
        auto s = name.trim();
        if (s.isEmpty())
            s = "Preset";

        juce::String out;
        out.preallocateBytes(s.getNumBytesAsUTF8());

        for (const juce::juce_wchar c : s)
        {
            const bool allowed = juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ' || c == '-' || c == '_';
            out << (allowed ? juce::String::charToString((char) c) : "_");
        }

        out = out.trim();
        if (out.isEmpty())
            out = "Preset";

        while (out.contains("  "))
            out = out.replace("  ", " ");

        return out;
    }

    inline juce::File getUniqueFileForPresetName(const juce::String& moduleKey,
                                                 const juce::String& desiredName,
                                                 const juce::File* fileToIgnore = nullptr)
    {
        const auto dir = getModuleDirectory(moduleKey);
        const auto base = sanitizeNameForFile(desiredName);

        juce::File file = dir.getChildFile(base + ".xml");
        if (fileToIgnore != nullptr && file == *fileToIgnore)
            return file;
        if (!file.existsAsFile())
            return file;

        for (int i = 2; i < 10000; ++i)
        {
            file = dir.getChildFile(base + " " + juce::String(i) + ".xml");
            if (fileToIgnore != nullptr && file == *fileToIgnore)
                return file;
            if (!file.existsAsFile())
                return file;
        }

        return dir.getNonexistentChildFile(base, ".xml", false);
    }

    inline bool savePresetToFile(const juce::String& moduleKey,
                                 const juce::String& presetName,
                                 const juce::ValueTree& payloadState,
                                 const juce::File& destinationFile,
                                 juce::String* errorMessage = nullptr)
    {
        if (!payloadState.isValid())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid preset payload.";
            return false;
        }

        const auto moduleDir = getModuleDirectory(moduleKey);
        if (!moduleDir.createDirectory())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Unable to create preset directory:\n" + moduleDir.getFullPathName();
            return false;
        }

        const auto parentDir = destinationFile.getParentDirectory();
        if (!parentDir.createDirectory())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Unable to create preset directory:\n" + parentDir.getFullPathName();
            return false;
        }

        juce::ValueTree root(kRootType);
        root.setProperty(kPresetNameProp, presetName.trim().isNotEmpty() ? presetName.trim() : "Preset", nullptr);
        root.setProperty(kModuleKeyProp, moduleKey, nullptr);
        root.setProperty(kVersionProp, 1, nullptr);

        juce::ValueTree payload(kPayloadType);
        payload.addChild(payloadState.createCopy(), -1, nullptr);
        root.addChild(payload, -1, nullptr);

        auto xml = root.createXml();
        if (xml == nullptr || !xml->writeTo(destinationFile))
        {
            if (errorMessage != nullptr)
                *errorMessage = "Unable to write preset file:\n" + destinationFile.getFullPathName();
            return false;
        }

        return true;
    }

    inline bool savePreset(const juce::String& moduleKey,
                           const juce::String& presetName,
                           const juce::ValueTree& payloadState,
                           juce::String* errorMessage = nullptr)
    {
        if (!payloadState.isValid())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid preset payload.";
            return false;
        }

        const auto requestedName = presetName.trim().isNotEmpty() ? presetName.trim() : juce::String("Preset");
        auto entries = listPresets(moduleKey);

        const auto hasName = [&entries](const juce::String& name)
        {
            for (const auto& e : entries)
                if (e.displayName.compareIgnoreCase(name) == 0)
                    return true;
            return false;
        };

        const auto parseNumberedSuffix = [](const juce::String& name,
                                            juce::String& stem,
                                            int& number)
        {
            auto t = name.trim();
            if (t.isEmpty())
                return false;

            int i = t.length() - 1;
            while (i >= 0 && juce::CharacterFunctions::isDigit(t[i]))
                --i;

            if (i == t.length() - 1)
                return false; // no numeric suffix
            if (i < 0 || t[i] != ' ')
                return false; // must be "name <n>"

            const auto digits = t.substring(i + 1);
            const int n = digits.getIntValue();
            if (n <= 0)
                return false;

            stem = t.substring(0, i).trimEnd();
            if (stem.isEmpty())
                return false;

            number = n;
            return true;
        };

        const auto findExactName = [&entries](const juce::String& name)
        {
            return std::find_if(entries.begin(),
                                entries.end(),
                                [&name](const Entry& e)
                                {
                                    return e.displayName.compareIgnoreCase(name) == 0;
                                });
        };

        auto exact = findExactName(requestedName);
        if (exact == entries.end())
        {
            const auto file = getUniqueFileForPresetName(moduleKey, requestedName);
            return savePresetToFile(moduleKey, requestedName, payloadState, file, errorMessage);
        }

        juce::String targetName;

        juce::String suffixStem;
        int suffixValue = 0;
        if (parseNumberedSuffix(requestedName, suffixStem, suffixValue))
        {
            int next = suffixValue + 1;
            targetName = suffixStem + " " + juce::String(next);
            while (hasName(targetName))
            {
                ++next;
                targetName = suffixStem + " " + juce::String(next);
            }

            const auto file = getUniqueFileForPresetName(moduleKey, targetName);
            return savePresetToFile(moduleKey, targetName, payloadState, file, errorMessage);
        }

        int existingNumber = 1;
        juce::String renamedExistingName = requestedName + " " + juce::String(existingNumber);
        while (hasName(renamedExistingName))
        {
            ++existingNumber;
            renamedExistingName = requestedName + " " + juce::String(existingNumber);
        }

        juce::File ignoredRenamedFile;
        if (!renamePresetFile(exact->file,
                              moduleKey,
                              renamedExistingName,
                              &ignoredRenamedFile,
                              errorMessage))
        {
            return false;
        }

        entries = listPresets(moduleKey);
        int nextNumber = existingNumber + 1;
        targetName = requestedName + " " + juce::String(nextNumber);
        while (true)
        {
            bool alreadyUsed = false;
            for (const auto& e : entries)
            {
                if (e.displayName.compareIgnoreCase(targetName) == 0)
                {
                    alreadyUsed = true;
                    break;
                }
            }

            if (!alreadyUsed)
                break;

            ++nextNumber;
            targetName = requestedName + " " + juce::String(nextNumber);
        }

        const auto file = getUniqueFileForPresetName(moduleKey, targetName);
        return savePresetToFile(moduleKey, targetName, payloadState, file, errorMessage);
    }

    inline bool savePresetInPlace(const juce::File& existingPresetFile,
                                  const juce::String& expectedModuleKey,
                                  const juce::ValueTree& payloadState,
                                  juce::String* errorMessage = nullptr)
    {
        juce::ValueTree ignoredPayload;
        juce::String existingPresetName;
        juce::String existingModuleKey;

        if (!loadPresetPayload(existingPresetFile,
                               ignoredPayload,
                               &existingPresetName,
                               &existingModuleKey,
                               errorMessage))
        {
            return false;
        }

        const auto moduleKey = existingModuleKey.isNotEmpty() ? existingModuleKey : expectedModuleKey;
        if (moduleKey.isEmpty())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid module key for preset save.";
            return false;
        }

        if (expectedModuleKey.isNotEmpty() && moduleKey != expectedModuleKey)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Preset module mismatch.";
            return false;
        }

        const auto presetName = existingPresetName.trim().isNotEmpty()
                                    ? existingPresetName.trim()
                                    : existingPresetFile.getFileNameWithoutExtension();

        return savePresetToFile(moduleKey,
                                presetName,
                                payloadState,
                                existingPresetFile,
                                errorMessage);
    }

    inline bool renamePresetFile(const juce::File& existingPresetFile,
                                 const juce::String& expectedModuleKey,
                                 const juce::String& newPresetName,
                                 juce::File* outRenamedFile = nullptr,
                                 juce::String* errorMessage = nullptr)
    {
        juce::ValueTree payloadState;
        juce::String existingPresetName;
        juce::String existingModuleKey;

        if (!loadPresetPayload(existingPresetFile,
                               payloadState,
                               &existingPresetName,
                               &existingModuleKey,
                               errorMessage))
        {
            return false;
        }

        const auto moduleKey = existingModuleKey.isNotEmpty() ? existingModuleKey : expectedModuleKey;
        if (moduleKey.isEmpty())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid module key for preset rename.";
            return false;
        }

        if (expectedModuleKey.isNotEmpty() && moduleKey != expectedModuleKey)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Preset module mismatch.";
            return false;
        }

        const auto finalPresetName = newPresetName.trim().isNotEmpty()
                                         ? newPresetName.trim()
                                         : (existingPresetName.trim().isNotEmpty()
                                                ? existingPresetName.trim()
                                                : existingPresetFile.getFileNameWithoutExtension());

        const auto targetFile = getUniqueFileForPresetName(moduleKey, finalPresetName, &existingPresetFile);
        if (!savePresetToFile(moduleKey, finalPresetName, payloadState, targetFile, errorMessage))
            return false;

        if (targetFile != existingPresetFile && existingPresetFile.existsAsFile())
        {
            if (!existingPresetFile.deleteFile())
            {
                if (errorMessage != nullptr)
                    *errorMessage = "Unable to remove old preset file:\n" + existingPresetFile.getFullPathName();
                return false;
            }
        }

        if (outRenamedFile != nullptr)
            *outRenamedFile = targetFile;
        return true;
    }

    inline bool deletePresetFile(const juce::File& presetFile,
                                 const juce::String& expectedModuleKey,
                                 juce::String* errorMessage = nullptr)
    {
        juce::ValueTree payloadState;
        juce::String presetName;
        juce::String moduleKey;

        if (!loadPresetPayload(presetFile,
                               payloadState,
                               &presetName,
                               &moduleKey,
                               errorMessage))
        {
            return false;
        }

        const auto resolvedModuleKey = moduleKey.isNotEmpty() ? moduleKey : expectedModuleKey;
        if (resolvedModuleKey.isEmpty())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid module key for preset delete.";
            return false;
        }

        if (expectedModuleKey.isNotEmpty() && resolvedModuleKey != expectedModuleKey)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Preset module mismatch.";
            return false;
        }

        if (!presetFile.deleteFile())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Unable to delete preset file:\n" + presetFile.getFullPathName();
            return false;
        }

        return true;
    }

    inline bool loadPresetPayload(const juce::File& file,
                                  juce::ValueTree& payloadState,
                                  juce::String* outPresetName = nullptr,
                                  juce::String* outModuleKey = nullptr,
                                  juce::String* errorMessage = nullptr)
    {
        if (!file.existsAsFile())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Preset file not found:\n" + file.getFullPathName();
            return false;
        }

        std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
        if (xml == nullptr)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid XML in preset file:\n" + file.getFullPathName();
            return false;
        }

        const auto root = juce::ValueTree::fromXml(*xml);
        if (!root.isValid() || !root.hasType(kRootType))
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid module preset root in:\n" + file.getFullPathName();
            return false;
        }

        if (outPresetName != nullptr)
            *outPresetName = root.getProperty(kPresetNameProp, file.getFileNameWithoutExtension()).toString();
        if (outModuleKey != nullptr)
            *outModuleKey = root.getProperty(kModuleKeyProp, juce::String()).toString();

        const auto payloadNode = root.getChildWithName(kPayloadType);
        if (!payloadNode.isValid() || payloadNode.getNumChildren() <= 0)
        {
            if (errorMessage != nullptr)
                *errorMessage = "Missing payload in preset file:\n" + file.getFullPathName();
            return false;
        }

        payloadState = payloadNode.getChild(0).createCopy();
        if (!payloadState.isValid())
        {
            if (errorMessage != nullptr)
                *errorMessage = "Invalid payload tree in preset file:\n" + file.getFullPathName();
            return false;
        }

        return true;
    }

    inline std::vector<Entry> listPresets(const juce::String& moduleKey)
    {
        std::vector<Entry> out;
        const auto dir = getModuleDirectory(moduleKey);
        if (!dir.isDirectory())
            return out;

        const auto files = dir.findChildFiles(juce::File::findFiles, false, "*.xml");
        out.reserve((size_t) files.size());

        for (const auto& f : files)
        {
            juce::String presetName;
            juce::String fileModuleKey;
            juce::ValueTree payload;

            if (!loadPresetPayload(f, payload, &presetName, &fileModuleKey, nullptr))
                continue;

            if (fileModuleKey.isNotEmpty() && fileModuleKey != moduleKey)
                continue;

            Entry e;
            e.file = f;
            e.displayName = presetName.trim().isNotEmpty() ? presetName.trim() : f.getFileNameWithoutExtension();
            out.push_back(std::move(e));
        }

        std::sort(out.begin(), out.end(),
                  [](const Entry& a, const Entry& b)
                  {
                      return a.displayName.compareIgnoreCase(b.displayName) < 0;
                  });
        return out;
    }

    inline juce::ValueTree captureParameterState(const juce::AudioProcessorValueTreeState& vts,
                                                 const std::vector<juce::String>& paramIds)
    {
        juce::ValueTree state(kParamStateType);

        for (const auto& id : paramIds)
        {
            if (id.isEmpty())
                continue;

            auto* param = vts.getParameter(id);
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param);
            if (ranged == nullptr)
                continue;

            juce::ValueTree node(kParamNodeType);
            node.setProperty(kParamIdProp, id, nullptr);
            node.setProperty(kParamValueProp, ranged->getValue(), nullptr);
            state.addChild(node, -1, nullptr);
        }

        return state;
    }

    inline void applyParameterState(juce::AudioProcessorValueTreeState& vts,
                                    const juce::ValueTree& state)
    {
        if (!state.isValid() || !state.hasType(kParamStateType))
            return;

        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            const auto node = state.getChild(i);
            if (!node.isValid() || !node.hasType(kParamNodeType))
                continue;

            const auto id = node.getProperty(kParamIdProp, juce::String()).toString();
            auto* param = vts.getParameter(id);
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param);
            if (ranged == nullptr)
                continue;

            const float value = (float) node.getProperty(kParamValueProp, ranged->getValue());
            ranged->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
        }
    }
} // namespace ModulePresetStore
