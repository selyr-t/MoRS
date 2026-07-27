//******************************************************************************
//
// MainContentComponent.cpp
//
// MoRS - Morphing Rhythm System
//
//******************************************************************************

#include "MainContentComponent.h"
#include "MorsOnsetMapper.h"
#include "MorsDTW.h"
#include "MorsFile.h"
#include <cmath>
#include <algorithm>

//******************************************************************************
//
// MoRS - Lifecycle Management
//
//******************************************************************************
void MainContentComponent::syncGranularParamsToAllStretchers()
{
    const double sizeMs    = grainSizeSlider.getValue();
    const float  scatter   = (float) grainScatterSlider.getValue();
    const int    density   = (int) grainDensitySlider.getValue();
    const float  ampJitter = 0.3f;

    granular.setGrainParams(sizeMs, scatter, ampJitter, density);

    for (auto& band : bandSlots)
    {
        if (auto* g = dynamic_cast<Granular*>(band.stretcher.get()))
            g->setGrainParams(sizeMs, scatter, ampJitter, density);
    }
}

void MainContentComponent::initializeBandUi()
{
    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& band = bandSlots[b];

        band.bandLabel .setText("Band " + juce::String(b + 1), juce::dontSendNotification);
        band.beatLabel .setText("Beat",  juce::dontSendNotification);
        band.morphLabel.setText("Morph", juce::dontSendNotification);

        band.bandLabel .setJustificationType(juce::Justification::centredLeft);
        band.beatLabel .setJustificationType(juce::Justification::centredLeft);
        band.morphLabel.setJustificationType(juce::Justification::centredLeft);

        addAndMakeVisible(band.bandLabel);
        addAndMakeVisible(band.beatLabel);
        addAndMakeVisible(band.morphLabel);
        addAndMakeVisible(band.beatSelector);
        addAndMakeVisible(band.morphSelector);

        band.beatSelector.onChange = [this, b]() { loadBandBeatFile(b); };
        band.morphSelector.onChange = [this, b]() { loadBandMorphFile(b); };

        band.stretcher = std::make_unique<PhaseVocoder>(1024, 512);
    }

    refreshBandUiVisibility();
}

void MainContentComponent::refreshBandUiVisibility()
{
    const bool showBandControls = isBandAnalysisEnabled();

    for (int b = 0; b < kMaxBands; ++b)
    {
        const bool showControlsForBand = showBandControls && b < activeBandCount;

        bandSlots[b].bandLabel.setVisible(showControlsForBand);
        bandSlots[b].beatLabel.setVisible(showControlsForBand);
        bandSlots[b].morphLabel.setVisible(showControlsForBand);
        bandSlots[b].beatSelector.setVisible(showControlsForBand);
        bandSlots[b].morphSelector.setVisible(showControlsForBand);
    }
}

void MainContentComponent::populateBandSelectors()
{
    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& band = bandSlots[b];

        band.beatSelector.clear();
        band.morphSelector.clear();

        int id = 1;
        for (const auto& f : sampleLibraryFiles)
        {
            const auto name = f.getFileNameWithoutExtension();
            band.beatSelector.addItem(name, id);
            band.morphSelector.addItem(name, id);
            ++id;
        }
    }
}

//==============================================================================
// [Lifecycle] Constructor
//==============================================================================
MainContentComponent::MainContentComponent()
    : juce::AudioAppComponent(otherDeviceManager),
      phaseVocoder(1024, 512),
      sola(1024, 512),
      granular(1024, 512)
{
    //--------------------------------------------------------------------------
    // File buttons
    //--------------------------------------------------------------------------
    for (auto* b : fileButtons)
    {
        addAndMakeVisible(b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
        b->addListener(this);
    }

    //--------------------------------------------------------------------------
    // System buttons
    //--------------------------------------------------------------------------
    for (auto* systemButton : systemButtons)
    {
        addAndMakeVisible(systemButton);
        systemButton->setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
        systemButton->addListener(this);
    }

    //--------------------------------------------------------------------------
    // Algorithm
    //--------------------------------------------------------------------------
    algorithmSelector.addItem("Phase Vocoder", 1);
    algorithmSelector.addItem("SOLA",          2);
    algorithmSelector.addItem("Granular",      3);
    algorithmSelector.setSelectedId(1);
    algorithmSelector.onChange = [this]() { onAlgorithmChanged(); };
    addAndMakeVisible(algorithmSelector);
    currentStretchAlgo = &phaseVocoder;

    //--------------------------------------------------------------------------
    // Transport
    //--------------------------------------------------------------------------
    for (auto* b : transportButtons)
    {
        addAndMakeVisible(b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
        b->addListener(this);
        b->setEnabled(false);
    }

    loopButton.setClickingTogglesState(true);
    loopButton.setToggleState(true, juce::dontSendNotification);

    addAndMakeVisible(scrubber);
    scrubber.addListener(this);
    scrubber.setEnabled(false);

    //--------------------------------------------------------------------------
    // Audio
    //--------------------------------------------------------------------------
    otherDeviceManager.initialise(2, 2, nullptr, true);
    setAudioChannels(0, 2);

    //--------------------------------------------------------------------------
    // Number boxes
    //--------------------------------------------------------------------------
    numberLabelBpm.setText("BPM", juce::dontSendNotification);
    numberLabelSubdiv.setText("Subdivision", juce::dontSendNotification);

    for (auto i = 0; i < (int) numberBoxes.size(); ++i)
    {
        addAndMakeVisible(numberBoxes[i]);
        numberBoxes[i]->setEditable(true);
        numberBoxes[i]->setJustificationType(juce::Justification::centred);
        numberBoxes[i]->onTextChange = [this]() { reanalyze(); };

        addAndMakeVisible(numberLabels[i]);
        numberLabels[i]->setJustificationType(juce::Justification::centredLeft);
    }

    //--------------------------------------------------------------------------
    // Quantization
    //--------------------------------------------------------------------------
    quantizeOnsetsLabel.setText("Quantize", juce::dontSendNotification);
    quantizeStrengthLabel.setText("Strength", juce::dontSendNotification);

    addAndMakeVisible(quantizeOnsetsButton);
    quantizeOnsetsButton.setButtonText("Quantize Onsets");
    quantizeOnsetsButton.setToggleState(false, juce::dontSendNotification);
    quantizeOnsetsButton.onClick = [this]() { reanalyze(); };

    addAndMakeVisible(quantizeOnsetsLabel);
    addAndMakeVisible(quantizeStrengthLabel);

    addAndMakeVisible(quantizeStrengthSlider);
    quantizeStrengthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    quantizeStrengthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    quantizeStrengthSlider.setRange(0.0, 1.0, 0.01);
    quantizeStrengthSlider.setValue(1.0);
    quantizeStrengthSlider.onValueChange = [this]() { reanalyze(); };

    //--------------------------------------------------------------------------
    // Onset sliders
    //--------------------------------------------------------------------------
    onsetThresholdLabel.setText("Onset Threshold", juce::dontSendNotification);
    onsetSilenceLabel.setText("Onset Silence", juce::dontSendNotification);
    onsetMinMsLabel.setText("Onset Min Ms", juce::dontSendNotification);

    for (auto* onsetSlider : onsetSliders)
    {
        addAndMakeVisible(onsetSlider);
        onsetSlider->setSliderStyle(juce::Slider::LinearHorizontal);
        onsetSlider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        onsetSlider->setEnabled(true);
        onsetSlider->onValueChange = [this]() { reanalyze(); };

        if (onsetSlider->getName() == "onsetThresholdSlider")
        {
            onsetSlider->setRange(0.0, 1.0, 0.001);
            onsetSlider->setValue(0.3);
        }
        else if (onsetSlider->getName() == "onsetSilenceSlider")
        {
            onsetSlider->setRange(-90.0, 0.0, 0.1);
            onsetSlider->setValue(-70.0);
        }
        else if (onsetSlider->getName() == "onsetMinMsSlider")
        {
            onsetSlider->setRange(1.0, 500.0, 1.0);
            onsetSlider->setValue(12.5);
        }
    }

    for (auto* onsetLabel : onsetLabels)
    {
        addAndMakeVisible(onsetLabel);
        onsetLabel->setJustificationType(juce::Justification::centredLeft);
    }

    //--------------------------------------------------------------------------
    // Band controls
    //--------------------------------------------------------------------------
    addAndMakeVisible(analyzeBandsToggle);
    analyzeBandsToggle.setButtonText("Analyze Bands");
    analyzeBandsToggle.setToggleState(false, juce::dontSendNotification);
    analyzeBandsToggle.onClick = [this]()
    {
        refreshBandUiVisibility();
        reanalyze();
        resized();
        repaint();
    };

    bandCountSelector.addItem("1 (Off)", 1);
    bandCountSelector.addItem("2", 2);
    bandCountSelector.addItem("3", 3);
    bandCountSelector.addItem("4", 4);
    bandCountSelector.setSelectedId(1);
    bandCountSelector.onChange = [this]() { onBandCountChanged(); };
    addAndMakeVisible(bandCountSelector);

    //--------------------------------------------------------------------------
    // Morph / interpolation sliders
    //--------------------------------------------------------------------------
    addAndMakeVisible(textureMorphSlider);
    addAndMakeVisible(gaussWidthSlider);
    addAndMakeVisible(gaussGuardSlider);
    addAndMakeVisible(gaussDisplaceSlider);

    addAndMakeVisible(textureMorphLabel);
    addAndMakeVisible(gaussWidthLabel);
    addAndMakeVisible(gaussGuardLabel);
    addAndMakeVisible(gaussDisplaceLabel);

    textureMorphSlider .setSliderStyle(juce::Slider::LinearHorizontal);
    gaussWidthSlider   .setSliderStyle(juce::Slider::LinearHorizontal);
    gaussGuardSlider   .setSliderStyle(juce::Slider::LinearHorizontal);
    gaussDisplaceSlider.setSliderStyle(juce::Slider::LinearHorizontal);

    for (auto* s : { &textureMorphSlider, &gaussWidthSlider,
                     &gaussGuardSlider, &gaussDisplaceSlider })
    {
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    }

    textureMorphSlider.setRange(0.0, 1.0, 0.01);
    textureMorphSlider.setValue(0.0);
    textureMorphSlider.onValueChange = [this]()
    {
        const float v = (float) textureMorphSlider.getValue();
        phaseVocoder.setCrossSynthesisEnabled(morphFile.isLoaded() && v > 0.0f);
    };

    // gaussWidthSlider drives alpha (blend depth): 0 = transparent/dry, 1 = full morph.
    // sigmaScale (Gaussian kernel width) is a fixed internal constant (0.5).
    gaussWidthSlider.setRange(0.0, 1.0, 0.01);
    gaussWidthSlider.setValue(0.0);
    gaussWidthSlider.onValueChange = [this]()
    {
        mappingDirty.store(true, std::memory_order_relaxed);
    };

    gaussGuardSlider.setRange(0.0, 50.0, 0.1);
    gaussGuardSlider.setValue(5.0);
    gaussGuardSlider.onValueChange = [this]()
    {
        mappingDirty.store(true, std::memory_order_relaxed);
    };

    // gaussDisplaceSlider drives displacementBoost: 0 = no push, 1 = full boost.
    gaussDisplaceSlider.setRange(0.0, 1, 0.01);
    gaussDisplaceSlider.setValue(0.0);
    gaussDisplaceSlider.onValueChange = [this]()
    {
        mappingDirty.store(true, std::memory_order_relaxed);
    };
    // drawRange is fixed at 0.35 — no slider needed.

    //--------------------------------------------------------------------------
    // Granular sliders
    //--------------------------------------------------------------------------
    for (auto* grainSlider : grainSliders)
    {
        addAndMakeVisible(grainSlider);
        grainSlider->setSliderStyle(juce::Slider::LinearHorizontal);
        grainSlider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        grainSlider->setEnabled(true);
        grainSlider->onValueChange = [this]() { syncGranularParamsToAllStretchers(); };

        if (grainSlider->getName() == "grainSizeSlider")
        {
            grainSlider->setRange(0.1, 500.0, 0.1);
            grainSlider->setValue(50.0);
        }
        else if (grainSlider->getName() == "grainDensitySlider")
        {
            grainSlider->setRange(1.0, 8.0, 1.0);
            grainSlider->setValue(2.0);
        }
        else if (grainSlider->getName() == "grainScatterSlider")
        {
            grainSlider->setRange(0.0, 500.0, 0.1);
            grainSlider->setValue(0.0);
        }
    }

    for (auto* grainLabel : grainLabels)
        addAndMakeVisible(grainLabel);

    //--------------------------------------------------------------------------
    // Sample library
    //--------------------------------------------------------------------------
    addAndMakeVisible(chooseFolderButton);
    chooseFolderButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
    chooseFolderButton.onClick = [this]()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Select Sample Library Folder",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory));

        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto result = fc.getResult();
                                 if (result.isDirectory())
                                 {
                                     sampleLibraryFolder = result;
                                     scanSampleLibrary();
                                 }
                             });
    };

    addAndMakeVisible(morphSampleSelector);
    morphSampleSelector.onChange = [this]()
    {
        const int idx = morphSampleSelector.getSelectedItemIndex();
        if (idx >= 0 && idx < sampleLibraryPaths.size())
        {
            juce::File f(sampleLibraryPaths[idx]);
            loadMorphAudio(f);
        }
    };

    //--------------------------------------------------------------------------
    // Waveform display
    //--------------------------------------------------------------------------
    formatManager.registerBasicFormats();
    initializeBandUi();

    //--------------------------------------------------------------------------
    // Presets
    //--------------------------------------------------------------------------
    presetManager = std::make_unique<PresetManager>(
        [this]() { return captureState(); },
        [this](const MorsState& s) { applyState(s); }
    );
    presetPanel.setManager(presetManager.get());
    addAndMakeVisible(presetPanel);

    //--------------------------------------------------------------------------
    // Debug + timer
    //--------------------------------------------------------------------------
    MorsDebug::init();
    MorsDebug::setMinLevel(MorsLogLevel::debug);
    MorsDataExport::init();
    startTimer(40);

    setSize(1200, 1100);
}

//==============================================================================
// [Lifecycle] Destructor
//==============================================================================
MainContentComponent::~MainContentComponent()
{
    MorsDebug::shutdown();
    isPlayingRT.store(false, std::memory_order_release);
    shutdownAudio();
    numberBoxes.clear();
    numberLabels.clear();
    numberBoxes.shrink_to_fit();
    numberLabels.shrink_to_fit();
}

//==============================================================================
// [Preset] Capture State
//==============================================================================
MorsState MainContentComponent::captureState() const
{
    MorsState s;
    s.onsetThreshold   = onsetThresholdSlider.getValue();
    s.onsetSilence     = onsetSilenceSlider.getValue();
    s.onsetMinMs       = onsetMinMsSlider.getValue();
    s.textureMorph     = (float) textureMorphSlider.getValue();
    s.gaussDisplace    = (float) gaussDisplaceSlider.getValue();
    s.gaussWidth       = (float) gaussWidthSlider.getValue();
    s.gaussGuard       = (float) gaussGuardSlider.getValue();
    s.grainSizeMs      = grainSizeSlider.getValue();
    s.grainDensity     = (int) grainDensitySlider.getValue();
    s.grainScatter     = grainScatterSlider.getValue();
    s.algorithmId      = algorithmSelector.getSelectedId();
    s.quantizeEnabled  = quantizeOnsetsButton.getToggleState();
    s.quantizeStrength = quantizeStrengthSlider.getValue();
    s.bpm              = numberBoxBpm.getText().getDoubleValue();
    s.subdivision      = numberBoxSubdiv.getText().getIntValue();
    return s;
}

//==============================================================================
// [Preset] Apply State
//==============================================================================
void MainContentComponent::applyState(const MorsState& s)
{
    onsetThresholdSlider   .setValue(s.onsetThreshold,      juce::dontSendNotification);
    onsetSilenceSlider     .setValue(s.onsetSilence,        juce::dontSendNotification);
    onsetMinMsSlider       .setValue(s.onsetMinMs,          juce::dontSendNotification);
    textureMorphSlider     .setValue(s.textureMorph,        juce::dontSendNotification);
    gaussDisplaceSlider    .setValue(s.gaussDisplace,       juce::dontSendNotification);
    gaussWidthSlider       .setValue(s.gaussWidth,          juce::dontSendNotification);
    gaussGuardSlider       .setValue(s.gaussGuard,          juce::dontSendNotification);
    grainSizeSlider        .setValue(s.grainSizeMs,         juce::dontSendNotification);
    grainDensitySlider     .setValue(s.grainDensity,        juce::dontSendNotification);
    grainScatterSlider     .setValue(s.grainScatter,        juce::dontSendNotification);
    algorithmSelector      .setSelectedId(s.algorithmId,    juce::dontSendNotification);
    quantizeOnsetsButton   .setToggleState(s.quantizeEnabled, juce::dontSendNotification);
    quantizeStrengthSlider .setValue(s.quantizeStrength,    juce::dontSendNotification);
    numberBoxBpm           .setText(juce::String(s.bpm),         juce::dontSendNotification);
    numberBoxSubdiv        .setText(juce::String(s.subdivision), juce::dontSendNotification);

    syncGranularParamsToAllStretchers();
    reanalyze();
}

//==============================================================================
// [UI Layout] Component Placement
//==============================================================================
void MainContentComponent::resized()
{
    const int margin       = 10;
    const int gap          = 6;
    const int rowH         = 28;
    const int sliderH      = 24;
    const int sliderRowH   = 42;
    const int labelYOffset = 15;
    const int bandVisualH  = 110;

    int y = margin;
    auto area = getLocalBounds().reduced(margin);

    auto layoutSliderGroup = [&](juce::Rectangle<int> bounds,
                                 juce::Label& label,
                                 juce::Slider& slider)
    {
        auto sliderBounds = bounds.withY(bounds.getY() + labelYOffset)
                                  .withHeight(sliderH);
        auto labelBounds  = bounds.withY(sliderBounds.getY() - labelYOffset)
                                  .withHeight(14);
        label.setBounds(labelBounds);
        slider.setBounds(sliderBounds);
    };

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), rowH);

        const int smallBoxW  = 60;
        const int textLabelW = 80;
        const int rightW     = (textLabelW + smallBoxW) * 2 + gap * 2;

        auto leftArea  = row.removeFromLeft(row.getWidth() - rightW);
        auto rightArea = row;

        juce::FlexBox fb;
        fb.flexDirection  = juce::FlexBox::Direction::row;
        fb.justifyContent = juce::FlexBox::JustifyContent::flexStart;
        fb.alignItems     = juce::FlexBox::AlignItems::stretch;

        auto addFlex = [&](juce::Component& c, float flex = 1.0f)
        {
            fb.items.add(juce::FlexItem(c)
                         .withFlex(flex)
                         .withHeight((float) rowH)
                         .withMargin({ 0.0f, (float) gap, 0.0f, 0.0f }));
        };

        addFlex(sourceButtonOpen,  1.0f);
        addFlex(beatRefButtonOpen, 1.0f);
        addFlex(morphButtonOpen,   1.0f);
        addFlex(exportDataButton,  1.0f);
        addFlex(audioConfigButton, 1.2f);
        addFlex(algorithmSelector, 1.2f);
        addFlex(analyzeBandsToggle,1.0f);
        addFlex(bandCountSelector, 0.8f);
        fb.performLayout(leftArea.toFloat());

        auto bpmArea = rightArea.removeFromLeft(textLabelW + smallBoxW + gap);
        auto subArea = rightArea.removeFromLeft(textLabelW + smallBoxW);

        numberLabels[0]->setBounds(bpmArea.removeFromLeft(textLabelW));
        numberBoxes[0]->setBounds(bpmArea.removeFromLeft(smallBoxW));
        numberLabels[1]->setBounds(subArea.removeFromLeft(textLabelW));
        numberBoxes[1]->setBounds(subArea.removeFromLeft(smallBoxW));
    }

    y += rowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), rowH);

        juce::FlexBox fb;
        fb.flexDirection  = juce::FlexBox::Direction::row;
        fb.justifyContent = juce::FlexBox::JustifyContent::flexStart;
        fb.alignItems     = juce::FlexBox::AlignItems::stretch;

        auto addFlex = [&](juce::Component& c, float flex = 1.0f)
        {
            fb.items.add(juce::FlexItem(c)
                         .withFlex(flex)
                         .withHeight((float) rowH)
                         .withMargin({ 0.0f, (float) gap, 0.0f, 0.0f }));
        };

        addFlex(playButton);
        addFlex(stopButton);
        addFlex(recordButton);
        addFlex(loopButton);

        fb.performLayout(row.toFloat());
    }

    y += rowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), rowH);
        chooseFolderButton.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(gap);
        morphSampleSelector.setBounds(row);
    }

    y += rowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), rowH);
        quantizeOnsetsLabel.setBounds(row.removeFromLeft(70));
        quantizeOnsetsButton.setBounds(row.removeFromLeft(120));
        row.removeFromLeft(20);
        quantizeStrengthLabel.setBounds(row.removeFromLeft(70));
        quantizeStrengthSlider.setBounds(row.removeFromLeft(220));
    }

    y += rowH + gap + 4;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), sliderRowH);
        const int groupW = (row.getWidth() - 2 * gap) / 3;

        auto g1 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g2 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g3 = row;

        layoutSliderGroup(g1, onsetThresholdLabel, onsetThresholdSlider);
        layoutSliderGroup(g2, onsetSilenceLabel,   onsetSilenceSlider);
        layoutSliderGroup(g3, onsetMinMsLabel,     onsetMinMsSlider);
    }

    y += sliderRowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), sliderRowH);
        const int groupW = (row.getWidth() - gap) / 2;

        auto g1 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g2 = row;

        layoutSliderGroup(g1, textureMorphLabel, textureMorphSlider);
        layoutSliderGroup(g2, gaussWidthLabel,   gaussWidthSlider);
    }

    y += sliderRowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), sliderRowH);
        const int groupW = (row.getWidth() - gap) / 2;

        auto g1 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g2 = row;

        layoutSliderGroup(g1, gaussDisplaceLabel, gaussDisplaceSlider);
        layoutSliderGroup(g2, gaussGuardLabel,    gaussGuardSlider);
    }

    y += sliderRowH + gap;

    {
        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), sliderRowH);
        const int groupW = (row.getWidth() - 2 * gap) / 3;

        auto g1 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g2 = row.removeFromLeft(groupW); row.removeFromLeft(gap);
        auto g3 = row;

        layoutSliderGroup(g1, grainSizeLabel,    grainSizeSlider);
        layoutSliderGroup(g2, grainDensityLabel, grainDensitySlider);
        layoutSliderGroup(g3, grainScatterLabel, grainScatterSlider);
    }

    y += sliderRowH + gap;

    presetPanel.setBounds(area.getX(), y, area.getWidth(), sliderRowH);
    y += sliderRowH + gap;

    scrubber.setBounds(area.getX(), y, area.getWidth(), rowH);
    y += rowH + gap;

    refreshBandUiVisibility();

    const int visibleBandRows = getVisibleBandVisualizerCount();

    if (isBandAnalysisEnabled())
    {
        for (int b = 0; b < visibleBandRows; ++b)
        {
            auto& band = bandSlots[b];

            auto controlRow = juce::Rectangle<int>(area.getX(), y, area.getWidth(), rowH);
            band.controlBounds = controlRow;

            auto bandName = controlRow.removeFromLeft(70);
            band.bandLabel.setBounds(bandName);

            band.beatLabel.setBounds(controlRow.removeFromLeft(40));
            band.beatSelector.setBounds(controlRow.removeFromLeft(220));
            controlRow.removeFromLeft(gap);

            band.morphLabel.setBounds(controlRow.removeFromLeft(45));
            band.morphSelector.setBounds(controlRow.removeFromLeft(220));

            y += rowH + gap;

            band.visualBounds = juce::Rectangle<int>(area.getX(), y, area.getWidth(), bandVisualH);
            y += bandVisualH + gap;
        }
    }
    else
    {
        // Default full-spectrum row uses Band 1 slot visuals only.
        // Extend the visualizer to the bottom of the window, with the same
        // padding as everything else (already baked into `area` via reduced(margin)).
        const int remainingH = juce::jmax(bandVisualH, area.getBottom() - y*2);
        bandSlots[0].visualBounds = juce::Rectangle<int>(area.getX(), y, area.getWidth(), remainingH);
        y += remainingH + gap;
    }
}

//==============================================================================
// [UI] Audio Configuration Dialog
//==============================================================================
void MainContentComponent::menuAudioConfig(juce::Component* /*target*/)
{
    auto audioSettingsComp = std::make_unique<juce::AudioDeviceSelectorComponent>(
        otherDeviceManager,
        0, 2,
        0, 2,
        true,
        true,
        true,
        true
    );

    audioSettingsComp->setSize(500, 450);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(audioSettingsComp.release());
    options.dialogTitle                  = "Audio Settings";
    options.dialogBackgroundColour       = getLookAndFeel()
                                           .findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = false;

    options.launchAsync();
}

//==============================================================================
// [Audio Callbacks] Preparation
//==============================================================================
void MainContentComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    lastBlockSize  = samplesPerBlockExpected;
    lastSampleRate = sampleRate;

    phaseVocoder.prepare(sampleRate, samplesPerBlockExpected);
    sola.prepare(sampleRate, samplesPerBlockExpected);
    granular.prepare(sampleRate, samplesPerBlockExpected);

    for (auto& band : bandSlots)
        if (band.stretcher)
            band.stretcher->prepare(sampleRate, samplesPerBlockExpected);

    onsetSource.prepare();
    onsetBeatRef.prepare();

    tempoSource.prepare(sampleRate);
    tempoSource.reset();

    if (currentStretchAlgo)
        currentStretchAlgo->seek(playheadSeconds.load(std::memory_order_relaxed));
    
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlockExpected;
    spec.numChannels = (juce::uint32) juce::jmax(1, sourceFile.getBuffer().getNumChannels() > 0
                                                    ? sourceFile.getBuffer().getNumChannels()
                                                    : 2);

    outputLimiter.reset();
    outputLimiter.prepare(spec);
    outputLimiter.setThreshold(-0.3f);
    outputLimiter.setRelease(50.0f);
}

// [Audio Callbacks] Block Processing
//==============================================================================
void MainContentComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const auto _blockStart = juce::Time::getHighResolutionTicks();

    bufferToFill.clearActiveBufferRegion();

    if (!isPlayingRT.load(std::memory_order_acquire))
        return;

    if (sourceFile.getBuffer().getNumSamples() <= 0)
        return;

    const int numSamples = bufferToFill.numSamples;
    const int outNumCh   = bufferToFill.buffer->getNumChannels();

    if (outNumCh <= 0 || numSamples <= 0)
        return;

    const double playhead = playheadSeconds.load(std::memory_order_relaxed);

    juce::AudioBuffer<float> outputBuffer(outNumCh, numSamples);
    outputBuffer.clear();

    const bool useBands = isBandAnalysisEnabled() && activeBandCount > 1;
    bool bandsReady = useBands;

    if (useBands)
    {
        for (int b = 0; b < activeBandCount; ++b)
        {
            if (bandSlots[b].sourceBandBuffer.getNumSamples() <= 0
             || bandSlots[b].segments.size() < 2
             || bandSlots[b].stretcher == nullptr)
            {
                bandsReady = false;
                break;
            }
        }
    }

    if (bandsReady)
    {
        for (int b = 0; b < activeBandCount; ++b)
        {
            auto& band = bandSlots[b];

            juce::AudioBuffer<float> bandOut(outNumCh, numSamples);
            bandOut.clear();

            band.stretcher->processBlock(bandOut,
                                         band.sourceBandBuffer,
                                         playhead,
                                         band.segments);

            for (int ch = 0; ch < outNumCh; ++ch)
                outputBuffer.addFrom(ch, 0, bandOut, ch, 0, numSamples);
        }
    }
    else if (currentStretchAlgo != nullptr)
    {
        currentStretchAlgo->processBlock(outputBuffer,
                                         sourceFile.getBuffer(),
                                         playhead,
                                         segments);
    }

    //--------------------------------------------------------------------------
    // Apply output limiter AFTER summing all processing paths
    //--------------------------------------------------------------------------
    {
        outputBuffer.applyGain(0.95f);
        
        juce::dsp::AudioBlock<float> block(outputBuffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        outputLimiter.process(context);
    }

    if (state == RECORDING)
        fileWriter.record(outputBuffer);

    for (int ch = 0; ch < outNumCh; ++ch)
    {
        bufferToFill.buffer->copyFrom(ch,
                                      bufferToFill.startSample,
                                      outputBuffer,
                                      ch,
                                      0,
                                      numSamples);
    }

    const double sr      = juce::jmax(1.0, lastSampleRate);
    const double advance = (double) numSamples / sr;
    const double next    = playhead + advance;

    if (sourceFile.getLengthSeconds() > 0.0 && next >= sourceFile.getLengthSeconds())
    {
        // Recording should always stop cleanly at EOF for exact full-length capture
        if (state == RECORDING)
        {
            playheadSeconds.store(sourceFile.getLengthSeconds(), std::memory_order_relaxed);
            isPlayingRT.store(false, std::memory_order_release);
            fileWriter.close();

            juce::MessageManager::callAsync([this]()
            {
                changeState(IDLE);
            });

            return;
        }

        if (loopButton.getToggleState())
        {
            playheadSeconds.store(0.0, std::memory_order_relaxed);
            currentStretchAlgo->seek(0.0);

            phaseVocoder.seek(0.0);
            sola.seek(0.0);
            granular.seek(0.0);

            for (int b = 0; b < activeBandCount; ++b)
                if (bandSlots[b].stretcher)
                    bandSlots[b].stretcher->seek(0.0);

            return;
        }
        else
        {
            playheadSeconds.store(sourceFile.getLengthSeconds(), std::memory_order_relaxed);
            isPlayingRT.store(false, std::memory_order_release);

            juce::MessageManager::callAsync([this]()
            {
                changeState(IDLE);
            });

            return;
        }
    }

    playheadSeconds.store(next, std::memory_order_relaxed);
    // Blocking
    const auto   _blockEnd = juce::Time::getHighResolutionTicks();
    const double _blockMs  = juce::Time::highResolutionTicksToSeconds(_blockEnd - _blockStart) * 1000.0;

    static std::atomic<uint32_t> _blk_counter { 0 };
    const uint32_t _blk_n = _blk_counter.fetch_add(1, std::memory_order_relaxed);

    if ((_blk_n & 0xFu) == 0u)
    {
        const double _budget = (lastSampleRate > 0.0)
                             ? ((double) bufferToFill.numSamples / lastSampleRate) * 1000.0
                             : 11.6;

        char _m[128];
        std::snprintf(_m, sizeof(_m),
                      "block=%.3f ms  budget=%.3f ms  load=%.1f%%  bands=%s",
                      _blockMs, _budget,
                      (_blockMs / _budget) * 100.0,
                      (bandsReady ? "ON" : "OFF"));
        LOGD_RT("BLOCK_PERF", _m);

        MorsLog::logBlockPerf(_blockMs, _budget,
                              (_blockMs / _budget) * 100.0,
                              bandsReady);
    }

    // Push a perf sample every 32 blocks for thesis export (RT-safe: no alloc/lock/IO)
    if ((_blk_n & 0x1Fu) == 0u)
    {
        const char* algoName = "PV";
        const int   algoId   = algorithmSelector.getSelectedId();
        if      (algoId == 2) algoName = "SOLA";
        else if (algoId == 3) algoName = "Granular";

        MorsDataExport::pushPerfSample(perfBlockCounter,
                                       bufferToFill.numSamples,
                                       lastSampleRate,
                                       algoName,
                                       _blockMs);
        ++perfBlockCounter;
    }
}

//==============================================================================
// [Audio Callbacks] Release Resources
//==============================================================================
void MainContentComponent::releaseResources()
{
    isPlayingRT.store(false, std::memory_order_release);

    onsetSource.reset();
    onsetBeatRef.reset();
    tempoSource.reset();

    lastBlockSize  = 0;
    lastSampleRate = 0.0;
}

//==============================================================================
// [File Operations] Scan Sample Library
//==============================================================================
void MainContentComponent::scanSampleLibrary()
{
    if (!sampleLibraryFolder.isDirectory())
        return;

    sampleLibraryPaths.clear();
    sampleLibraryFiles.clear();
    morphSampleSelector.clear();

    juce::Array<juce::File> files;
    sampleLibraryFolder.findChildFiles(files,
                                       juce::File::findFiles,
                                       false,
                                       "*.wav;*.aif;*.aiff;*.flac");

    files.sort();

    int id = 1;
    for (auto& f : files)
    {
        sampleLibraryFiles.add(f);
        sampleLibraryPaths.add(f.getFullPathName());
        morphSampleSelector.addItem(f.getFileNameWithoutExtension(), id++);
    }

    populateBandSelectors();
}

//==============================================================================
// [File Operations] Open Source File
//==============================================================================
void MainContentComponent::openSourceFile()
{
    sourceFile.open([this](const juce::File& file,
                           juce::AudioBuffer<float>& /*buffer*/,
                           double /*sampleRate*/,
                           std::unique_ptr<juce::AudioFormatReaderSource> reader)
    {
        loadedSourceFile = file;
        readerSource = std::move(reader);

        sourceFile.analyze("aubio",
                           onsetThresholdSlider.getValue(),
                           onsetSilenceSlider.getValue(),
                           onsetMinMsSlider.getValue());

        if (isBandAnalysisEnabled())
            sourceFile.analyzeBands(crossoverHz[0], crossoverHz[1], crossoverHz[2],
                                    onsetThresholdSlider.getValue(),
                                    onsetSilenceSlider.getValue(),
                                    onsetMinMsSlider.getValue());

        buildSourceBandBuffers();

        if (quantizeOnsetsButton.getToggleState())
        {
            sourceFile.quantize(numberBoxBpm.getText().getDoubleValue(),
                                numberBoxSubdiv.getText().getIntValue(),
                                (float) quantizeStrengthSlider.getValue());
        }

        rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());
        if (isBandAnalysisEnabled())
            rebuildBandSegments((float) gaussWidthSlider.getValue());

        repaint();

        playButton.setEnabled(true);
        recordButton.setEnabled(true);
        scrubber.setEnabled(true);
        scrubber.setRange(0.0, sourceFile.getLengthSeconds(), 0.0);
        scrubber.setValue(0.0, juce::dontSendNotification);
        playheadSeconds.store(0.0, std::memory_order_relaxed);
        currentStretchAlgo->seek(0.0);
        changeState(IDLE);

        if (sourceFile.isLoaded() && beatRefFile.isLoaded())
            analyzeButton.setEnabled(true);

        sourceFile.thumbnailChanged = [this]() { repaint(); };
    });
}

//==============================================================================
// [File Operations] Open Morph File
//==============================================================================
void MainContentComponent::openMorphFile()
{
    morphFile.open([this](const juce::File&,
                          juce::AudioBuffer<float>&,
                          double,
                          std::unique_ptr<juce::AudioFormatReaderSource>)
    {
        phaseVocoder.setMorphDebugCaptureEnabled(true);

        phaseVocoder.analyzeMorphTarget(morphFile.getBuffer(),
                                        morphFile.getSampleRate());
        
        phaseVocoder.setCrossSynthesisEnabled(morphFile.isLoaded() &&
                                              (float) textureMorphSlider.getValue() > 0.0f);

        buildSpectrogramImage();
        morphFile.thumbnailChanged = [this]() { repaint(); };
        repaint();
    });
}

//==============================================================================
// [File Operations] Open Beat Ref File
//==============================================================================
void MainContentComponent::openBeatRefFile()
{
    beatRefFile.open([this](const juce::File&,
                            juce::AudioBuffer<float>&,
                            double,
                            std::unique_ptr<juce::AudioFormatReaderSource>)
    {
        beatRefFile.analyze("aubio",
                            onsetThresholdSlider.getValue(),
                            onsetSilenceSlider.getValue(),
                            onsetMinMsSlider.getValue());

        if (quantizeOnsetsButton.getToggleState())
        {
            beatRefFile.quantize(numberBoxBpm.getText().getDoubleValue(),
                                 numberBoxSubdiv.getText().getIntValue(),
                                 (float) quantizeStrengthSlider.getValue());
        }

        rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());
        if (isBandAnalysisEnabled())
            rebuildBandSegments((float) gaussWidthSlider.getValue());

        if (sourceFile.isLoaded() && beatRefFile.isLoaded())
            analyzeButton.setEnabled(true);

        beatRefFile.thumbnailChanged = [this]() { repaint(); };
        repaint();
    });
}

//==============================================================================
// [File Operations] Load Source Audio
//==============================================================================
bool MainContentComponent::loadSourceAudio(juce::File& file)
{
    if (!sourceFile.loadFromFile(file))
        return false;

    loadedSourceFile = file;

    sourceFile.analyze("aubio",
                       onsetThresholdSlider.getValue(),
                       onsetSilenceSlider.getValue(),
                       onsetMinMsSlider.getValue());

    buildSourceBandBuffers();

    playButton.setEnabled(true);
    recordButton.setEnabled(true);
    scrubber.setEnabled(true);
    scrubber.setRange(0.0, sourceFile.getLengthSeconds(), 0.0);
    scrubber.setValue(0.0, juce::dontSendNotification);
    playheadSeconds.store(0.0, std::memory_order_relaxed);
    currentStretchAlgo->seek(0.0);
    changeState(IDLE);

    if (sourceFile.isLoaded() && beatRefFile.isLoaded())
        analyzeButton.setEnabled(true);

    sourceFile.thumbnailChanged = [this]() { repaint(); };
    repaint();
    return true;
}

//==============================================================================
// [File Operations] Load Morph Audio
//==============================================================================
bool MainContentComponent::loadMorphAudio(juce::File& file)
{
    if (!morphFile.loadFromFile(file))
        return false;

    phaseVocoder.setMorphDebugCaptureEnabled(true);

    phaseVocoder.analyzeMorphTarget(morphFile.getBuffer(),
                                    morphFile.getSampleRate());

    phaseVocoder.setCrossSynthesisEnabled(morphFile.isLoaded() &&
                                          (float) textureMorphSlider.getValue() > 0.0f);

    buildSpectrogramImage();
    morphFile.thumbnailChanged = [this]() { repaint(); };
    repaint();
    return true;
}

//==============================================================================
// [File Operations] Load Beat Ref Audio
//==============================================================================
bool MainContentComponent::loadBeatRefAudio(juce::File& file)
{
    if (!beatRefFile.loadFromFile(file))
        return false;

    beatRefFile.analyze("aubio",
                        onsetThresholdSlider.getValue(),
                        onsetSilenceSlider.getValue(),
                        onsetMinMsSlider.getValue());

    if (sourceFile.isLoaded() && beatRefFile.isLoaded())
        analyzeButton.setEnabled(true);

    beatRefFile.thumbnailChanged = [this]() { repaint(); };
    repaint();
    return true;
}

//==============================================================================
// [File Operations] Record to File
//==============================================================================
void MainContentComponent::recordFile()
{
    const auto name = "mors_rec_"
                    + juce::Time::getCurrentTime().formatted("%Y_%m_%d_%Hh%Mm%Ss")
                    + ".wav";

    auto recDir  = AudioFile::path("audio_processed/mors_rec");
    auto recFile = recDir.getChildFile(name);

    fileRecord = recFile;

    const int recSampleRate = (int) juce::jmax(1.0, lastSampleRate);
    const int recChannels   = juce::jmax(1, sourceFile.getBuffer().getNumChannels());

    fileWriter.setup(recFile, recSampleRate, recChannels);
}

//==============================================================================
// [Analysis] State
//==============================================================================
bool MainContentComponent::isBandAnalysisEnabled() const
{
    return analyzeBandsToggle.getToggleState() && activeBandCount > 1;
}

//==============================================================================
// [Analysis] Analyze Source
//==============================================================================
void MainContentComponent::analyzeSourceFile()
{
    if (!sourceFile.isLoaded())
        return;

    sourceFile.analyze("aubio",
                       onsetThresholdSlider.getValue(),
                       onsetSilenceSlider.getValue(),
                       onsetMinMsSlider.getValue());

    if (isBandAnalysisEnabled())
    {
        sourceFile.analyzeBands(crossoverHz[0], crossoverHz[1], crossoverHz[2],
                                onsetThresholdSlider.getValue(),
                                onsetSilenceSlider.getValue(),
                                onsetMinMsSlider.getValue());
        buildSourceBandBuffers();
    }

    if (quantizeOnsetsButton.getToggleState())
    {
        sourceFile.quantize(numberBoxBpm.getText().getDoubleValue(),
                            numberBoxSubdiv.getText().getIntValue(),
                            (float) quantizeStrengthSlider.getValue());
    }
}

//==============================================================================
// [Analysis] Analyze Morph
//==============================================================================
void MainContentComponent::analyzeMorphFile()
{
    if (!morphFile.isLoaded())
        return;

    phaseVocoder.setMorphDebugCaptureEnabled(true);

    phaseVocoder.analyzeMorphTarget(morphFile.getBuffer(),
                                    morphFile.getSampleRate());

    phaseVocoder.setCrossSynthesisEnabled(morphFile.isLoaded() &&
                                          (float) textureMorphSlider.getValue() > 0.0f);
}

//==============================================================================
// [Analysis] Analyze Beat Ref
//==============================================================================
void MainContentComponent::analyzeBeatRefFile()
{
    if (!beatRefFile.isLoaded())
        return;

    beatRefFile.analyze("aubio",
                        onsetThresholdSlider.getValue(),
                        onsetSilenceSlider.getValue(),
                        onsetMinMsSlider.getValue());

    if (quantizeOnsetsButton.getToggleState())
    {
        beatRefFile.quantize(numberBoxBpm.getText().getDoubleValue(),
                             numberBoxSubdiv.getText().getIntValue(),
                             (float) quantizeStrengthSlider.getValue());
    }
}

//==============================================================================
// [Analysis] Reanalyze
//==============================================================================
void MainContentComponent::reanalyze()
{
    analyzeSourceFile();
    analyzeMorphFile();
    analyzeBeatRefFile();

    rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());

    if (isBandAnalysisEnabled())
        rebuildBandSegments((float) gaussWidthSlider.getValue());

    if (exportNextAnalysis)
    {
        exportNextAnalysis = false;
        exportAnalysisData();

        juce::MessageManager::callAsync([this]()
        {
            exportDataButton.setButtonText("Export Data");
            exportDataButton.setEnabled(true);

            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Export Complete",
                "CSV files written to:\n"
                + MorsDataExport::getSessionDir().getFullPathName());
        });
    }

    repaint();
}

//==============================================================================
// [Spectrogram] Build generic spectrogram image
//==============================================================================
juce::Image MainContentComponent::buildSpectrogramFromBuffer(const juce::AudioBuffer<float>& buf, double /*sampleRate*/)
{
    juce::Image image;

    const int numSamples = buf.getNumSamples();
    if (numSamples <= 0)
        return image;

    constexpr int kFFTOrder = 10;
    constexpr int kFFTSize  = 1 << kFFTOrder;
    constexpr int kHopSize  = 256;
    constexpr int kNumBins  = kFFTSize / 2 + 1;

    juce::dsp::FFT fft(kFFTOrder);

    std::vector<float> window(kFFTSize);
    for (int i = 0; i < kFFTSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(
            2.0f * juce::MathConstants<float>::pi * i / (kFFTSize - 1)));

    const int numFrames = juce::jmax(1, (numSamples - kFFTSize) / kHopSize + 1);
    const float* src    = buf.getReadPointer(0);

    std::vector<std::vector<float>> logMags(numFrames, std::vector<float>(kNumBins, -100.0f));
    std::vector<juce::dsp::Complex<float>> cplx(kFFTSize);

    float dbMax = -1e10f;
    float dbMin =  1e10f;

    for (int f = 0; f < numFrames; ++f)
    {
        const int start = f * kHopSize;

        for (int i = 0; i < kFFTSize; ++i)
        {
            const int si = start + i;
            const float s = (si < numSamples) ? src[si] * window[i] : 0.0f;
            cplx[i] = { s, 0.0f };
        }

        fft.perform(cplx.data(), cplx.data(), false);

        for (int bin = 0; bin < kNumBins; ++bin)
        {
            const float re  = cplx[bin].real();
            const float im  = cplx[bin].imag();
            const float mag = std::sqrt(re * re + im * im) + 1e-10f;
            const float db  = 20.0f * std::log10f(mag);
            logMags[f][bin] = db;
            dbMax = std::max(dbMax, db);
            dbMin = std::min(dbMin, db);
        }
    }

    dbMin = std::max(dbMin, dbMax - 80.0f);
    const float dbRange = std::max(1.0f, dbMax - dbMin);

    const int imgW = juce::jmin(numFrames, 1024);
    const int imgH = 256;
    image = juce::Image(juce::Image::RGB, imgW, imgH, true);

    for (int px = 0; px < imgW; ++px)
    {
        const int frame = (int) ((float) px / imgW * numFrames);

        for (int py = 0; py < imgH; ++py)
        {
            const float normY = (float) (imgH - 1 - py) / (float) (imgH - 1);
            const int bin = juce::jlimit(0, kNumBins - 1,
                                         (int) (std::powf(10.0f,
                                                          normY * std::log10f((float) kNumBins)) - 1));

            const float norm = juce::jlimit(0.0f, 1.0f,
                                            (logMags[frame][bin] - dbMin) / dbRange);

            juce::Colour col;
            if      (norm < 0.25f) col = juce::Colours::black.interpolatedWith(juce::Colour(0xff001080), norm * 4.0f);
            else if (norm < 0.50f) col = juce::Colour(0xff001080).interpolatedWith(juce::Colours::cyan, (norm - 0.25f) * 4.0f);
            else if (norm < 0.75f) col = juce::Colours::cyan.interpolatedWith(juce::Colours::yellow, (norm - 0.50f) * 4.0f);
            else                   col = juce::Colours::yellow.interpolatedWith(juce::Colours::white, (norm - 0.75f) * 4.0f);

            image.setPixelAt(px, py, col);
        }
    }

    return image;
}

void MainContentComponent::buildSpectrogramImage()
{
    spectrogramImage = juce::Image();
    if (!morphFile.isLoaded())
        return;

    spectrogramImage = buildSpectrogramFromBuffer(morphFile.getBuffer(), morphFile.getSampleRate());
}

void MainContentComponent::buildBandSpectrogramImage(int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return;

    auto& band = bandSlots[bandIndex];
    band.spectrogramImage = juce::Image();

    if (!band.morphFile.isLoaded())
        return;

    band.spectrogramImage = buildSpectrogramFromBuffer(band.morphFile.getBuffer(),
                                                       band.morphFile.getSampleRate());
}

//==============================================================================
// [Processing] Algorithm selection
//==============================================================================
void MainContentComponent::onAlgorithmChanged()
{
    const int algoId = algorithmSelector.getSelectedId();

    if      (algoId == 1) currentStretchAlgo = &phaseVocoder;
    else if (algoId == 2) currentStretchAlgo = &sola;
    else if (algoId == 3) currentStretchAlgo = &granular;

    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& band = bandSlots[b];

        if      (algoId == 1) band.stretcher = std::make_unique<PhaseVocoder>(1024, 512);
        else if (algoId == 2) band.stretcher = std::make_unique<Sola>(1024, 512);
        else if (algoId == 3) band.stretcher = std::make_unique<Granular>(1024, 512);

        if (band.stretcher)
            band.stretcher->prepare(lastSampleRate, lastBlockSize);

        if (auto* g = dynamic_cast<Granular*>(band.stretcher.get()))
            g->setGrainParams(grainSizeSlider.getValue(),
                              grainScatterSlider.getValue(),
                              0.25f,
                              (int) grainDensitySlider.getValue());

        if (auto* pv = dynamic_cast<PhaseVocoder*>(band.stretcher.get()))
        {
            pv->clearMorphDebugFrames();
            pv->setMorphDebugCaptureEnabled(true);

            if (band.morphFile.isLoaded())
            {
                pv->analyzeMorphTarget(band.morphFile.getBuffer(),
                                       band.morphFile.getSampleRate());

                pv->setCrossSynthesisEnabled((float) textureMorphSlider.getValue() > 0.0f);
            }
            else
            {
                pv->setCrossSynthesisEnabled(false);
            }
        }
    }

    currentStretchAlgo->prepare(lastSampleRate, lastBlockSize);
    currentStretchAlgo->seek(playheadSeconds.load(std::memory_order_relaxed));
}

//==============================================================================
// [Processing] Rebuild global mapping
//==============================================================================
void MainContentComponent::rebuildSegmentsForAlpha(float alpha)
{
    const auto& refOnsets = beatRefFile.getOnsetTimes();
    const auto& srcOnsets = sourceFile.getOnsetTimes();

    if (srcOnsets.size() < 2 || refOnsets.size() < 2)
        return;

    OnsetMapper mapper;

//    const float sigmaScale  = (float) gaussWidthSlider.getValue();
//    const float guard       = 0.001f * (float) gaussGuardSlider.getValue();
//    const float displaceVal = (float) gaussDisplaceSlider.getValue();
//    const float drawVal     = 0.02f + 0.18f * (float) gaussDrawSlider.getValue();

    // alpha    = gaussWidthSlider (0 = dry/transparent, 1 = full morph)
    // sigmaScale is fixed — only alpha and displace are user-facing
    // sigmaScale is fixed — only alpha and displace are user-facing
    auto mapResult = mapper.mapOnsetsWithGaussian(
        srcOnsets,
        refOnsets,
        alpha,
        0.5f,                                        // sigmaScale
        0.01f,                                       // sigmaEps
        0.001 * gaussGuardSlider.getValue(),          // guard (ms → sec)
        (float) gaussDisplaceSlider.getValue(),       // displacementBoost
        0.5f,                                        // drawRange (you were missing this)
        false,                                       // captureMatrices
        OnsetMapper::CollisionMode::Proportional,     // <-- FIXED
        0.35f,                                       // spreadFactor
        0.001 * gaussGuardSlider.getValue()           // minSpacing
    );

    if (mapResult.morphedOnsets.size() < 2)
        return;

    auto morphed = mapResult.morphedOnsets;

    //--------------------------------------------------------------
    // Optional post-map quantization of morphed onsets
    //--------------------------------------------------------------
    if (quantizeOnsetsButton.getToggleState())
    {
        std::vector<double> grid = sourceFile.getGridTimes();

        // Fallback: build a grid if sourceFile grid is empty
        if (grid.empty())
        {
            const double bpm        = numberBoxBpm.getText().getDoubleValue();
            const int subdivision   = numberBoxSubdiv.getText().getIntValue();
            const double totalTime  = sourceFile.getLengthSeconds();

            if (bpm > 0.0 && subdivision > 0 && totalTime > 0.0)
            {
                const double gridSec = 60.0 / (bpm * (double) subdivision);
                for (double t = 0.0; t <= totalTime; t += gridSec)
                    grid.push_back(t);
            }
        }

        if (!grid.empty())
        {
            morphed = quantizeOnsetsToGrid(morphed,
                                           grid,
                                           (float) quantizeStrengthSlider.getValue());
        }
    }
    segments.clear();
    const size_t numSeg = std::min(srcOnsets.size(), morphed.size()) - 1;
    segments.reserve(numSeg);

    for (size_t i = 0; i < numSeg; ++i)
    {
        const double srcDur = srcOnsets[i + 1] - srcOnsets[i];
        const double outDur = morphed[i + 1]   - morphed[i];

        SegmentMapping s;
        s.startTime    = morphed[i];
        s.endTime      = morphed[i + 1];
        s.stretchRatio = juce::jlimit(0.001f, 1000.0f,
                                      (srcDur > 0.0) ? (float)(outDur / srcDur) : 1.0f);
        segments.push_back(s);

        MorsLog::logSegment((int) i, s.startTime, s.endTime, s.stretchRatio, -1);
    }

    morphedOnsets = morphed;
    segmantMapper.updateSegments(segments);

    MorsLog::logOnsetMap((int) srcOnsets.size(),
                         (int) refOnsets.size(),
                         mapResult.dtwPathLength,
                         mapResult.dtwFinalCost,
                         mapResult.dtwAvgCost,
                         -1);
}

//==============================================================================
// [Processing] Build filtered source band buffers
//==============================================================================
void MainContentComponent::buildSourceBandBuffers()
{
    if (!sourceFile.isLoaded())
        return;

    const auto& src      = sourceFile.getBuffer();
    const int numSamples = src.getNumSamples();
    const double sr      = sourceFile.getSampleRate();

    if (numSamples <= 0 || sr <= 0.0)
        return;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sr;
    spec.maximumBlockSize = (juce::uint32) juce::jmax(512, 4096);
    spec.numChannels      = 1;

    using LRType = juce::dsp::LinkwitzRileyFilterType;

    auto applyFilter = [&](const juce::AudioBuffer<float>& input,
                           LRType type, float freq) -> juce::AudioBuffer<float>
    {
        juce::dsp::LinkwitzRileyFilter<float> f;
        f.setType(type);
        f.setCutoffFrequency(freq);
        f.prepare(spec);

        juce::AudioBuffer<float> out(1, numSamples);
        out.copyFrom(0, 0, input, 0, 0, numSamples);
        juce::dsp::AudioBlock<float> block(out);
        f.process(juce::dsp::ProcessContextReplacing<float>(block));
        return out;
    };

    juce::AudioBuffer<float> mono(1, numSamples);
    mono.copyFrom(0, 0, src, 0, 0, numSamples);

    auto midHigh = applyFilter(mono,    LRType::highpass, crossoverHz[0]);
    auto high    = applyFilter(midHigh, LRType::highpass, crossoverHz[1]);

    if (kMaxBands >= 1) bandSlots[0].sourceBandBuffer = applyFilter(mono,    LRType::lowpass,  crossoverHz[0]);
    if (kMaxBands >= 2) bandSlots[1].sourceBandBuffer = applyFilter(midHigh, LRType::lowpass,  crossoverHz[1]);
    if (kMaxBands >= 3) bandSlots[2].sourceBandBuffer = applyFilter(high,    LRType::lowpass,  crossoverHz[2]);
    if (kMaxBands >= 4) bandSlots[3].sourceBandBuffer = applyFilter(high,    LRType::highpass, crossoverHz[2]);
}

//==============================================================================
// [Processing] Band count changed
//==============================================================================
void MainContentComponent::onBandCountChanged()
{
    activeBandCount = juce::jlimit(1, kMaxBands, bandCountSelector.getSelectedId());

    buildSourceBandBuffers();

    for (int b = 0; b < activeBandCount; ++b)
        if (bandSlots[b].stretcher)
            bandSlots[b].stretcher->prepare(lastSampleRate, lastBlockSize);

    refreshBandUiVisibility();
    resized();
    repaint();
}

//==============================================================================
// [Processing] Load per-band beat file
//==============================================================================
void MainContentComponent::loadBandBeatFile(int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return;

    auto& band = bandSlots[bandIndex];
    const int idx = band.beatSelector.getSelectedItemIndex();

    if (idx < 0 || idx >= sampleLibraryFiles.size())
        return;

    auto file = sampleLibraryFiles[idx];
    if (!band.beatFile.loadFromFile(file))
        return;

    band.beatPath = file;

    band.beatFile.analyze("aubio",
                          onsetThresholdSlider.getValue(),
                          onsetSilenceSlider.getValue(),
                          onsetMinMsSlider.getValue());

    if (quantizeOnsetsButton.getToggleState())
    {
        band.beatFile.quantize(numberBoxBpm.getText().getDoubleValue(),
                               numberBoxSubdiv.getText().getIntValue(),
                               (float) quantizeStrengthSlider.getValue());
    }

    rebuildBandSegments((float) gaussWidthSlider.getValue());
    repaint();
}

//==============================================================================
// [Processing] Load per-band morph file
//==============================================================================
void MainContentComponent::loadBandMorphFile(int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return;

    auto& band = bandSlots[bandIndex];
    const int idx = band.morphSelector.getSelectedItemIndex();

    if (idx < 0 || idx >= sampleLibraryFiles.size())
        return;

    auto file = sampleLibraryFiles[idx];
    if (!band.morphFile.loadFromFile(file))
        return;

    band.morphPath = file;
    buildBandSpectrogramImage(bandIndex);

    if (auto* pv = dynamic_cast<PhaseVocoder*>(band.stretcher.get()))
    {
        pv->clearMorphDebugFrames();
        pv->setMorphDebugCaptureEnabled(true);

        pv->analyzeMorphTarget(band.morphFile.getBuffer(),
                               band.morphFile.getSampleRate());

        pv->setCrossSynthesisEnabled((float) textureMorphSlider.getValue() > 0.0f);
    }

    repaint();
}

//==============================================================================
// [Processing] Rebuild per-band mapping
//==============================================================================
void MainContentComponent::rebuildBandSegments(float alpha)
{
    if (!sourceFile.isLoaded() || !isBandAnalysisEnabled())
        return;

//    const float sigmaScale  = (float) gaussWidthSlider.getValue();
//    const float guard       = 0.001f * (float) gaussGuardSlider.getValue();
//    const float displaceVal = (float) gaussDisplaceSlider.getValue();
//    const float drawVal     = 0.02f + 0.18f * (float) gaussDrawSlider.getValue();

    for (int b = 0; b < activeBandCount; ++b)
    {
        auto& band = bandSlots[b];

        const auto& srcOnsets = sourceFile.getBandOnsetTimes(b);
        const auto& refOnsets = band.beatFile.getOnsetTimes();

        if (srcOnsets.size() < 2 || refOnsets.size() < 2)
        {
            band.segments.clear();
            band.morphedOnsets.clear();
            continue;
        }

        // alpha    = gaussWidthSlider (0 = dry/transparent, 1 = full morph)
        // sigmaScale is fixed — only alpha and displace are user-facing
        auto mapResults = OnsetMapper::mapOnsetsWithGaussian(
            srcOnsets,
            refOnsets,
            alpha,
            0.5f,                                        // sigmaScale
            0.01f,                                       // sigmaEps
            0.001 * gaussGuardSlider.getValue(),          // guard (ms → sec)
            (float) gaussDisplaceSlider.getValue(),       // displacementBoost
            0.5f,                                        // drawRange (you were missing this)
            false,                                       // captureMatrices
            OnsetMapper::CollisionMode::Proportional,     // <-- FIXED
            0.35f,                                       // spreadFactor
            0.001 * gaussGuardSlider.getValue()           // minSpacing
        );
        auto morphed = mapResults.morphedOnsets;

        if (morphed.size() < 2)
            continue;

        //--------------------------------------------------------------
        // Optional post-map quantization of morphed onsets
        //--------------------------------------------------------------
        if (quantizeOnsetsButton.getToggleState())
        {
            std::vector<double> grid = sourceFile.getGridTimes();

            if (grid.empty())
            {
                const double bpm       = numberBoxBpm.getText().getDoubleValue();
                const int subdivision  = numberBoxSubdiv.getText().getIntValue();
                const double totalTime = sourceFile.getLengthSeconds();

                if (bpm > 0.0 && subdivision > 0 && totalTime > 0.0)
                {
                    const double gridSec = 60.0 / (bpm * (double) subdivision);
                    for (double t = 0.0; t <= totalTime; t += gridSec)
                        grid.push_back(t);
                }
            }

            if (!grid.empty())
            {
                morphed = quantizeOnsetsToGrid(morphed, grid, 1.0f);

                OnsetMapper mapper;
            }
        }

        band.morphedOnsets = morphed;
        band.segments.clear();

        band.segments.clear();
        const size_t numSeg = std::min(srcOnsets.size(), morphed.size()) - 1;
        band.segments.reserve(numSeg);

        for (size_t i = 0; i < numSeg; ++i)
        {
            const double srcDur = srcOnsets[i + 1] - srcOnsets[i];
            const double outDur = morphed[i + 1]   - morphed[i];

            SegmentMapping s;
            s.startTime    = morphed[i];
            s.endTime      = morphed[i + 1];
            s.stretchRatio = juce::jlimit(0.001f, 1000.0f,
                                          (srcDur > 0.0) ? (float)(outDur / srcDur) : 1.0f);
            band.segments.push_back(s);
        }

        band.segmentMapper.updateSegments(band.segments);
    }
}

//==============================================================================
// [Analysis] Quantize helper
//==============================================================================
std::vector<double> MainContentComponent::quantizeOnsetsToGrid(const std::vector<double>& onsets,
                                                               const std::vector<double>& grid,
                                                               float strength)
{
    std::vector<double> out;
    out.reserve(onsets.size());

    for (double t : onsets)
    {
        auto it = std::min_element(grid.begin(), grid.end(),
                                   [&](double a, double b)
                                   { return std::abs(a - t) < std::abs(b - t); });

        const double quantized = (it != grid.end()) ? *it : t;
        out.push_back(t + (double) strength * (quantized - t));
    }

    return out;
}


//==============================================================================
// [Analysis] Rebuild Mapping from UI Changes
//==============================================================================
void MainContentComponent::rebuildMappingsFromUi()
{
    rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());

    if (isBandAnalysisEnabled())
        rebuildBandSegments((float) gaussWidthSlider.getValue());

    lastAppliedGaussWidth   .store((float) gaussWidthSlider.getValue(),    std::memory_order_relaxed);
    lastAppliedGaussDisplace.store((float) gaussDisplaceSlider.getValue(), std::memory_order_relaxed);
    lastAppliedGaussGuard   .store((float) gaussGuardSlider.getValue(),    std::memory_order_relaxed);

    mappingDirty.store(false, std::memory_order_relaxed);

    if (exportNextAnalysis)
    {
        exportNextAnalysis = false;
        exportAnalysisData();

        juce::MessageManager::callAsync([this]()
        {
            exportDataButton.setButtonText("Export Data");
            exportDataButton.setEnabled(true);
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Export Complete",
                "CSV files written to:\n" + MorsDataExport::getSessionDir().getFullPathName());
        });
    }

    repaint();
}

//==============================================================================
// [Export] Write all analysis CSVs for the current session
//==============================================================================
void MainContentComponent::exportAnalysisData()
{
    const auto label = loadedSourceFile.getFileNameWithoutExtension().replace(" ", "_");
    MorsDataExport::beginSession(label);

    // 1. Onset times
    MorsDataExport::writeOnsets("source",  sourceFile.getOnsetTimes());
    MorsDataExport::writeOnsets("beatref", beatRefFile.getOnsetTimes());
    MorsDataExport::writeOnsets("morphed", morphedOnsets);

    // 2. Novelty functions
    MorsDataExport::writeNovelty("source",  onsetSource.getNoveltyEnergy(),  onsetSource.getFsFeature());
    MorsDataExport::writeNovelty("beatref", onsetBeatRef.getNoveltyEnergy(), onsetBeatRef.getFsFeature());

    // 3. DTW matrices — re-run once with captureMatrices = true
    {
        const auto& srcOnsets = sourceFile.getOnsetTimes();
        const auto& refOnsets = beatRefFile.getOnsetTimes();

        if (srcOnsets.size() >= 2 && refOnsets.size() >= 2)
        {
            const auto r = OnsetMapper::mapOnsetsWithGaussian(
                srcOnsets,
                refOnsets,
                (float) gaussWidthSlider.getValue(),
                0.5f,
                0.01f,
                0.001 * gaussGuardSlider.getValue(),
                (float) gaussDisplaceSlider.getValue(),
                0.35f,
                true);   // captureMatrices

            MorsDataExport::writeDTW(r.dtwCostMatrix,
                                     r.dtwAccumMatrix,
                                     r.dtwPath,
                                     (int) srcOnsets.size(),
                                     (int) refOnsets.size(),
                                     r.dtwFinalCost,
                                     r.dtwAvgCost);
        }
    }

    // 4. Segment mappingss
    MorsDataExport::writeSegments(segments);

    if (isBandAnalysisEnabled() && activeBandCount > 1)
    {
        for (int b = 0; b < activeBandCount; ++b)
        {
            if (auto* pv = dynamic_cast<PhaseVocoder*>(bandSlots[b].stretcher.get()))
            {
                LOGI("EXPORT", "Band " + juce::String(b + 1)
                               + " morph debug frame count = "
                               + juce::String((int) pv->getMorphDebugFrames().size()));

                MorsDataExport::writeMorphDebugFrames(pv->getMorphDebugFrames());
            }
        }
    }
    else
    {
        if (auto* pv = dynamic_cast<PhaseVocoder*>(currentStretchAlgo))
        {
            LOGI("EXPORT", "Main morph debug frame count = "
                           + juce::String((int) pv->getMorphDebugFrames().size()));

            MorsDataExport::writeMorphDebugFrames(pv->getMorphDebugFrames());
        }
        else
        {
            LOGI("EXPORT", "Current algorithm is not PhaseVocoder — no morph debug frames to export");
        }
    }

    // 5. Reset perf counter so next playback fills a clean performance.csv
    perfBlockCounter = 0;

    LOGI("EXPORT", "Export complete: " + MorsDataExport::getSessionDir().getFullPathName());
}

//==============================================================================
// [User Input] Button click
//==============================================================================
void MainContentComponent::buttonClicked(juce::Button* button)
{
    if      (button == &sourceButtonOpen)   openSourceFile();
    else if (button == &morphButtonOpen)    openMorphFile();
    else if (button == &beatRefButtonOpen)  openBeatRefFile();
    else if (button == &analyzeButton)
    {
        granular.prepare(lastSampleRate, lastBlockSize);
        sola.prepare(lastSampleRate, lastBlockSize);
        phaseVocoder.prepare(lastSampleRate, lastBlockSize);

        for (int b = 0; b < activeBandCount; ++b)
            if (bandSlots[b].stretcher)
                bandSlots[b].stretcher->prepare(lastSampleRate, lastBlockSize);

        rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());
        if (isBandAnalysisEnabled())
            rebuildBandSegments((float) gaussWidthSlider.getValue());
        reanalyze();
    }
    else if (button == &audioConfigButton)
    {
        menuAudioConfig(&audioConfigButton);
    }
    else if (button == &playButton)
    {
        if (sourceFile.getBuffer().getNumSamples() > 0)
        {
            double t = playheadSeconds.load(std::memory_order_relaxed);

            if (sourceFile.getLengthSeconds() > 0.0 && t >= sourceFile.getLengthSeconds())
            {
                t = 0.0;
                playheadSeconds.store(0.0, std::memory_order_relaxed);
                scrubber.setValue(0.0, juce::dontSendNotification);
            }

            if (auto* pv = dynamic_cast<PhaseVocoder*>(currentStretchAlgo))
            {
                pv->clearMorphDebugFrames();
                pv->setMorphDebugCaptureEnabled(true);
            }

            if (isBandAnalysisEnabled() && activeBandCount > 1)
            {
                for (int b = 0; b < activeBandCount; ++b)
                {
                    if (auto* pv = dynamic_cast<PhaseVocoder*>(bandSlots[b].stretcher.get()))
                    {
                        pv->clearMorphDebugFrames();
                        pv->setMorphDebugCaptureEnabled(true);
                    }
                }
            }

            currentStretchAlgo->seek(t);
            isPlayingRT.store(true, std::memory_order_release);
            changeState(PLAYING);
        }
    }
    else if (button == &stopButton)
    {
        isPlayingRT.store(false, std::memory_order_release);
        if (state == RECORDING)
            fileWriter.close();
        changeState(IDLE);
        MorsDataExport::flushPerfLog();
    }
    else if (button == &exportDataButton)
    {
        if (sourceFile.isLoaded() && beatRefFile.isLoaded())
        {
            exportDataButton.setButtonText("Exporting...");
            exportDataButton.setEnabled(false);

            exportAnalysisData();

            exportDataButton.setButtonText("Export Data");
            exportDataButton.setEnabled(true);

            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Export Complete",
                "CSV files written to:\n" + MorsDataExport::getSessionDir().getFullPathName());
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Export Data",
                "Please load source and beat reference files first.");
        }
    }
    else if (button == &recordButton)
    {
        if (sourceFile.getBuffer().getNumSamples() > 0)
        {
            // Start from the beginning for deterministic full-length capture
            playheadSeconds.store(0.0, std::memory_order_relaxed);

            // Reset all stretchers to time 0
            currentStretchAlgo->seek(0.0);
            phaseVocoder.seek(0.0);
            sola.seek(0.0);
            granular.seek(0.0);

            for (int b = 0; b < activeBandCount; ++b)
            {
                if (bandSlots[b].stretcher)
                    bandSlots[b].stretcher->seek(0.0);

                if (auto* pv = dynamic_cast<PhaseVocoder*>(bandSlots[b].stretcher.get()))
                {
                    pv->clearMorphDebugFrames();
                    pv->setMorphDebugCaptureEnabled(true);
                }
            }

            if (auto* pv = dynamic_cast<PhaseVocoder*>(currentStretchAlgo))
            {
                pv->clearMorphDebugFrames();
                pv->setMorphDebugCaptureEnabled(true);
            }

            scrubber.setValue(0.0, juce::dontSendNotification);

            // Start file recording
            recordFile();

            // Start playback immediately
            isPlayingRT.store(true, std::memory_order_release);
            changeState(RECORDING);
        }
    }
}

//==============================================================================
// [User Input] Slider value changed
//==============================================================================
void MainContentComponent::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &scrubber)
    {
        const double t = scrubber.getValue();
        playheadSeconds.store(t, std::memory_order_relaxed);

        if (!isPlayingRT.load(std::memory_order_acquire))
            currentStretchAlgo->seek(t);

        sliderDebounceCounter = 0;
        return;
    }
}

//==============================================================================
// [User Input] Change listener
//==============================================================================
void MainContentComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &sourceFile.getThumbnail()
     || source == &morphFile.getThumbnail()
     || source == &beatRefFile.getThumbnail())
    {
        thumbnailChanged();
        return;
    }

    for (int b = 0; b < activeBandCount; ++b)
    {
        if (source == &bandSlots[b].beatFile.getThumbnail()
         || source == &bandSlots[b].morphFile.getThumbnail())
        {
            thumbnailChanged();
            return;
        }
    }
}

//==============================================================================
// [Application State] State transition
//==============================================================================
void MainContentComponent::changeState(AppState newState)
{
    if (state == newState)
        return;

    state = newState;

    switch (state)
    {
        case IDLE:
            stopButton.setEnabled(false);
            playButton.setEnabled(sourceFile.getBuffer().getNumSamples() > 0);
            recordButton.setEnabled(sourceFile.getBuffer().getNumSamples() > 0);
            loopButton.setEnabled(true);
            scrubber.setEnabled(sourceFile.getBuffer().getNumSamples() > 0);
            LOGI("APP_STATE", "IDLE");
            break;

        case PLAYING:
            stopButton.setEnabled(true);
            playButton.setEnabled(false);
            recordButton.setEnabled(true);
            loopButton.setEnabled(true);
            scrubber.setEnabled(true);
            LOGI("APP_STATE", "PLAYING");
            break;

        case RECORDING:
            stopButton.setEnabled(true);
            playButton.setEnabled(false);
            recordButton.setEnabled(false);
            loopButton.setEnabled(true);
            scrubber.setEnabled(false);
            LOGI("APP_STATE", "RECORDING");
            break;

        default:
            break;
    }
}

//==============================================================================
// [Application State] Timer callback
//==============================================================================
void MainContentComponent::timerCallback()
{
    MorsDebug::flushRealtime();
    MorsDataExport::flushPerfLog();

    if (isPlayingRT.load(std::memory_order_acquire))
    {
        scrubber.setValue(playheadSeconds.load(std::memory_order_relaxed),
                          juce::dontSendNotification);
        repaint();
    }

    //==============================================================================
    // [TIMER] Unified debounce — fires when ANY mapping parameter changes
    //==============================================================================
    if (sourceFile.isLoaded() && beatRefFile.isLoaded()
        && !sourceFile.getOnsetTimes().empty()
        && !beatRefFile.getOnsetTimes().empty())
    {
        const float width    = (float) gaussWidthSlider.getValue();
        const float displace = (float) gaussDisplaceSlider.getValue();
        const float guard    = (float) gaussGuardSlider.getValue();

        const bool anyChanged =
            mappingDirty.load(std::memory_order_relaxed)
            || std::abs(width   - lastAppliedGaussWidth   .load(std::memory_order_relaxed)) > 0.001f
            || std::abs(displace- lastAppliedGaussDisplace.load(std::memory_order_relaxed)) > 0.001f
            || std::abs(guard   - lastAppliedGaussGuard   .load(std::memory_order_relaxed)) > 0.001f;

        if (anyChanged)
        {
            sliderDebounceCounter++;

            if (sliderDebounceCounter >= DEBOUNCE_FRAMES)
            {
                rebuildSegmentsForAlpha((float) gaussWidthSlider.getValue());

                if (isBandAnalysisEnabled())
                    rebuildBandSegments((float) gaussWidthSlider.getValue());

                lastAppliedGaussWidth   .store(width,    std::memory_order_relaxed);
                lastAppliedGaussDisplace.store(displace, std::memory_order_relaxed);
                lastAppliedGaussGuard   .store(guard,    std::memory_order_relaxed);
                mappingDirty.store(false, std::memory_order_relaxed);

                sliderDebounceCounter = 0;
                repaint();
            }
        }
        else
        {
            sliderDebounceCounter = 0;
        }
    }
}

//==============================================================================
// [Graphics] Helper
//==============================================================================
static juce::Colour invertedRgb(juce::Colour c)
{
    return juce::Colour::fromRGBA(
        (juce::uint8)(255 - c.getRed()),
        (juce::uint8)(255 - c.getGreen()),
        (juce::uint8)(255 - c.getBlue()),
        c.getAlpha()
    );
}

void MainContentComponent::drawBufferWaveform(juce::Graphics& g,
                                              const juce::AudioBuffer<float>& buffer,
                                              juce::Rectangle<int> bounds)
{
    if (buffer.getNumSamples() <= 0)
    {
        g.setColour(juce::Colours::whitesmoke);
        g.drawFittedText("No Band Source", bounds, juce::Justification::centred, 1);
        return;
    }

    const int n = buffer.getNumSamples();

    // Find peak amplitude across all channels so we can normalise to ±1 for display.
    // Band-filtered buffers often have peaks well below 1.0 — without this the
    // waveform would appear nearly flat even when audio is present.
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            peak = std::max(peak, std::abs(data[i]));
    }
    const float normalise = (peak > 1e-6f) ? (1.0f / peak) : 1.0f;

    juce::Path p;
    const float* x = buffer.getReadPointer(0);

    p.startNewSubPath((float) bounds.getX(), (float) bounds.getCentreY());

    for (int px = 0; px < bounds.getWidth(); ++px)
    {
        const int i = juce::jlimit(0, n - 1, (px * n) / juce::jmax(1, bounds.getWidth()));
        const float y = x[i] * normalise;   // scale to ±1 before mapping to pixels
        const float py = juce::jmap(y, -1.0f, 1.0f,
                                    (float) bounds.getBottom(),
                                    (float) bounds.getY());
        p.lineTo((float) (bounds.getX() + px), py);
    }

    g.setColour(juce::Colours::lightgrey);
    g.strokePath(p, juce::PathStrokeType(1.0f));
}

int MainContentComponent::getVisibleBandVisualizerCount() const
{
    if (isBandAnalysisEnabled() && activeBandCount > 1)
        return juce::jlimit(1, kMaxBands, activeBandCount);

    return 1;
}

void MainContentComponent::paintBandVisualizer(juce::Graphics& g,
                                               BandSlot& band,
                                               juce::Rectangle<int> area,
                                               int bandIndex)
{
    const bool fullSpectrumMode = !isBandAnalysisEnabled();

    auto sourceRect = area.removeFromLeft(area.getWidth() / 3);
    auto beatRect   = area.removeFromLeft(area.getWidth() / 2);
    auto morphRect  = area;

    auto sourceInner = sourceRect;
    auto beatInner   = beatRect;
    auto morphInner  = morphRect;

    g.setColour(juce::Colours::darkgrey);
    g.fillRect(sourceRect);
    g.fillRect(beatRect);
    g.fillRect(morphRect);

    g.setColour(juce::Colours::whitesmoke);
    g.drawRect(sourceRect);
    g.drawRect(beatRect);
    g.drawRect(morphRect);

    const juce::String bandTitle = fullSpectrumMode
        ? "Band 1"
        : "Band " + juce::String(bandIndex + 1);

    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(11.0f);
    g.drawText(bandTitle + " Source",
               sourceInner.removeFromTop(14),
               juce::Justification::centredLeft, false);
    g.drawText(bandTitle + " Beat",
               beatInner.removeFromTop(14),
               juce::Justification::centredLeft, false);
    g.drawText(bandTitle + " Morph",
               morphInner.removeFromTop(14),
               juce::Justification::centredLeft, false);

    //--------------------------------------------------------------------------
    // SOURCE PANEL
    //--------------------------------------------------------------------------
    if (fullSpectrumMode)
    {
        if (sourceFile.getThumbnail().getNumChannels() > 0
         && sourceFile.getThumbnail().getTotalLength() > 0.0)
        {
            g.setColour(juce::Colours::grey);
            sourceFile.getThumbnail().drawChannels(g,
                                                   sourceInner,
                                                   0.0,
                                                   sourceFile.getThumbnail().getTotalLength(),
                                                   0.75f);

            drawMorphedWaveOverlay(g,
                                   sourceInner,
                                   sourceFile.getThumbnail(),
                                   sourceFile.getLengthSeconds(),
                                   sourceFile.getOnsetTimes(),
                                   morphedOnsets,
                                   juce::Colours::lightblue.withAlpha(0.6f));

            drawOnsetLines(g,
                           sourceFile.getOnsetTimes(),
                           sourceFile.getLengthSeconds(),
                           sourceInner,
                           juce::Colours::black.withAlpha(0.75f),
                           1.0f);

            drawOnsetLines(g,
                           morphedOnsets,
                           sourceFile.getLengthSeconds(),
                           sourceInner,
                           juce::Colours::orange.withAlpha(0.9f),
                           1.5f);
        }
        else
        {
            g.setColour(juce::Colours::whitesmoke);
            g.drawFittedText("No Source File", sourceInner, juce::Justification::centred, 1);
        }
    }
    else
    {
        drawBufferWaveform(g, band.sourceBandBuffer, sourceInner);

        const auto& srcBandOnsets = sourceFile.getBandOnsetTimes(bandIndex);

        drawOnsetLines(g,
                       srcBandOnsets,
                       sourceFile.getLengthSeconds(),
                       sourceInner,
                       juce::Colours::black.withAlpha(0.75f),
                       1.0f);

        drawOnsetLines(g,
                       band.morphedOnsets,
                       sourceFile.getLengthSeconds(),
                       sourceInner,
                       juce::Colours::orange.withAlpha(0.9f),
                       1.5f);
    }

    //--------------------------------------------------------------------------
    // BEAT PANEL
    //--------------------------------------------------------------------------
    constexpr float sigmaScale  = 0.5f;   // fixed — gaussWidthSlider now drives alpha, not kernel width
    constexpr float sigmaEps    = 0.01f;
    const float     displaceViz = (float) gaussDisplaceSlider.getValue();
    constexpr float drawViz     = 0.35f;  // fixed drawRange — no slider

    if (fullSpectrumMode)
    {
        if (beatRefFile.getThumbnail().getNumChannels() > 0
         && beatRefFile.getThumbnail().getTotalLength() > 0.0)
        {
            g.setColour(juce::Colours::grey);
            beatRefFile.getThumbnail().drawChannels(g,
                                                    beatInner,
                                                    0.0,
                                                    beatRefFile.getThumbnail().getTotalLength(),
                                                    0.75f);

            drawGaussianSpread(g,
                               beatRefFile.getOnsetTimes(),
                               beatRefFile.getLengthSeconds(),
                               beatInner,
                               sigmaScale,
                               sigmaEps,
                               displaceViz,
                               drawViz);

            drawOnsetLines(g,
                           beatRefFile.getOnsetTimes(),
                           beatRefFile.getLengthSeconds(),
                           beatInner,
                           juce::Colours::black.withAlpha(0.75f),
                           1.0f);
        }
        else
        {
            g.setColour(juce::Colours::whitesmoke);
            g.drawFittedText("No Beat File", beatInner, juce::Justification::centred, 1);
        }
    }
    else
    {
        if (band.beatFile.getThumbnail().getNumChannels() > 0
         && band.beatFile.getThumbnail().getTotalLength() > 0.0)
        {
            g.setColour(juce::Colours::grey);
            band.beatFile.getThumbnail().drawChannels(g,
                                                      beatInner,
                                                      0.0,
                                                      band.beatFile.getThumbnail().getTotalLength(),
                                                      0.75f);

            drawGaussianSpread(g,
                               band.beatFile.getOnsetTimes(),
                               band.beatFile.getLengthSeconds(),
                               beatInner,
                               sigmaScale,
                               sigmaEps,
                               displaceViz,
                               drawViz);

            drawOnsetLines(g,
                           band.beatFile.getOnsetTimes(),
                           band.beatFile.getLengthSeconds(),
                           beatInner,
                           juce::Colours::black.withAlpha(0.75f),
                           1.0f);
        }
        else
        {
            g.setColour(juce::Colours::whitesmoke);
            g.drawFittedText("No Beat File", beatInner, juce::Justification::centred, 1);
        }
    }

    //--------------------------------------------------------------------------
    // MORPH PANEL
    //--------------------------------------------------------------------------
    if (fullSpectrumMode)
    {
        if (spectrogramImage.isValid())
        {
            g.drawImage(spectrogramImage,
                        morphInner.toFloat(),
                        juce::RectanglePlacement::stretchToFit);
        }
        else
        {
            g.setColour(juce::Colours::whitesmoke);
            g.drawFittedText("No Morph File", morphInner, juce::Justification::centred, 1);
        }
    }
    else
    {
        if (band.spectrogramImage.isValid())
        {
            g.drawImage(band.spectrogramImage,
                        morphInner.toFloat(),
                        juce::RectanglePlacement::stretchToFit);
        }
        else
        {
            g.setColour(juce::Colours::whitesmoke);
            g.drawFittedText("No Morph File", morphInner, juce::Justification::centred, 1);
        }
    }
}

void MainContentComponent::drawOnsetLines(juce::Graphics& g,
                                          const std::vector<double>& onsets,
                                          double totalLengthSeconds,
                                          juce::Rectangle<int> bounds,
                                          juce::Colour colour,
                                          float thickness)
{
    if (onsets.empty() || totalLengthSeconds <= 0.0 || bounds.getWidth() <= 0)
        return;

    // Clip to the panel so markers with out-of-range times can never
    // bleed into adjacent panels (e.g. when morphedOnsets exceed file length).
    g.saveState();
    g.reduceClipRegion(bounds);
    g.setColour(colour);

    for (double t : onsets)
    {
        if (t < 0.0 || t > totalLengthSeconds)
            continue;

        const double norm = t / totalLengthSeconds;
        const int x = juce::jlimit(bounds.getX(),
                                   bounds.getRight(),
                                   bounds.getX() + (int) std::round(norm * bounds.getWidth()));

        g.drawLine((float) x,
                   (float) bounds.getY(),
                   (float) x,
                   (float) bounds.getBottom(),
                   thickness);
    }

    g.restoreState();
}

void MainContentComponent::drawGaussianSpread(juce::Graphics& g,
                                              const std::vector<double>& onsets,
                                              double totalLengthSeconds,
                                              juce::Rectangle<int> bounds,
                                              float sigmaScale,
                                              float sigmaEps,
                                              float displacementBoost,
                                              float drawRange)
{
    if (onsets.empty() || totalLengthSeconds <= 0.0 || bounds.getWidth() <= 0)
        return;

    // Hard-clip everything to the panel so gradients never bleed into
    // adjacent panels regardless of how wide sigmaScale or drawRange are.
    g.saveState();
    g.reduceClipRegion(bounds);

    const size_t N = onsets.size();
    std::vector<double> sigma(N, sigmaEps);

    for (size_t i = 0; i < N; ++i)
    {
        const double ioi =
            (i + 1 < N) ? (onsets[i + 1] - onsets[i])
                        : (i > 0 ? (onsets[i] - onsets[i - 1]) : 0.1);

        sigma[i] = std::max((double) sigmaScale * ioi, (double) sigmaEps);
    }

    for (size_t i = 0; i < N; ++i)
    {
        const double t = onsets[i];
        if (t < 0.0 || t > totalLengthSeconds)
            continue;

        const int x = bounds.getX() + (int) std::round((t / totalLengthSeconds) * bounds.getWidth());

        // ── Layer 1: Gaussian kernel width (sigmaScale / sigmaEps) ─────────
        const int gradW = juce::jmax(2, (int) std::round((sigma[i] / totalLengthSeconds) * bounds.getWidth()));

        juce::ColourGradient gradRight(
            juce::Colours::white.withAlpha(0.35f), (float) x, 0.0f,
            juce::Colours::white.withAlpha(0.0f),  (float) (x + gradW), 0.0f,
            false
        );
        g.setGradientFill(gradRight);
        g.fillRect(x, bounds.getY(), gradW, bounds.getHeight());

        juce::ColourGradient gradLeft(
            juce::Colours::white.withAlpha(0.35f), (float) x, 0.0f,
            juce::Colours::white.withAlpha(0.0f),  (float) (x - gradW), 0.0f,
            false
        );
        g.setGradientFill(gradLeft);
        g.fillRect(x - gradW, bounds.getY(), gradW, bounds.getHeight());

        // ── Layer 2: Displacement boost range (drawRange / displacementBoost) ─
        // Rendered as a wider, amber-tinted halo whose opacity scales with
        // displacementBoost and whose radius in pixels matches drawRange seconds.
        // This makes both sliders immediately visible in the beat panel.
        if (displacementBoost > 0.0f && drawRange > 0.0f)
        {
            const float boostAlpha = juce::jlimit(0.0f, 0.25f, displacementBoost * 0.12f);
            const int   boostW     = juce::jmax(gradW + 1,
                                                (int) std::round((drawRange / (float) totalLengthSeconds)
                                                                 * bounds.getWidth()));

            juce::ColourGradient boostRight(
                juce::Colour(0xffFFAA00).withAlpha(boostAlpha), (float) x, 0.0f,
                juce::Colour(0xffFFAA00).withAlpha(0.0f),       (float) (x + boostW), 0.0f,
                false
            );
            g.setGradientFill(boostRight);
            g.fillRect(x, bounds.getY(), boostW, bounds.getHeight());

            juce::ColourGradient boostLeft(
                juce::Colour(0xffFFAA00).withAlpha(boostAlpha), (float) x, 0.0f,
                juce::Colour(0xffFFAA00).withAlpha(0.0f),       (float) (x - boostW), 0.0f,
                false
            );
            g.setGradientFill(boostLeft);
            g.fillRect(x - boostW, bounds.getY(), boostW, bounds.getHeight());
        }
    }

    g.restoreState();
}

void MainContentComponent::drawMorphedWaveOverlay(juce::Graphics& g,
                                                  juce::Rectangle<int> bounds,
                                                  juce::AudioThumbnail& thumbnail,
                                                  double totalLengthSeconds,
                                                  const std::vector<double>& srcOnsets,
                                                  const std::vector<double>& morphedOnsetsIn,
                                                  juce::Colour colour)
{
    if (srcOnsets.size() < 2 || morphedOnsetsIn.size() < 2 || totalLengthSeconds <= 0.0)
        return;

    // Hard-clip to the panel so segments that extend beyond the source file
    // duration (e.g. after a large displacementBoost) cannot bleed off-screen.
    g.saveState();
    g.reduceClipRegion(bounds);

    const size_t numSeg = std::min(srcOnsets.size(), morphedOnsetsIn.size()) - 1;

    for (size_t i = 0; i < numSeg; ++i)
    {
        const double srcStart   = srcOnsets[i];
        const double srcEnd     = srcOnsets[i + 1];
        const double morphStart = morphedOnsetsIn[i];
        const double morphEnd   = morphedOnsetsIn[i + 1];

        // Clamp pixel positions to within the panel before building the rect.
        const int pixMorphStart = juce::jlimit(bounds.getX(),
                                               bounds.getRight(),
                                               bounds.getX() + (int) std::round((morphStart / totalLengthSeconds) * bounds.getWidth()));
        const int pixMorphEnd   = juce::jlimit(bounds.getX(),
                                               bounds.getRight(),
                                               bounds.getX() + (int) std::round((morphEnd   / totalLengthSeconds) * bounds.getWidth()));

        juce::Rectangle<int> dstRect(pixMorphStart,
                                     bounds.getY(),
                                     pixMorphEnd - pixMorphStart,
                                     bounds.getHeight());

        // Also intersect with panel bounds for absolute safety.
        dstRect = dstRect.getIntersection(bounds);

        if (dstRect.getWidth() <= 0)
            continue;

        g.setColour(colour);
        thumbnail.drawChannels(g, dstRect, srcStart, srcEnd, 0.75f);
    }

    g.restoreState();
}
//==============================================================================
// [Graphics] Paint
//==============================================================================
void MainContentComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);

    const int visibleBandRows = getVisibleBandVisualizerCount();

    for (int b = 0; b < visibleBandRows; ++b)
        paintBandVisualizer(g, bandSlots[b], bandSlots[b].visualBounds, b);
}

//==============================================================================
// [Graphics] Thumbnail changed
//==============================================================================
void MainContentComponent::thumbnailChanged()
{
    repaint();
}

void MainContentComponent::paintIfNoFileLoaded(juce::Graphics& g, juce::Rectangle<int>& bounds)
{
    g.setColour(juce::Colours::darkgrey);
    g.fillRect(bounds);
    g.setColour(juce::Colours::whitesmoke);
    g.drawFittedText("No File Loaded", bounds, juce::Justification::centred, 1);
}

void MainContentComponent::paintIfFileLoaded(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour(juce::Colours::darkgrey);
    g.fillRect(bounds);

    g.setColour(juce::Colours::grey);
    sourceFile.getThumbnail().drawChannels(g, bounds, 0.0,
                                           sourceFile.getThumbnail().getTotalLength(), 0.75f);
}
