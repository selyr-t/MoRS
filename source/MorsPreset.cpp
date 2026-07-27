/*
  ==============================================================================

    MorsPreset.cpp
    Created: 17 Mar 2026 10:49:33pm
    Author:  Edward Ryles

  ==============================================================================
*/

#include "MorsPreset.h"

constexpr const char* kCat = "PRESET";

//==============================================================================
// Static members
//==============================================================================
const juce::File PresetManager::defaultDirectory
{
    juce::File::getSpecialLocation(juce::File::commonDocumentsDirectory)
        .getChildFile(ProjectInfo::companyName)
        .getChildFile(ProjectInfo::projectName)
};

const juce::String PresetManager::extension { "preset" };

//==============================================================================
// MorsState serialization
//==============================================================================
std::unique_ptr<juce::XmlElement> MorsState::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement>("MorsPreset");

    xml->setAttribute("onsetThreshold",     onsetThreshold);
    xml->setAttribute("onsetSilence",       onsetSilence);
    xml->setAttribute("onsetMinMs",         onsetMinMs);
    xml->setAttribute("textureMorph",       (double) textureMorph);
    xml->setAttribute("beatInterp",         (double) beatInterp);
    xml->setAttribute("gaussDisplace",      (double) gaussDisplace);
    xml->setAttribute("gaussDraw",          (double) gaussDraw);
    xml->setAttribute("gaussWidth",         (double) gaussWidth);
    xml->setAttribute("gaussGuard",         (double) gaussGuard);
    xml->setAttribute("jumpWeight",         jumpWeight);
    xml->setAttribute("grainSizeMs",        grainSizeMs);
    xml->setAttribute("grainDensity",       grainDensity);
    xml->setAttribute("grainScatter",       grainScatter);
    xml->setAttribute("algorithmId",        algorithmId);
    xml->setAttribute("quantizeEnabled",    quantizeEnabled);
    xml->setAttribute("quantizeStrength",   quantizeStrength);
    xml->setAttribute("bpm",                bpm);
    xml->setAttribute("subdivision",        subdivision);

    return xml;
}

MorsState MorsState::fromXml(const juce::XmlElement& xml)
{
    MorsState s; // defaults apply for any field missing from the file

    s.onsetThreshold   = xml.getDoubleAttribute("onsetThreshold",       s.onsetThreshold);
    s.onsetSilence     = xml.getDoubleAttribute("onsetSilence",         s.onsetSilence);
    s.onsetMinMs       = xml.getDoubleAttribute("onsetMinMs",           s.onsetMinMs);
    s.textureMorph     = (float) xml.getDoubleAttribute("textureMorph", s.textureMorph);
    s.beatInterp       = (float) xml.getDoubleAttribute("beatInterp",   s.beatInterp);
    s.gaussDisplace    = (float) xml.getDoubleAttribute("gaussDisplace",s.gaussDisplace);
    s.gaussDraw        = (float) xml.getDoubleAttribute("gaussDraw",    s.gaussDraw);
    s.gaussWidth       = (float) xml.getDoubleAttribute("gaussWidth",   s.gaussWidth);
    s.gaussGuard       = (float) xml.getDoubleAttribute("gaussGuard",   s.gaussGuard);
    s.jumpWeight       = xml.getBoolAttribute  ("jumpWeight",           s.jumpWeight);
    s.grainSizeMs      = xml.getDoubleAttribute("grainSizeMs",          s.grainSizeMs);
    s.grainDensity     = xml.getIntAttribute   ("grainDensity",         s.grainDensity);
    s.grainScatter     = xml.getDoubleAttribute("grainScatter",         s.grainScatter);
    s.algorithmId      = xml.getIntAttribute   ("algorithmId",          s.algorithmId);
    s.quantizeEnabled  = xml.getBoolAttribute  ("quantizeEnabled",      s.quantizeEnabled);
    s.quantizeStrength = xml.getDoubleAttribute("quantizeStrength",     s.quantizeStrength);
    s.bpm              = xml.getDoubleAttribute("bpm",                  s.bpm);
    s.subdivision      = xml.getIntAttribute   ("subdivision",          s.subdivision);

    return s;
}

//==============================================================================
// PresetManager
//==============================================================================
PresetManager::PresetManager(GetState getter, SetState setter)
    : getState(getter), setState(setter)
{
    if (!defaultDirectory.exists())
    {
        const auto result = defaultDirectory.createDirectory();
        if (result.failed())
        {
            LOGE(kCat, "Could not create preset directory: " + result.getErrorMessage());
            jassertfalse;
        }
        else
        {
            LOGD(kCat, "Created preset directory: " + defaultDirectory.getFullPathName());
        }
    }
}

void PresetManager::savePreset(const juce::String& presetName)
{
    if (presetName.isEmpty())
        return;

    const auto xml        = getState().toXml();
    const auto presetFile = defaultDirectory.getChildFile(presetName + "." + extension);

    if (!xml->writeTo(presetFile))
    {
        LOGE(kCat, "Could not write preset file: " + presetFile.getFullPathName());
        jassertfalse;
        return;
    }

    currentPreset = presetName;
    LOGD(kCat, "Saved preset: " + presetName);
}

void PresetManager::deletePreset(const juce::String& presetName)
{
    if (presetName.isEmpty())
        return;

    const auto presetFile = defaultDirectory.getChildFile(presetName + "." + extension);

    if (!presetFile.existsAsFile())
    {
        LOGW(kCat, "Preset file does not exist: " + presetFile.getFullPathName());
        jassertfalse;
        return;
    }

    if (!presetFile.deleteFile())
    {
        LOGE(kCat, "Could not delete preset file: " + presetFile.getFullPathName());
        jassertfalse;
        return;
    }

    LOGD(kCat, "Deleted preset: " + presetName);

    if (currentPreset == presetName)
        currentPreset = {};
}

void PresetManager::loadPreset(const juce::String& presetName)
{
    if (presetName.isEmpty())
        return;

    const auto presetFile = defaultDirectory.getChildFile(presetName + "." + extension);

    if (!presetFile.existsAsFile())
    {
        LOGW(kCat, "Preset file does not exist: " + presetFile.getFullPathName());
        jassertfalse;
        return;
    }

    if (auto xml = juce::XmlDocument::parse(presetFile))
    {
        setState(MorsState::fromXml(*xml));
        currentPreset = presetName;
        LOGD(kCat, "Loaded preset: " + presetName);
    }
    else
    {
        LOGE(kCat, "Failed to parse preset file: " + presetFile.getFullPathName());
        jassertfalse;
    }
}

void PresetManager::loadNextPreset()
{
    const auto all = getAllPresets();
    if (all.isEmpty())
        return;

    const int current = all.indexOf(currentPreset);
    const int next    = (current >= all.size() - 1) ? 0 : current + 1;
    loadPreset(all[next]);
}

void PresetManager::loadPreviousPreset()
{
    const auto all = getAllPresets();
    if (all.isEmpty())
        return;

    const int current  = all.indexOf(currentPreset);
    const int previous = (current <= 0) ? all.size() - 1 : current - 1;
    loadPreset(all[previous]);
}

juce::StringArray PresetManager::getAllPresets() const
{
    juce::StringArray presets;

    const auto files = defaultDirectory.findChildFiles(
        juce::File::findFiles, false, "*." + extension);

    for (const auto& f : files)
        presets.add(f.getFileNameWithoutExtension());

    presets.sort(true);
    return presets;
}

juce::String PresetManager::getCurrentPreset() const
{
    return currentPreset;
}

//==============================================================================
// PresetPanel
//==============================================================================
PresetPanel::PresetPanel()
{
    configureButton(saveButton,           "Save");
    configureButton(deleteButton,         "Delete");
    configureButton(previousPresetButton, "<");
    configureButton(nextPresetButton,     ">");

    presetList.setTextWhenNothingSelected("No Preset Selected");
    presetList.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    addAndMakeVisible(presetList);
    presetList.addListener(this);
}

PresetPanel::~PresetPanel()
{
    saveButton.removeListener(this);
    deleteButton.removeListener(this);
    previousPresetButton.removeListener(this);
    nextPresetButton.removeListener(this);
    presetList.removeListener(this);
}

void PresetPanel::resized()
{
    const auto container = getLocalBounds().reduced(4);
    auto bounds = container;

    saveButton.setBounds           (bounds.removeFromLeft(container.proportionOfWidth(0.2f)).reduced(4));
    previousPresetButton.setBounds (bounds.removeFromLeft(container.proportionOfWidth(0.1f)).reduced(4));
    presetList.setBounds           (bounds.removeFromLeft(container.proportionOfWidth(0.4f)).reduced(4));
    nextPresetButton.setBounds     (bounds.removeFromLeft(container.proportionOfWidth(0.1f)).reduced(4));
    deleteButton.setBounds         (bounds.reduced(4));
}

void PresetPanel::configureButton(juce::Button& button, const juce::String& text)
{
    button.setButtonText(text);
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    addAndMakeVisible(button);
    button.addListener(this);
}

void PresetPanel::refreshPresetList()
{
    presetList.clear(juce::dontSendNotification);

    if (manager == nullptr)
        return;

    const auto presets = manager->getAllPresets();
    for (int i = 0; i < presets.size(); ++i)
        presetList.addItem(presets[i], i + 1);

    const auto current = manager->getCurrentPreset();
    if (current.isNotEmpty())
        presetList.setText(current, juce::dontSendNotification);
}

void PresetPanel::buttonClicked(juce::Button* button)
{
    if (manager == nullptr)
        return;

    if (button == &saveButton)
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save Preset",
            PresetManager::defaultDirectory,
            "*." + PresetManager::extension);

        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file == juce::File{})
                    return;

                const auto name = file.getFileNameWithoutExtension();
                if (name.isNotEmpty() && manager != nullptr)
                {
                    manager->savePreset(name);
                    refreshPresetList();
                }
            });
    }
    else if (button == &deleteButton)
    {
        const auto name = manager->getCurrentPreset();
        if (name.isNotEmpty())
        {
            manager->deletePreset(name);
            refreshPresetList();
        }
    }
    else if (button == &previousPresetButton)
    {
        manager->loadPreviousPreset();
        refreshPresetList();
    }
    else if (button == &nextPresetButton)
    {
        manager->loadNextPreset();
        refreshPresetList();
    }
}

void PresetPanel::comboBoxChanged(juce::ComboBox* comboBox)
{
    if (comboBox == &presetList && manager != nullptr)
        manager->loadPreset(presetList.getText());
}
