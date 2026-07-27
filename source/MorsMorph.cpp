#include "MorsMorph.h"

#include <limits>
#include <cmath>

constexpr const char* kCat = "MORPH";

// [MORS MORPH] Local Debug Helpers
//------------------------------------------------------------------------------
// Compact logging helpers for buffer inspection and stage-by-stage comparison.
namespace
{
    void logScalar(const juce::String& label, float value)
    {
        LOGD(kCat, label + " = " + juce::String(value, 6));
    }

    void logInt(const juce::String& label, int value)
    {
        LOGD(kCat, label + " = " + juce::String(value));
    }

    void logBufferStats(const juce::String& label,
                        const float* data,
                        int size,
                        int previewCount = 6)
    {
        if (data == nullptr || size <= 0)
        {
            LOGD(kCat, label + " [empty]");
            return;
        }

        float minVal = std::numeric_limits<float>::infinity();
        float maxVal = -std::numeric_limits<float>::infinity();
        float sum = 0.0f;
        float sumSq = 0.0f;
        int finiteCount = 0;

        juce::String preview;
        const int count = juce::jmin(size, previewCount);

        for (int i = 0; i < size; ++i)
        {
            const float v = data[i];

            if (std::isfinite(v))
            {
                minVal = std::min(minVal, v);
                maxVal = std::max(maxVal, v);
                sum += v;
                sumSq += v * v;
                ++finiteCount;
            }

            if (i < count)
                preview += juce::String(v, 6) + " ";
        }

        const float mean = (finiteCount > 0) ? (sum / (float) finiteCount) : 0.0f;
        const float rms  = (finiteCount > 0) ? std::sqrt(sumSq / (float) finiteCount) : 0.0f;

        LOGD(kCat,
             label
             + " size="   + juce::String(size)
             + " finite=" + juce::String(finiteCount)
             + " min="    + juce::String(minVal, 6)
             + " max="    + juce::String(maxVal, 6)
             + " mean="   + juce::String(mean, 6)
             + " rms="    + juce::String(rms, 6)
             + " preview=[" + preview.trimEnd() + "]");
    }

    void logComplexPackedBins(const juce::String& label,
                              const float* packed,
                              int numBins,
                              int previewBins = 4)
    {
        if (packed == nullptr || numBins <= 0)
        {
            LOGD(kCat, label + " [empty]");
            return;
        }

        juce::String msg = label + " bins=" + juce::String(numBins) + " preview=";
        const int count = juce::jmin(numBins, previewBins);

        for (int bin = 0; bin < count; ++bin)
        {
            const float re = packed[2 * bin];
            const float im = packed[2 * bin + 1];

            msg += "["
                + juce::String(bin)
                + ": "
                + juce::String(re, 6)
                + ", "
                + juce::String(im, 6)
                + "] ";
        }

        LOGD(kCat, msg.trimEnd());
    }

    void logDiffStats(const juce::String& label,
                      const float* a,
                      const float* b,
                      int size,
                      int previewCount = 6)
    {
        if (a == nullptr || b == nullptr || size <= 0)
        {
            LOGD(kCat, label + " [empty]");
            return;
        }

        float maxAbsDiff = 0.0f;
        float meanAbsDiff = 0.0f;
        juce::String preview;

        const int count = juce::jmin(size, previewCount);

        for (int i = 0; i < size; ++i)
        {
            const float d = std::abs(a[i] - b[i]);
            maxAbsDiff = std::max(maxAbsDiff, d);
            meanAbsDiff += d;

            if (i < count)
                preview += juce::String(d, 6) + " ";
        }

        meanAbsDiff /= (float) size;

        LOGD(kCat,
             label
             + " maxAbsDiff=" + juce::String(maxAbsDiff, 6)
             + " meanAbsDiff=" + juce::String(meanAbsDiff, 6)
             + " preview=[" + preview.trimEnd() + "]");
    }

    void logStageBanner(const juce::String& label)
    {
        LOGD(kCat, "================ " + label + " ================");
    }
}


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
// Initialize FFT workspace, precompute the Hann window, and allocate
// persistent buffers for buildMorphFrame.
MorsMorph::MorsMorph()
    : window(kFFTSize,
             juce::dsp::WindowingFunction<float>::hann,
             true)
{
    fftData.resize(2 * kFFTSize, 0.0f);
    fullSpec.resize(2 * kFFTSize, 0.0f);
    cachedWindow.resize(kFFTSize, 1.0f);

    cepBuf.resize((size_t) kFFTSize, 0.0f);
    cepWorkSpec.resize((size_t) (2 * kFFTSize), 0.0f);

    cepLogFull.resize((size_t) kFFTSize, 0.0f);
    cepLogFullBeforeLifter.resize((size_t) kFFTSize, 0.0f);
    cepComplexIn.resize((size_t) kFFTSize);
    cepComplexOut.resize((size_t) kFFTSize);

    // Pre-allocated buffers for buildMorphFrame (zero RT heap allocation)
    sourceEnvBuf       .resize ((size_t) kNumBins, 0.0f);
    targetEnvInterpBuf .resize ((size_t) kNumBins, 0.0f);
    targetEnvNormBuf   .resize ((size_t) kNumBins, 0.0f);
    sourceStructureBuf .resize ((size_t) kNumBins, 0.0f);

    window.multiplyWithWindowingTable(cachedWindow.data(), kFFTSize);

    logInt("Ctor.kFFTSize", kFFTSize);
    logInt("Ctor.kHopSize", kHopSize);
    logInt("Ctor.kNumBins", kNumBins);
    logInt("Ctor.kLifterCutoff", kLifterCutoff);
    logBufferStats("Ctor.cachedWindow", cachedWindow.data(), kFFTSize);
}


//------------------------------------------------------------------------------
// Analyze Target File
//------------------------------------------------------------------------------
// Precompute cepstral envelope frames from the target buffer
void MorsMorph::analyzeTargetFile(const juce::AudioBuffer<float>& targetBuffer,
                                  double sampleRate)
{
    LOGD(kCat, "analyzeTargetFile.begin");
    logScalar("analyzeTargetFile.sampleRate", (float) sampleRate);

    const int numSamples  = targetBuffer.getNumSamples();
    const int numChannels = targetBuffer.getNumChannels();

    logInt("analyzeTargetFile.numSamples", numSamples);
    logInt("analyzeTargetFile.numChannels", numChannels);

    if (numSamples <= 0 || numChannels <= 0)
    {
        LOGD(kCat, "analyzeTargetFile.early_exit_invalid_buffer");
        return;
    }

    std::vector<float> mono((size_t) numSamples, 0.0f);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = targetBuffer.getReadPointer(ch);
        logBufferStats("analyzeTargetFile.input_channel_" + juce::String(ch),
                       src,
                       juce::jmin(numSamples, 1024));

        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += src[i];
    }

    logBufferStats("analyzeTargetFile.mono_sum_before_avg",
                   mono.data(),
                   juce::jmin(numSamples, 1024));

    if (numChannels > 1)
    {
        const float scale = 1.0f / (float) numChannels;
        logScalar("analyzeTargetFile.mono_scale", scale);

        for (auto& s : mono)
            s *= scale;
    }

    logBufferStats("analyzeTargetFile.mono_after_avg",
                   mono.data(),
                   juce::jmin(numSamples, 1024));

    targetNumFrames = (numSamples - kFFTSize) / kHopSize + 1;
    logInt("analyzeTargetFile.targetNumFrames", targetNumFrames);

    if (targetNumFrames <= 0)
    {
        targetEnvelopeFrames.clear();
        targetNumFrames = 0;
        LOGD(kCat, "analyzeTargetFile.early_exit_no_frames");
        return;
    }

    targetEnvelopeFrames.assign((size_t) targetNumFrames,
                                std::vector<float>((size_t) kNumBins, 0.0f));

    std::vector<float> frameBuf((size_t) kFFTSize, 0.0f);
    std::vector<float> magBuf((size_t) kNumBins, 0.0f);
    std::vector<float> phaseBuf((size_t) kNumBins, 0.0f);
    std::vector<float> envBuf((size_t) kNumBins, 0.0f);

    for (int frame = 0; frame < targetNumFrames; ++frame)
    {
        const int startSample = frame * kHopSize;
        logInt("analyzeTargetFile.frame", frame);
        logInt("analyzeTargetFile.startSample", startSample);

        for (int i = 0; i < kFFTSize; ++i)
        {
            const int idx = startSample + i;
            frameBuf[(size_t) i] = (idx < numSamples)
                                 ? mono[(size_t) idx] * cachedWindow[(size_t) i]
                                 : 0.0f;
        }

        logBufferStats("analyzeTargetFile.frameBuf_windowed", frameBuf.data(), kFFTSize);

        analyzeFrame(frameBuf.data(), magBuf.data(), phaseBuf.data());
        logBufferStats("analyzeTargetFile.magBuf_after_analyzeFrame", magBuf.data(), kNumBins);
        logBufferStats("analyzeTargetFile.phaseBuf_after_analyzeFrame", phaseBuf.data(), kNumBins);

        computeCepstralEnvelope(magBuf.data(), kLifterCutoff, envBuf.data());
        logBufferStats("analyzeTargetFile.envBuf_after_computeCepstralEnvelope", envBuf.data(), kNumBins);

        targetEnvelopeFrames[(size_t) frame] = envBuf;
    }

    LOGD(kCat, "analyzeTargetFile.end");
}


//------------------------------------------------------------------------------
// Analyze Frame
//------------------------------------------------------------------------------
// Perform FFT analysis and extract magnitude and phase
void MorsMorph::analyzeFrame(const float* frameBuf,
                             float* magBuf,
                             float* phaseBuf)
{
    LOGD(kCat, "analyzeFrame.begin");
    logBufferStats("analyzeFrame.frameBuf_input", frameBuf, kFFTSize);

    std::fill(fftData.begin(), fftData.end(), 0.0f);
    logBufferStats("analyzeFrame.fftData_after_clear", fftData.data(), (int) fftData.size());

    for (int i = 0; i < kFFTSize; ++i)
        fftData[(size_t) i] = frameBuf[i];

    logBufferStats("analyzeFrame.fftData_after_pack_real", fftData.data(), (int) fftData.size());

    fft.performRealOnlyForwardTransform(fftData.data());
    logBufferStats("analyzeFrame.fftData_after_rfft", fftData.data(), (int) fftData.size());
    logComplexPackedBins("analyzeFrame.fft_bins_after_rfft", fftData.data(), kNumBins);

    for (int bin = 0; bin < kNumBins; ++bin)
    {
        const float re = fftData[(size_t) (2 * bin)];
        const float im = fftData[(size_t) (2 * bin + 1)];

        magBuf[bin]   = std::sqrt(re * re + im * im);
        phaseBuf[bin] = std::atan2(im, re);
    }

    logBufferStats("analyzeFrame.magBuf_output", magBuf, kNumBins);
    logBufferStats("analyzeFrame.phaseBuf_output", phaseBuf, kNumBins);
    LOGD(kCat, "analyzeFrame.end");
}


//------------------------------------------------------------------------------
// Compute Cepstral Envelope
//------------------------------------------------------------------------------
// Estimate smooth spectral envelope via cepstral liftering:
//   log(mag) -> IFFT -> lifter -> FFT -> exp
void MorsMorph::computeCepstralEnvelope(const float* magBuf,
                                        int lifterCutoff,
                                        float* envOut)
{
    std::fill(cepBuf.begin(), cepBuf.end(), 0.0f);
    std::fill(cepLogFull.begin(), cepLogFull.end(), 0.0f);

    for (auto& c : cepComplexIn)  c = { 0.0f, 0.0f };
    for (auto& c : cepComplexOut) c = { 0.0f, 0.0f };

    // Build mirrored full real log-spectrum
    cepLogFull[0] = std::log(std::max(magBuf[0], MorsConstants::kEps));

    for (int k = 1; k < kNumBins - 1; ++k)
        cepLogFull[(size_t) k] = std::log(std::max(magBuf[k], MorsConstants::kEps));

    cepLogFull[(size_t) (kNumBins - 1)] =
        std::log(std::max(magBuf[kNumBins - 1], MorsConstants::kEps));

    for (int k = 1; k < kNumBins - 1; ++k)
        cepLogFull[(size_t) (kFFTSize - k)] = cepLogFull[(size_t) k];

    // Full complex IFFT to cepstral domain
    for (int n = 0; n < kFFTSize; ++n)
        cepComplexIn[(size_t) n] = { cepLogFull[(size_t) n], 0.0f };

    fft.perform(cepComplexIn.data(), cepComplexOut.data(), true);

    for (int n = 0; n < kFFTSize; ++n)
        cepBuf[(size_t) n] = cepComplexOut[(size_t) n].real();

    // Lifter: zero out high-quefrency coefficients
    const int cutoff = juce::jlimit(1, kFFTSize / 2, lifterCutoff);
    for (int i = cutoff + 1; i < kFFTSize - cutoff; ++i)
        cepBuf[(size_t) i] = 0.0f;

    // Forward FFT back to smooth log-spectrum
    for (int n = 0; n < kFFTSize; ++n)
        cepComplexIn[(size_t) n] = { cepBuf[(size_t) n], 0.0f };

    fft.perform(cepComplexIn.data(), cepComplexOut.data(), false);

    for (int n = 0; n < kFFTSize; ++n)
        cepLogFull[(size_t) n] = cepComplexOut[(size_t) n].real();

    // Exponentiate to get the linear-amplitude envelope
    for (int bin = 0; bin < kNumBins; ++bin)
        envOut[(size_t) bin] = std::max(std::exp(cepLogFull[(size_t) bin]),
                                        MorsConstants::kEps);
}


//------------------------------------------------------------------------------
// Normalize Envelope (legacy — matches sum to raw magnitude)
//------------------------------------------------------------------------------
// Kept for backward compatibility. For cross-synthesis, use matchEnvelopeSum
// with the source *envelope* as the reference instead.
void MorsMorph::normalizeEnvelope(const float* magBuf,
                                  float* envFrame)
{
    float refSum = 0.0f;
    float envSum = 0.0f;

    for (int k = 0; k < kNumBins; ++k)
    {
        refSum += magBuf[k];
        envSum += envFrame[k];
    }

    envSum = std::max(envSum, MorsConstants::kEps);
    const float scale = refSum / envSum;

    for (int k = 0; k < kNumBins; ++k)
        envFrame[k] *= scale;
}


//------------------------------------------------------------------------------
// Match Envelope Sum
//------------------------------------------------------------------------------
// Rescale envFrame so that its bin-sum equals that of referenceEnv.
// Used to make a foreign (target) envelope occupy the energy budget of the
// source envelope it is replacing — not the raw magnitude. This prevents the
// +6 to +14 dB gain overshoot that occurs when normalizing against sourceMag.
void MorsMorph::matchEnvelopeSum (const float* referenceEnv,
                                  float* envFrame)
{
    float refSum = 0.0f;
    float envSum = 0.0f;

    for (int k = 0; k < kNumBins; ++k)
    {
        refSum += referenceEnv[k];
        envSum += envFrame[k];
    }

    envSum = std::max (envSum, MorsConstants::kEps);
    const float scale = refSum / envSum;

    for (int k = 0; k < kNumBins; ++k)
        envFrame[k] *= scale;
}


//------------------------------------------------------------------------------
// Get Target Envelope At Frame
//------------------------------------------------------------------------------
// Interpolate target envelopes using normalized position in [0, 1]
void MorsMorph::getTargetEnvelopeAtFrame(float normalizedPos,
                                         float* envOut) const
{
    if (targetEnvelopeFrames.empty() || targetNumFrames <= 0)
    {
        std::fill(envOut, envOut + kNumBins, 0.0f);
        return;
    }

    normalizedPos = juce::jlimit(0.0f, 1.0f, normalizedPos);

    const float targetPos = normalizedPos * (float) (targetNumFrames - 1);
    const int   frame0    = (int) std::floor(targetPos);
    const int   frame1    = std::min(frame0 + 1, targetNumFrames - 1);
    const float frac      = targetPos - (float) frame0;

    for (int k = 0; k < kNumBins; ++k)
    {
        envOut[k] = targetEnvelopeFrames[(size_t) frame0][(size_t) k] * (1.0f - frac)
                  + targetEnvelopeFrames[(size_t) frame1][(size_t) k] * frac;
    }
}


//------------------------------------------------------------------------------
// Get Target Num Frames
//------------------------------------------------------------------------------
int MorsMorph::getTargetNumFrames() const
{
    return targetNumFrames;
}


//------------------------------------------------------------------------------
// Build Morph Frame
//------------------------------------------------------------------------------
// Build one morphed magnitude frame via cepstral cross-synthesis.
//
// Pipeline:
//   1. Compute source cepstral envelope
//   2. Interpolate target envelope at current position
//   3. Normalize target envelope to match source *envelope* sum (not magnitude)
//   4. Whiten source by dividing out its envelope (with relative floor)
//   5. Recolour with the normalized target envelope
//   6. Energy-preserving post-scale to guarantee no net broadband gain
//
// All scratch buffers are pre-allocated class members — zero heap allocation.
void MorsMorph::buildMorphFrame (int          frameIndex,
                                 float        normalizedPos,
                                 const float* sourceMag,
                                 const float* sourcePhase,
                                 float*       morphedMag)
{
    // Clear pre-allocated working buffers
    std::fill (sourceEnvBuf.begin(),       sourceEnvBuf.end(),       0.0f);
    std::fill (targetEnvInterpBuf.begin(), targetEnvInterpBuf.end(), 0.0f);
    std::fill (targetEnvNormBuf.begin(),   targetEnvNormBuf.end(),   0.0f);
    std::fill (sourceStructureBuf.begin(), sourceStructureBuf.end(), 0.0f);

    // 1. Compute source cepstral envelope
    computeCepstralEnvelope (sourceMag, kLifterCutoff, sourceEnvBuf.data());

    // 2. Interpolate target envelope at current playback position
    getTargetEnvelopeAtFrame (normalizedPos, targetEnvInterpBuf.data());

    // 3. Normalize target envelope to match source ENVELOPE sum (not magnitude).
    //    This is the critical fix: using sourceMag here caused +6 to +14 dB gain.
    std::copy (targetEnvInterpBuf.begin(), targetEnvInterpBuf.end(),
               targetEnvNormBuf.begin());
    matchEnvelopeSum (sourceEnvBuf.data(), targetEnvNormBuf.data());

    // 4 + 5. Whiten source, recolour with target.
    //    Floor relative to envelope peak so deep spectral notches can't produce
    //    astronomical structure values that crackle through into the output.
    float envPeak = 0.0f;
    for (int k = 0; k < kNumBins; ++k)
        envPeak = std::max (envPeak, sourceEnvBuf[(size_t) k]);

    const float envFloor = std::max (envPeak * 1.0e-3f, MorsConstants::kEps);

    for (int k = 0; k < kNumBins; ++k)
    {
        const float safeEnv = std::max (sourceEnvBuf[(size_t) k], envFloor);
        sourceStructureBuf[(size_t) k] = sourceMag[k] / safeEnv;
        morphedMag[k] = sourceStructureBuf[(size_t) k] * targetEnvNormBuf[(size_t) k];
    }

    // 6. Energy-preserving post-scale: force sum(morphedMag^2) == sum(sourceMag^2).
    //    Double precision avoids float32 noise on quiet frames.
    double srcEnergy = 0.0;
    double morEnergy = 0.0;

    for (int k = 0; k < kNumBins; ++k)
    {
        srcEnergy += (double) sourceMag[k]  * sourceMag[k];
        morEnergy += (double) morphedMag[k] * morphedMag[k];
    }

    if (morEnergy > 1.0e-20)
    {
        const float postScale = (float) std::sqrt (srcEnergy / morEnergy);
        for (int k = 0; k < kNumBins; ++k)
            morphedMag[k] *= postScale;
    }

    // Optional: capture full debug snapshot for thesis CSV export
    if (debugCaptureEnabled)
    {
        captureMorphDebugFrame (frameIndex, normalizedPos,
                                sourceMag, sourcePhase,
                                sourceEnvBuf.data(),
                                targetEnvInterpBuf.data(),
                                targetEnvNormBuf.data(),
                                sourceStructureBuf.data(),
                                morphedMag);
    }
}


//------------------------------------------------------------------------------
// Debug Capture Control
//------------------------------------------------------------------------------
void MorsMorph::setDebugCaptureEnabled (bool shouldCapture)
{
    debugCaptureEnabled = shouldCapture;

    if (shouldCapture)
        LOGD (kCat, "Debug frame capture ENABLED");
    else
        LOGD (kCat, "Debug frame capture DISABLED");
}

void MorsMorph::clearDebugFrames()
{
    debugFrames.clear();
}

const std::vector<MorsMorph::DebugFrame>& MorsMorph::getDebugFrames() const noexcept
{
    return debugFrames;
}


//------------------------------------------------------------------------------
// Capture Morph Debug Frame
//------------------------------------------------------------------------------
// Snapshot every intermediate buffer for one morph frame so that
// MorsDataExport can write them to CSV for comparison with Python.
// Only stores data when debugCaptureEnabled is true.
void MorsMorph::captureMorphDebugFrame (int          frameIndex,
                                        float        normalizedPos,
                                        const float* sourceMag,
                                        const float* sourcePhase,
                                        const float* sourceEnvelope,
                                        const float* targetEnvelopeInterpolated,
                                        const float* targetEnvelopeNormalized,
                                        const float* sourceStructure,
                                        const float* morphedMagnitude)
{
    DebugFrame f;
    f.frameIndex    = frameIndex;
    f.normalizedPos = normalizedPos;

    const auto n = (size_t) kNumBins;

    f.sourceMag                  .assign (sourceMag,                   sourceMag + n);
    f.sourcePhase                .assign (sourcePhase,                 sourcePhase + n);
    f.sourceEnvelope             .assign (sourceEnvelope,              sourceEnvelope + n);
    f.targetEnvelopeInterpolated .assign (targetEnvelopeInterpolated,  targetEnvelopeInterpolated + n);
    f.targetEnvelopeNormalized   .assign (targetEnvelopeNormalized,    targetEnvelopeNormalized + n);
    f.sourceStructure            .assign (sourceStructure,             sourceStructure + n);
    f.morphedMagnitude           .assign (morphedMagnitude,            morphedMagnitude + n);

    debugFrames.push_back (std::move (f));
}
