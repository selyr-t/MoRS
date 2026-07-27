#pragma once

#include <JuceHeader.h>
#include <aubio/aubio.h>
#include <vector>
#include <cstdint>
#include <cmath>
#include <numeric>
#include <algorithm>
#include "MorsDebug.h"

//==============================================================================
// Custom Onset Method
//==============================================================================
class Onset
{
public:
    Onset(int N = 2048, int H = 128);
    ~Onset();

    void reset();
    void prepare();

    /**
     * @brief Computes an energy-based novelty function from the audio buffer.
     *
     * Slides a window across the signal, computes the log-compressed energy
     * of each frame, then takes the half-wave rectified first difference
     * to produce a novelty curve that peaks at transient events.
     *
     * Based on: Notebook C6/C6S1_NoveltyEnergy.ipynb
     *
     * @param data      Input audio signal (mono, floating point)
     * @param Fs        Sampling rate in Hz
     * @param N         Window size in samples                          (default: 2048)
     * @param H         Hop size in samples                             (default: 128)
     * @param gamma     Logarithmic compression factor — higher values
     *                  compress loud frames more aggressively          (default: 10.0f)
     * @param norm      If true, normalise output to a max value of 1.0 (default: true)
     *
     * @return Energy-based novelty function, one float per hop frame.
     *         Feature rate = Fs / H (samples per second of novelty)
     */
    std::pair<std::vector<float>, double>  novelty(const std::vector<float>& data,
                                                   double Fs    = 44100.0,
                                                   int    N     = 2048,
                                                   int    H     = 128,
                                                   float  gamma = 10.0f,
                                                   bool   norm  = true);
    
    /**
     * @brief Detects peaks in a novelty function using the Boeck peak-picking method.
     *
     * Implements the peak-picking method described in:
     * "Evaluating the Online Capabilities of Onset Detection Methods"
     * Sebastian Boeck, Florian Krebs and Markus Schedl
     * Proceedings of the 13th International Society for Music Information
     * Retrieval Conference (ISMIR), 2012
     *
     * Modified by Jan Schlueter, 2014-04-24
     *
     * @param activations   Vector of activations to process (novelty function)
     * @param fsFeature     Frame rate of onset activation function in Hz
     * @param threshold     Threshold for peak-picking                          (default: 0.5)
     * @param scores        Include activation value for each returned peak     (default: false)
     * @param minMsGate     only report one onset per N milliseconds            (default: true)
     * @param minMsValue    Minimum amount of time before next onset detection  (default: 25ms)
     * @param preAvg        Past milliseconds used for moving average           (default: 250ms)
     * @param postAvg       Future milliseconds used for moving average         (default: 125ms)
     * @param preMax        Past milliseconds used for moving maximum           (default: 125ms)
     * @param postMax       Future milliseconds used for moving maximum         (default: 125ms)
     *
     * @return Peak positions as timestamps in seconds
     */
    std::vector<double> peak(std::vector<float>  activations,
                            double  fsFeature,
                            float   threshold  = 0.0125f,
                            bool    scores     = false,
                            bool    minMsGate  = true,
                            double  minMsValue = 10,
                            float   pre_avg    = 50,   // ms
                            float   post_avg   = 50,   // ms
                            float   pre_max    = 150,   // ms
                            float   post_max   = 150);  // ms
    
    
    
    
    
    const std::vector<float>& getNoveltyEnergy() const { return noveltyEnergy; }
    double getFsFeature() const { return fsFeature; }

private:
    int winSize;
    int hopSize;

    juce::dsp::WindowingFunction<float> window;
    std::vector<float>                  cachedWindow;
    std::vector<float>                  noveltyEnergy;
    double                              fsFeature = 0.0;
    
    std::vector<double> onsetTimes;

    // Helper Functions for `peak`
    std::vector<float> slidingMax(const std::vector<float>& x,
                                  int windowSize,
                                  int origin);
    std::vector<float> slidingAvg(const std::vector<float>& x,
                                  int windowSize,
                                  int origin);
};

// Using AUBIO
//==============================================================================
class OnsetAubio
{
public:
    OnsetAubio();
    ~OnsetAubio();

    void reset();

    void prepare(double sampleRate,
                 int    winSize      = 1024,
                 int    hopSize      = 512,
                 float  onsetThresh  = 0.125f,
                 float  onsetSilence = -120.0f,
                 float  onsetMinMs   = 64.0f);

    void processBlock(const float* samples, int numSamples);

    const std::vector<double>& getOnsetTimes() const { return onsetTimes; }

private:
    aubio_onset_t* onset     = nullptr;
    fvec_t*        aubioInput  = nullptr;
    fvec_t*        aubioOutput = nullptr;

    int           hopSize    = 0;
    int           writeIndex = 0;
    double        sampleRate = 0.0;
    std::uint64_t totalSamplesProcessed = 0;

    std::vector<double> onsetTimes;
};

// Tempo — Autocorrelation Tempogram
//==============================================================================
class Tempo
{
public:
    Tempo();
    ~Tempo();

    void prepare(double sampleRate,
                 int    hopSize = 512,
                 int    minBpm  = 20,
                 int    maxBpm  = 240);

    void reset();
    void processBlock(const float* samples, int numSamples);
    float getEstimatedBpm() const;

private:
    std::vector<float> computeNovelty   (const float* samples, int numSamples);
    std::vector<float> downsample       (const std::vector<float>& signal, int factor);
    std::vector<float> autocorrelate    (const std::vector<float>& segment);

    double sampleRate       = 0.0;
    int    hopSize          = 512;
    int    minBpm           = 20;
    int    maxBpm           = 240;
    int    downsampleFactor = 1;
    double fsFeature        = 0.0;
    int    winLength        = 0;

    std::vector<float> estimatedBpms;
};
