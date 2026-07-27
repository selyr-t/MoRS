#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <functional>

#include "MorsBeat.h"
#include "MorsOnsetMapper.h"


class AudioFile : public juce::ChangeListener
{
public:
    //==========================================================================
    //  LIFECYCLE
    //==========================================================================
    AudioFile();
    ~AudioFile();
    
    //==========================================================================
    //  UTILITY
    //==========================================================================
    static juce::File path(const juce::String& folderName);
    using LoadCallback = std::function<void (const juce::File&,
                                             juce::AudioBuffer<float>&,
                                             double,
                                             std::unique_ptr<juce::AudioFormatReaderSource>)>;
    void open(LoadCallback callback);
    bool loadFromFile(const juce::File& file);   // load without dialog; mirrors open() internals
    bool isLoaded() const;
    std::function<void()> thumbnailChanged;
    void changeListenerCallback(juce::ChangeBroadcaster* source);
    bool setup(const juce::File& outputFile,
               int sampleRate,
               int numChannels);
    void record(const juce::AudioBuffer<float>& buffer);
    void close();
    
    //==========================================================================
    //  ANALYSIS
    //==========================================================================
    void analyze(juce::String method    = "custom",
                 double onsetThreshold  = 0.125,
                 double onsetSilence    = 0,
                 double onsetMinMs      = 50);
    
    //==========================================================================
    //  PROCESSING
    //==========================================================================
    void quantize(double bpm,
                  double subdivision,
                  float strength);

    void analyzeBands(float crossover1     = 500.0f,
                      float crossover2     = 2000.0f,
                      float crossover3     = 8000.0f,
                      double onsetThreshold = 0.125,
                      double onsetSilence   = 0,
                      double onsetMinMs     = 50);

    const std::vector<double>& getBandOnsetTimes(int band) const
    {
        return bandOnsetTimes[juce::jlimit(0, 3, band)];
    }
    
    //==========================================================================
    //  GETTERS
    //==========================================================================
    juce::AudioThumbnail&        getThumbnail()             { return audioThumbnail; }
    const juce::AudioBuffer<float>& getBuffer()     const   { return fileBuffer; }
    const std::vector<double>&   getOnsetTimes()    const   { return onsetTimes; }
    const std::vector<double>&   getGridTimes()     const   { return gridTimes; }
    double                       getSampleRate()    const   { return fileSamplerate; }
    double                       getLengthSeconds() const   { return fileLengthSeconds; }

private:
    //==========================================================================
    // Audio Data
    //==========================================================================
    juce::AudioBuffer<float>    fileBuffer;
    juce::AudioFormatManager    formatManager;
    juce::AudioThumbnailCache   thumbnailCache { 128 };
    juce::AudioThumbnail        audioThumbnail;

    //==========================================================================
    // Analysis Data
    //==========================================================================
    Onset                       onset;
    Tempo                       tempoDetection;
    std::vector<double>         onsetTimes;
    std::vector<double>         gridTimes;
    
    std::array<juce::dsp::LinkwitzRileyFilter<float>, 3> crossovers;
    std::array<std::vector<double>, 4> bandOnsetTimes;
    //==========================================================================
    // File Metadata
    //==========================================================================
    double fileSamplerate    = 0.0;
    double fileLengthSeconds = 0.0;

    //==========================================================================
    // File I/O
    //==========================================================================
    std::unique_ptr<juce::FileChooser>              fileChooser;
    std::unique_ptr<juce::AudioFormatReaderSource>  fileReader;

    //==========================================================================
    // File Writing
    //==========================================================================
    std::unique_ptr<juce::FileOutputStream>  fileStream;
    std::unique_ptr<juce::AudioFormatWriter> writer;
};
