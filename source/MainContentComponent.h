#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <vector>

#include "MorsFile.h"
#include "MorsBeat.h"
#include "MorsStretch.h"
#include "MorsPreset.h"
#include "MorsDebug.h"
#include "MorsLog.h"
#include "MorsDataExport.h"

class MainContentComponent  : public juce::AudioAppComponent,
                              public juce::ChangeListener,
                              public juce::Button::Listener,
                              public juce::Slider::Listener,
                              public juce::Timer
{
public:
    //==========================================================================
    //  PER-BAND SLOT
    //==========================================================================
    struct BandSlot
    {
        juce::File beatPath;
        juce::File morphPath;

        AudioFile beatFile;
        AudioFile morphFile;

        juce::AudioBuffer<float> sourceBandBuffer;
        std::vector<SegmentMapping> segments;
        std::vector<double> morphedOnsets;
        SegmentMapper segmentMapper;

        juce::Image spectrogramImage;

        juce::Label bandLabel   { {}, {} };
        juce::Label beatLabel   { {}, {} };
        juce::Label morphLabel  { {}, {} };

        juce::ComboBox beatSelector;
        juce::ComboBox morphSelector;

        juce::Rectangle<int> controlBounds;
        juce::Rectangle<int> visualBounds;

        std::unique_ptr<TimeStretch> stretcher;
    };

    //==========================================================================
    //  LIFECYCLE
    //==========================================================================
    MainContentComponent();
    ~MainContentComponent() override;

    //==========================================================================
    //  AUDIO APP COMPONENT
    //==========================================================================
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    //==========================================================================
    //  PRESET SYSTEM
    //==========================================================================
    PresetPanel                    presetPanel;
    std::unique_ptr<PresetManager> presetManager;

    MorsState captureState() const;
    void      applyState(const MorsState& s);

    //==========================================================================
    //  COMPONENT
    //==========================================================================
    void paint   (juce::Graphics& g) override;
    void resized () override;

    //==========================================================================
    //  LISTENERS / TIMER
    //==========================================================================
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void buttonClicked          (juce::Button* button) override;
    void sliderValueChanged     (juce::Slider* slider) override;
    void timerCallback          () override;

private:
    static constexpr int kMaxBands = 4;

    //==========================================================================
    //  APPLICATION MANAGEMENT
    //==========================================================================
    juce::AudioDeviceManager                            otherDeviceManager;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSettings;

    void menuAudioConfig(juce::Component* target);

    //==========================================================================
    //  STATE
    //==========================================================================
    enum AppState { IDLE, PLAYING, RECORDING };
    AppState state { IDLE };

    void changeState(AppState newState);

    //==========================================================================
    //  UI
    //==========================================================================

    // [UI] File — Buttons
    juce::TextButton sourceButtonOpen  { "Source"   };
    juce::TextButton morphButtonOpen   { "Morph"    };
    juce::TextButton beatRefButtonOpen { "Beat Ref" };

    std::vector<juce::TextButton*> fileButtons
    {
        &sourceButtonOpen,
        &morphButtonOpen,
        &beatRefButtonOpen
    };

    // [UI] System — Buttons
    juce::TextButton audioConfigButton { "Audio Configuration" };
    juce::TextButton analyzeButton     { "Analyze" };
    juce::TextButton exportDataButton  { "Export Data" };

    std::vector<juce::TextButton*> systemButtons
    {
        &audioConfigButton,
        &analyzeButton,
        &exportDataButton
    };

    // [UI] Algorithm — Selector
    juce::ComboBox algorithmSelector;

    // [UI] Transport — Buttons
    juce::TextButton playButton   { "Play"   };
    juce::TextButton stopButton   { "Stop"   };
    juce::TextButton recordButton { "Record" };
    juce::TextButton loopButton   { "Loop"   };

    std::vector<juce::TextButton*> transportButtons
    {
        &playButton,
        &stopButton,
        &recordButton,
        &loopButton
    };

    // [UI] Transport — Visuals
    juce::Slider scrubber;

    // [UI] Sample Library
    juce::TextButton chooseFolderButton { "Set Library" };
    juce::ComboBox   morphSampleSelector;

    // [UI] Quantization
    juce::ToggleButton quantizeOnsetsButton { "quantizeOnsetsButton" };

    juce::Slider quantizeStrengthSlider { "quantizeStrengthSlider" };
    juce::Label  quantizeOnsetsLabel    { "quantizeOnsetsLabel",   "Quantize" };
    juce::Label  quantizeStrengthLabel  { "quantizeStrengthLabel", "Strength" };

    // [UI] Onset Detection
    juce::Slider onsetThresholdSlider { "onsetThresholdSlider" };
    juce::Slider onsetSilenceSlider   { "onsetSilenceSlider"   };
    juce::Slider onsetMinMsSlider     { "onsetMinMsSlider"     };

    juce::Label  onsetThresholdLabel  { "onsetThresholdLabel", "Threshold" };
    juce::Label  onsetSilenceLabel    { "onsetSilenceLabel",   "Silence"   };
    juce::Label  onsetMinMsLabel      { "onsetMinMsLabel",     "Min MS"    };

    std::vector<juce::Slider*> onsetSliders
    {
        &onsetThresholdSlider,
        &onsetSilenceSlider,
        &onsetMinMsSlider
    };

    std::vector<juce::Label*> onsetLabels
    {
        &onsetThresholdLabel,
        &onsetSilenceLabel,
        &onsetMinMsLabel
    };

    // [UI] Morph / Interpolation
    // alpha (beat interpolation) is fixed at 1.0 — no slider
    // drawRange is fixed at 0.35 — no slider
    juce::Slider textureMorphSlider  { "textureMorphSlider"  };
    juce::Slider gaussWidthSlider    { "gaussWidthSlider"    };
    juce::Slider gaussGuardSlider    { "gaussGuardSlider"    };
    juce::Slider gaussDisplaceSlider { "gaussDisplaceSlider" };

    juce::Label  textureMorphLabel   { "textureMorphLabel",  "Texture Morph" };
    juce::Label  gaussWidthLabel     { "gaussWidthLabel",    "Beat Interp"   };
    juce::Label  gaussGuardLabel     { "gaussGuardLabel",    "Gauss Guard"   };
    juce::Label  gaussDisplaceLabel  { "gaussDisplaceLabel", "Displace"      };

    // [UI] Band Processing
    juce::ToggleButton analyzeBandsToggle { "Analyze Bands" };
    juce::ComboBox     bandCountSelector;

    // [UI] BPM & Subdivision
    juce::Label numberBoxBpm      { "numberBoxBpm",      "60"          };
    juce::Label numberLabelBpm    { "numberLabelBpm",    "BPM"         };
    juce::Label numberBoxSubdiv   { "numberBoxSubdiv",   "8"           };
    juce::Label numberLabelSubdiv { "numberLabelSubdiv", "Subdivision" };

    std::vector<juce::Label*> numberBoxes
    {
        &numberBoxBpm,
        &numberBoxSubdiv
    };

    std::vector<juce::Label*> numberLabels
    {
        &numberLabelBpm,
        &numberLabelSubdiv
    };

    // [UI] Granular
    juce::Slider grainSizeSlider    { "grainSizeSlider"    };
    juce::Slider grainDensitySlider { "grainDensitySlider" };
    juce::Slider grainScatterSlider { "grainScatterSlider" };

    juce::Label  grainSizeLabel     { "grainSizeLabel",    "Size"       };
    juce::Label  grainDensityLabel  { "grainDensityLabel", "Density"    };
    juce::Label  grainScatterLabel  { "grainScatterLabel", "Scatter MS" };

    std::vector<juce::Slider*> grainSliders
    {
        &grainSizeSlider,
        &grainDensitySlider,
        &grainScatterSlider
    };

    std::vector<juce::Label*> grainLabels
    {
        &grainSizeLabel,
        &grainDensityLabel,
        &grainScatterLabel
    };

    //==========================================================================
    //  FILE I/O
    //==========================================================================
    AudioFile  sourceFile;   // parent source
    AudioFile  morphFile;    // parent/global morph
    AudioFile  beatRefFile;  // parent/global beat ref
    AudioFile  fileWriter;

    juce::File loadedSourceFile;   // path of last successfully loaded source

    juce::File        sampleLibraryFolder;
    juce::StringArray sampleLibraryPaths;
    juce::Array<juce::File> sampleLibraryFiles;

    juce::File fileRecord;

    std::unique_ptr<juce::FileChooser>             chooser;
    juce::AudioFormatManager                       formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    void onAlgorithmChanged();
    void openSourceFile();
    void openMorphFile();
    void openBeatRefFile();
    bool loadSourceAudio(juce::File& file);
    bool loadMorphAudio (juce::File& file);
    bool loadBeatRefAudio(juce::File& file);
    void recordFile();
    void scanSampleLibrary();

    //==========================================================================
    //  THUMBNAIL / DRAWING
    //==========================================================================
    void thumbnailChanged();
    void paintIfNoFileLoaded(juce::Graphics& g, juce::Rectangle<int>& bounds);
    void paintIfFileLoaded  (juce::Graphics& g, juce::Rectangle<int> bounds);
    
    int getVisibleBandVisualizerCount() const;
    void paintBandVisualizer(juce::Graphics& g, BandSlot& band, juce::Rectangle<int> area, int bandIndex);
    void drawBufferWaveform(juce::Graphics& g, const juce::AudioBuffer<float>& buffer, juce::Rectangle<int> bounds);
    juce::Image buildSpectrogramFromBuffer(const juce::AudioBuffer<float>& buf, double sampleRate);
    void buildSpectrogramImage();
    void buildBandSpectrogramImage(int bandIndex);

    std::vector<double> morphedOnsets;

    //==========================================================================
    //  ANALYSIS
    //==========================================================================
    Onset onsetSource;
    Onset onsetBeatRef;

    Tempo tempoSource;
    Tempo tempoBeatRef;
    
    void drawOnsetLines(juce::Graphics& g,
                        const std::vector<double>& onsets,
                        double totalLengthSeconds,
                        juce::Rectangle<int> bounds,
                        juce::Colour colour,
                        float thickness = 1.0f);
    
    void drawGaussianSpread(juce::Graphics& g,
                            const std::vector<double>& onsets,
                            double totalLengthSeconds,
                            juce::Rectangle<int> bounds,
                            float sigmaScale,
                            float sigmaEps,
                            float displacementBoost = 0.0f,
                            float drawRange         = 0.5f);

    void drawMorphedWaveOverlay(juce::Graphics& g,
                                juce::Rectangle<int> bounds,
                                juce::AudioThumbnail& thumbnail,
                                double totalLengthSeconds,
                                const std::vector<double>& srcOnsets,
                                const std::vector<double>& morphedOnsets,
                                juce::Colour colour);
    
    void analyzeSourceFile();
    void analyzeMorphFile();
    void analyzeBeatRefFile();
    std::vector<double> quantizeOnsetsToGrid(const std::vector<double>& onsets,
                                             const std::vector<double>& grid,
                                             float strength);
    void rebuildMappingsFromUi();
    void reanalyze();

    //==========================================================================
    //  PROCESSING
    //==========================================================================
    SegmentMapper               segmantMapper;
    std::vector<SegmentMapping> segments;

    bool isBandAnalysisEnabled() const;
    void onBandCountChanged();
    void syncGranularParamsToAllStretchers();
    void rebuildSegmentsForAlpha(float alpha);

    TimeStretch* currentStretchAlgo = nullptr;

    PhaseVocoder phaseVocoder;
    Sola         sola;
    Granular     granular { 512, 50, 2, 44100 };

    // Band configuration
    int   activeBandCount = 1;
    float crossoverHz[3]  = { 250, 1000.0f, 4000.0f };

    std::array<BandSlot, kMaxBands> bandSlots;

    void rebuildBandSegments(float alpha);
    void buildSourceBandBuffers();
    void initializeBandUi();
    void refreshBandUiVisibility();
    void populateBandSelectors();
    void loadBandBeatFile(int bandIndex);
    void loadBandMorphFile(int bandIndex);

    // Morph file spectrogram
    juce::Image spectrogramImage;

    // Playback state
    std::atomic<bool>   isPlayingRT     { false };
    std::atomic<double> playheadSeconds { 0.0 };
    
    std::atomic<float> lastAppliedGaussWidth    { -1.0f };
    std::atomic<float> lastAppliedGaussDisplace { -1.0f };
    std::atomic<float> lastAppliedGaussGuard    { -1.0f };
    std::atomic<bool>  mappingDirty             { false };
    
    int    lastBlockSize  = 0;
    double lastSampleRate = 0.0;

    juce::dsp::Limiter<float> outputLimiter;
    //==========================================================================
    //  PARAMS
    //==========================================================================
    std::atomic<float> gain         { 1.0f };
    std::atomic<float> pvMakeupGain { 2.0f };

    int magnitudePrintCounter = 0;
    int sliderDebounceCounter = 0;

    //==========================================================================
    //  DATA EXPORT
    //==========================================================================
    // MORPH DEBUG - Enable / clear / access captured morph frames
    //------------------------------------------------------------------------------
    void setMorphDebugCaptureEnabled(bool shouldEnable);
    void clearMorphDebugFrames();
    const std::vector<MorsMorph::DebugFrame>& getMorphDebugFrames() const noexcept;
    
    bool exportNextAnalysis = false;   // set true when Export Data button is clicked
    void exportAnalysisData();         // writes all CSVs for the current session
    int  perfBlockCounter   = 0;       // row index for performance.csv

    static constexpr int DEBOUNCE_FRAMES = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainContentComponent)
};
