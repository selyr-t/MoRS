/*
  ==============================================================================

    MorsPreset.h
    Created: 17 Mar 2026 10:49:33pm
    Author:  Edward Ryles

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "MorsDebug.h"

//==============================================================================
// MorsState — all saveable parameters
//==============================================================================
struct MorsState
{
    //==========================================================================
    // Onset Detection
    //==========================================================================
    double onsetThreshold = 0.5;
    double onsetSilence   = -90.0;
    double onsetMinMs     = 50.0;

    //==========================================================================
    // Morph
    //==========================================================================
    float  textureMorph     = 1.0f;
    float  beatInterp       = 1.0f;
    float  gaussDisplace    = 0.1f;
    float  gaussDraw        = 0.01f;
    float  gaussWidth       = 1.0f;
    float  gaussGuard       = 1.0f;
    bool   jumpWeight       = false;

    //==========================================================================
    // Granular
    //==========================================================================
    double grainSizeMs  = 50.0;
    int    grainDensity = 2;
    double grainScatter = 0.0;

    //==========================================================================
    // Algorithm
    //==========================================================================
    int algorithmId = 1;

    //==========================================================================
    // Quantize
    //==========================================================================
    bool   quantizeEnabled  = false;
    double quantizeStrength = 100.0;
    double bpm              = 60.0;
    int    subdivision      = 8;

    //==========================================================================
    // Serialization
    //==========================================================================
    std::unique_ptr<juce::XmlElement> toXml() const;
    static MorsState fromXml(const juce::XmlElement& xml);
};

//==============================================================================
// PresetManager — saves and loads MorsState to/from .preset files
//==============================================================================
class PresetManager
{
public:
    using GetState = std::function<MorsState()>;
    using SetState = std::function<void(const MorsState&)>;

    static const juce::File   defaultDirectory;
    static const juce::String extension;

    PresetManager(GetState getter, SetState setter);

    void savePreset     (const juce::String& presetName);
    void deletePreset   (const juce::String& presetName);
    void loadPreset     (const juce::String& presetName);
    void loadNextPreset();
    void loadPreviousPreset();

    juce::StringArray getAllPresets()     const;
    juce::String      getCurrentPreset() const;

private:
    GetState     getState;
    SetState     setState;
    juce::String currentPreset;
};

//==============================================================================
// PresetPanel — UI strip: Save | < | [dropdown] | > | Delete
//==============================================================================
class PresetPanel : public juce::Component,
                    public juce::Button::Listener,
                    public juce::ComboBox::Listener
{
public:
    PresetPanel();
    ~PresetPanel() override;
    void resized() override;

    // Call this after construction, before the panel is shown.
    // MainContentComponent owns the PresetManager lifetime.
    void setManager(PresetManager* m)
    {
        manager = m;
        refreshPresetList();
    }

private:
    void buttonClicked   (juce::Button*   button)   override;
    void comboBoxChanged (juce::ComboBox* comboBox) override;
    void configureButton (juce::Button& button, const juce::String& text);
    void refreshPresetList();

    juce::TextButton saveButton           { "Save"   };
    juce::TextButton deleteButton         { "Delete" };
    juce::TextButton previousPresetButton { "<"      };
    juce::TextButton nextPresetButton     { ">"      };
    juce::ComboBox   presetList;
    
    std::unique_ptr<juce::FileChooser> fileChooser; 

    PresetManager* manager = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetPanel)
};
