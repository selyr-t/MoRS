#pragma once

#include <JuceHeader.h>
#include "MorsConstants.h"
#include "MorsDebug.h"

class MorsMorph
{
public:
    static constexpr int kFFTOrder     = 10;
    static constexpr int kFFTSize      = 1 << kFFTOrder;
    static constexpr int kNumBins      = kFFTSize / 2 + 1;
    static constexpr int kHopSize      = 512;
    static constexpr int kLifterCutoff = 40;

    //==========================================================================
    // Debug frame export payload
    //==========================================================================
    struct DebugFrame
    {
        int frameIndex = -1;
        float normalizedPos = 0.0f;

        std::vector<float> sourceMag;
        std::vector<float> sourcePhase;
        std::vector<float> sourceEnvelope;

        std::vector<float> targetEnvelopeInterpolated;
        std::vector<float> targetEnvelopeNormalized;

        std::vector<float> sourceStructure;
        std::vector<float> morphedMagnitude;
    };

    MorsMorph();
    ~MorsMorph() = default;

    void analyzeTargetFile(const juce::AudioBuffer<float>& targetBuffer,
                           double sampleRate);

    void analyzeFrame(const float* frameBuf,
                      float* magBuf,
                      float* phaseBuf);

    void computeCepstralEnvelope(const float* magBuf,
                                 int lifterCutoff,
                                 float* envOut);

    void alignEnvelope(const std::vector<std::vector<float>>& targetEnvelopeFrames,
                       int sourceNumFrames,
                       std::vector<std::vector<float>>& alignedOut);

    void normalizeEnvelope(const float* magBuf,
                           float* envFrame);
    
    /** Rescale envFrame so that its bin-sum equals that of referenceEnv.
        Used to make a foreign envelope occupy the energy budget of the
        source envelope it is replacing. */
    void matchEnvelopeSum (const float* referenceEnv, float* envFrame);
    
    void getTargetEnvelopeAtFrame(float normalizedPos,
                                  float* envOut) const;

    int getTargetNumFrames() const;
    
    void buildMorphFrame(int frameIndex,
                         float normalizedPos,
                         const float* sourceMag,
                         const float* sourcePhase,
                         float* morphedMag);
    
    //==========================================================================
    // NEW — debug capture control
    //==========================================================================
    void setDebugCaptureEnabled(bool shouldCapture);
    void clearDebugFrames();
    const std::vector<DebugFrame>& getDebugFrames() const noexcept;

    //==========================================================================
    // NEW — build one debug frame for comparison with Python
    // This mirrors the classical cepstral cross-synthesis pipeline:
    // source magnitude / source envelope * target envelope
    //==========================================================================
    void captureMorphDebugFrame(int frameIndex,
                                float normalizedPos,
                                const float* sourceMag,
                                const float* sourcePhase,
                                const float* sourceEnvelope,
                                const float* targetEnvelopeInterpolated,
                                const float* targetEnvelopeNormalized,
                                const float* sourceStructure,
                                const float* morphedMagnitude);

private:
    std::vector<std::vector<float>> targetEnvelopeFrames;
    int targetNumFrames = 0;

    juce::dsp::FFT fft { kFFTOrder };
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> cachedWindow;

    std::vector<float> fftData;
    std::vector<float> fullSpec;

    std::vector<float> cepBuf;
    std::vector<float> cepWorkSpec;

    std::vector<float> cepLogFull;
    std::vector<float> cepLogFullBeforeLifter;
    std::vector<juce::dsp::Complex<float>> cepComplexIn;
    std::vector<juce::dsp::Complex<float>> cepComplexOut;
    
    // Pre-allocated buffers for buildMorphFrame (avoid RT heap allocation)
    //------------------------------------------------------------------------------
    std::vector<float> sourceEnvBuf;
    std::vector<float> targetEnvInterpBuf;
    std::vector<float> targetEnvNormBuf;
    std::vector<float> sourceStructureBuf;
    //==========================================================================
    // NEW — debug capture state
    //==========================================================================
    bool debugCaptureEnabled = false;
    std::vector<DebugFrame> debugFrames;
};
