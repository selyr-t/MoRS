#pragma once

#include <JuceHeader.h>
#include <random>
#include <vector>

#include "MorsDebug.h"
#include "MorsMorph.h"

// SEGMENT MAPPING - Output-to-source time mapping description
//------------------------------------------------------------------------------
struct SegmentMapping
{
    double startTime    = 0.0;  // Output time (seconds)
    double endTime      = 0.0;  // Output time (seconds), must be > startTime
    float  stretchRatio = 1.0f; // Time-scaling factor: outDur / srcDur
                                // ratio > 1 = stretch (slower)
                                // ratio < 1 = compress (faster)
};

// SEGMENT MAPPER - Piecewise source/output timeline conversion
//------------------------------------------------------------------------------
class SegmentMapper
{
public:
    // FIND SEGMENT INDEX - Find which segment contains the given output time
    static int findSegmentIndex(double outTime,
                                const std::vector<SegmentMapping>& segments) noexcept;

    // CALCULATE SOURCE POSITION - Map output time to source time by piecewise integration
    static double calculateSourcePosition(double outTime,
                                          const std::vector<SegmentMapping>& segments) noexcept;

    // UPDATE SEGMENTS - Thread-safe replacement of current stretch segments
    void updateSegments(const std::vector<SegmentMapping>& newSegments);

private:
    // SEGMENT MANAGEMENT - Shared stretch segment storage
    std::vector<SegmentMapping> segments; // Protected by spinlock
    juce::SpinLock segmentLock;           // Lightweight lock for segment updates
};

// TIME STRETCH - Abstract base class for time-stretch algorithms
//------------------------------------------------------------------------------
class TimeStretch
{
public:
    virtual ~TimeStretch() = default;

    // PREPARE - Initialize algorithm state for playback
    //------------------------------------------------------------------------------
    virtual void prepare(double sampleRate,
                         int maxBlockSize) = 0;

    // SEEK - Reset internal state after transport repositioning
    //------------------------------------------------------------------------------
    virtual void seek(double outputTimeSeconds) = 0;

    // PROCESS BLOCK - Render one output block from the input source
    //------------------------------------------------------------------------------
    virtual void processBlock(juce::AudioBuffer<float>& outputBuffer,
                              const juce::AudioBuffer<float>& inputBuffer,
                              double playheadSeconds,
                              const std::vector<SegmentMapping>& segments) = 0;
};

// PHASE VOCODER - FFT-based time stretching with phase propagation
//------------------------------------------------------------------------------
class PhaseVocoder : public TimeStretch
{
public:
    // CONSTRUCTOR - Initialize vocoder analysis/synthesis dimensions
    //------------------------------------------------------------------------------
    PhaseVocoder(int fftSize,
                 int hopSize);
    ~PhaseVocoder() = default;

    // PREPARE - Initialize vocoder runtime state and sample rate
    //------------------------------------------------------------------------------
    void prepare(double sampleRate,
                 int maxBlockSize) override;

    // SEEK - Reset overlap and phase state after scrubbing or repositioning
    //------------------------------------------------------------------------------
    void seek(double outputTimeSeconds) override;

    // PROCESS BLOCK - Render output for the current playhead position
    //------------------------------------------------------------------------------
    void processBlock(juce::AudioBuffer<float>& outputBuffer,
                      const juce::AudioBuffer<float>& inputBuffer,
                      double playheadSeconds,
                      const std::vector<SegmentMapping>& segments) override;
    
    // [PHASE VOCODER] Morph Debug Capture Enable
    //------------------------------------------------------------------------------
    void setMorphDebugCaptureEnabled(bool shouldEnable);

    // [PHASE VOCODER] Clear Morph Debug Frames
    //------------------------------------------------------------------------------
    void clearMorphDebugFrames();

    // [PHASE VOCODER] Get Morph Debug Frames
    //------------------------------------------------------------------------------
    const std::vector<MorsMorph::DebugFrame>& getMorphDebugFrames() const noexcept;

    // CROSS SYNTHESIS ENABLE - Toggle morph processing path
    //------------------------------------------------------------------------------
    void setCrossSynthesisEnabled(bool shouldEnable);
    
    void validateTargetEnvelope (int framesAnalyzed) const;
    
    // ANALYZE MORPH TARGET - Precompute target spectral envelopes for runtime lookup
    //------------------------------------------------------------------------------
    void analyzeMorphTarget(const juce::AudioBuffer<float>& targetBuffer,
                            double sampleRate);

private:

    // STFT ANALYSIS - Window frame, perform FFT, and extract magnitude/phase
    //------------------------------------------------------------------------------
    void analyzeFrame(const float* input,
                      std::vector<float>& magnitude,
                      std::vector<float>& phase);

    // STFT SYNTHESIS - Reconstruct complex spectrum and perform IFFT
    //------------------------------------------------------------------------------
    void synthesizeFrame(const std::vector<float>& magnitude,
                         const std::vector<float>& phase,
                         float* output);

    // PHASE PROCESSING - Advance synthesis phases for time-scaling
    //------------------------------------------------------------------------------
    void processPhases(float ratio,
                       int channel,
                       std::vector<float>& magnitude,
                       std::vector<float>& phase);

    // PHASE WRAPPING - Wrap phase to principal interval [-pi, pi]
    //------------------------------------------------------------------------------
    float wrapPhase(float phase) noexcept;
    
    void logInfo  (const juce::String& msg) const;
    void logWarn  (const juce::String& msg) const;
    void logError (const juce::String& msg) const;
    void logInfoRT  (const char* msg) const;
    void logWarnRT  (const char* msg) const;

    // CONFIGURATION - Core FFT and timing parameters
    //------------------------------------------------------------------------------
    int N;
    int Ha;
    int numBins;
    double sr = 44100.0;

    // FFT & WINDOWING - Analysis/synthesis transform helpers
    //------------------------------------------------------------------------------
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> fftData;
    std::vector<float> cachedWindow;

    // PHASE TRACKING STATE - Per-channel phase evolution and transient support
    //------------------------------------------------------------------------------
    std::vector<std::vector<float>> prevPhase;
    std::vector<std::vector<float>> sumPhase;
    std::vector<std::vector<float>> magLast;

    // FRAME PROCESSING BUFFERS - Reusable per-frame storage
    //------------------------------------------------------------------------------
    std::vector<float> frameIn;
    std::vector<float> frameOut;
    std::vector<float> magnitude;
    std::vector<float> phase;

    // MORPH STATE - Cross-synthesis analysis and runtime buffers
    //------------------------------------------------------------------------------
    MorsMorph morph;
    bool crossSynthesisEnabled = false;

    std::vector<float> sourceEnv;
    std::vector<float> targetEnv;
    std::vector<float> morphMagnitude;

    // SEGMENT MANAGEMENT - Runtime time-warp segment storage
    //------------------------------------------------------------------------------
    std::vector<SegmentMapping> segments;
    juce::SpinLock segmentLock;

    // PER-CHANNEL STATE - Overlap-add buffers and frame scheduling
    //------------------------------------------------------------------------------
    struct ChannelState
    {
        std::vector<float> olaBuffer;
        std::vector<float> olaWindow;
        double nextFrameOutTime = 0.0;
    };
    std::vector<ChannelState> channelStates;
    
    void overlapAddFrame (ChannelState& chState,
                          float* out,
                          int samplesRemaining);
    void shiftOlaBuffers (ChannelState& chState, int samplesRemaining);
    
    void ensureChannelState (int numCh, double playheadSeconds);
    void passthroughCopy (juce::AudioBuffer<float>&       outputBuffer,
                          const juce::AudioBuffer<float>& inputBuffer,
                          double                          playheadSeconds) const;
    void extractInterpolatedFrame (const float* src, int srcLen, double srcPos);
};

// SOLA - Time-domain synchronous overlap-add time stretching
//------------------------------------------------------------------------------
class Sola : public TimeStretch
{
public:
    // CONSTRUCTOR - Initialize SOLA window and hop settings
    //------------------------------------------------------------------------------
    Sola(int windowSize,
         int hopSize);
    ~Sola() = default;

    // PREPARE - Initialize SOLA runtime state
    //------------------------------------------------------------------------------
    void prepare(double sampleRate,
                 int maxBlockSize) override;

    // SEEK - Reset state after transport repositioning
    //------------------------------------------------------------------------------
    void seek(double outputTimeSeconds) override;

    // PROCESS BLOCK - Render one block using SOLA alignment
    //------------------------------------------------------------------------------
    void processBlock(juce::AudioBuffer<float>& outputBuffer,
                      const juce::AudioBuffer<float>& inputBuffer,
                      double playheadSeconds,
                      const std::vector<SegmentMapping>& segments) override;

    // CONFIGURE - Update SOLA analysis and synthesis dimensions
    //------------------------------------------------------------------------------
    void configure(int windowSize,
                   int hopSize);

private:
    // PER-CHANNEL STATE - Overlap-add buffers and frame timing
    //------------------------------------------------------------------------------
    struct ChannelState
    {
        std::vector<float> olaBuffer;   // Overlap-add buffer (N samples)
        std::vector<float> olaWindow;   // Accumulated overlap-add window weights
        double nextFrameOutTime = 0.0;  // Output time (seconds) of next frame start
    };
    std::vector<ChannelState> channelStates;

    // CONFIGURATION - SOLA runtime parameters
    //------------------------------------------------------------------------------
    float samplerate;
    int analysisWindowSize;
    int analysisHopSize;
    int synthesisWindowSize;

    // FFT & WINDOWING - Window generation helpers
    //------------------------------------------------------------------------------
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> cachedWindow; // Pre-computed Hann window

    // FRAME PROCESSING BUFFERS - Temporary frame storage
    //------------------------------------------------------------------------------
    std::vector<float> frame; // Input frame buffer

    // INTERPOLATION - Read fractional source positions into an output frame
    //------------------------------------------------------------------------------
    void interpolateRange(float* outFrame,
                          int outLength,
                          const float* inputData,
                          int inputLength,
                          double inputPos);

    // TILE - Overlap-add a synthesized frame into the output
    //------------------------------------------------------------------------------
    void tile(ChannelState& chState,
              const float* frame,
              float* outputPtr,
              int& outputPos,
              int outputLength,
              int synthesisWindowSize,
              int synthesisHopSize,
              const float* cachedWindow);

    // SHIFT - Advance overlap-add state for the current playhead
    //------------------------------------------------------------------------------
    void shift(ChannelState& chState,
               double playheadSeconds,
               int& outputPos,
               const int synthesisWindowSize,
               const int synthesisHopSize);
};

// GRANULAR - Grain-based time stretching and scattering
//------------------------------------------------------------------------------
class Granular : public TimeStretch
{
public:
    // CONSTRUCTOR - Initialize granular synthesis defaults
    //------------------------------------------------------------------------------
    Granular(int windowSize = 512,
             int hopSize = 256,
             double grainSizeMs = 50,
             int grainDensity = 128,
             double samplerate = 44100);

    ~Granular() = default;

    // PREPARE - Initialize granular runtime state
    //------------------------------------------------------------------------------
    void prepare(double sampleRate,
                 int maxBlockSize) override;

    // SEEK - Reset state after transport repositioning
    //------------------------------------------------------------------------------
    void seek(double outputTimeSeconds) override;

    // PROCESS BLOCK - Render one block using grain-based resynthesis
    //------------------------------------------------------------------------------
    void processBlock(juce::AudioBuffer<float>& outputBuffer,
                      const juce::AudioBuffer<float>& inputBuffer,
                      double playheadSeconds,
                      const std::vector<SegmentMapping>& segments) override;

    // CONFIGURE - Update grain engine configuration
    //------------------------------------------------------------------------------
    void configure(int samplerate,
                   double grainSizeMs,
                   int grainDensity);

    // SET GRAIN PARAMS - Update grain size, scatter, jitter, and density
    //------------------------------------------------------------------------------
    void setGrainParams(double sizeMs,
                        float scatterMs,
                        float ampJitter,
                        int density);

    // SET OVERLAP FACTOR - Adjust grain overlap density behavior
    //------------------------------------------------------------------------------
    void setOverlapFactor(float factor);

private:
    // PER-CHANNEL STATE - Overlap-add buffers and frame timing
    //------------------------------------------------------------------------------
    struct ChannelState
    {
        std::vector<float> olaBuffer;   // Overlap-add buffer (N samples)
        std::vector<float> olaWindow;   // Accumulated overlap-add window weights
        double nextFrameOutTime = 0.0;  // Output time (seconds) of next frame start
    };
    std::vector<ChannelState> channelStates;

    // FFT & WINDOWING - Window generation helpers
    //------------------------------------------------------------------------------
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> cachedWindow; // Pre-computed Hann window

    // GRAIN PARAMETERS - Grain synthesis control state
    //------------------------------------------------------------------------------
    std::vector<float> grain;
    int samplerate;
    int grainSizeSamples;
    int grainDensity;
    double grainSizeMs = 128;
    std::mt19937 rng { std::random_device{}() };
    float positionScatterMs = 250.0f; // Max random offset from source position, ms
    float amplitudeJitter = 0.5f;     // Max random gain reduction per grain, 0..1

    std::atomic<double> atomicGrainSizeMs { 125.0 };
    std::atomic<float> atomicScatterMs { 50.0f };
    std::atomic<float> atomicAmpJitter { 0.3f };
    std::atomic<int> atomicGrainDensity { 128 };
};
