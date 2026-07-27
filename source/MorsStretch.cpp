#include "MorsStretch.h"
#include "MorsMorph.h"
#include "MorsDebug.h"
#include <algorithm>
#include <cmath>

//==============================================================================
// HELPER FUNCTIONS
//==============================================================================
namespace { constexpr const char* kCat = "PHASE_VOCODER"; }

// Safe Division
//------------------------------------------------------------------------------
// Divide a/b with a guard against near-zero denominators
static inline float safeDiv(float a, float b, float fallback = 0.0f) noexcept
{
    return (std::abs(b) > 1.0e-12f) ? (a / b) : fallback;
}


//==============================================================================
// SEGMENT MAPPER
//==============================================================================

// [SEGMENT MAPPER] Update Segments
//------------------------------------------------------------------------------
// Receives a new list of output-domain segments describing piecewise time-stretching
void SegmentMapper::updateSegments(const std::vector<SegmentMapping>& newSegments)
{
    constexpr const char* kCat = "SEGMENT_MAPPER";
    LOG_SCOPE(kCat);
    PERF_START(segmentmapper_update);

    LOGI(kCat, "updateSegments(): begin"
               " | inputSegmentCount = " + juce::String((int) newSegments.size()));

    // Protect shared segment vector (UI thread writes, audio thread reads)
    juce::SpinLock::ScopedLockType lock(segmentLock);
    segments = newSegments;

    // Sort by start time so lookup behavior is deterministic
    std::sort(segments.begin(),
              segments.end(),
              [](const SegmentMapping& a, const SegmentMapping& b)
              {  return a.startTime < b.startTime;  });

    int degenerateCount = 0;
    int nonFiniteCount = 0;

    // Validate/clamp each segment to avoid degenerate cases
    for (auto& s : segments)
    {
        if (!std::isfinite(s.startTime) || !std::isfinite(s.endTime) || !std::isfinite(s.stretchRatio))
        {
            ++nonFiniteCount;
            LOGW(kCat, "updateSegments(): non-finite segment field detected"
                       " | start = " + juce::String(s.startTime, 6)
                       + " | end = " + juce::String(s.endTime, 6)
                       + " | stretch = " + juce::String(s.stretchRatio, 6));
        }

        // Error Check: ensure non-zero positive duration
        if (s.endTime <= s.startTime)
        {
            ++degenerateCount;
            LOGW(kCat, "updateSegments(): degenerate segment duration fixed"
                       " | start = " + juce::String(s.startTime, 6)
                       + " | end(before) = " + juce::String(s.endTime, 6));
            s.endTime = s.startTime + 1.0e-6;
        }

        // Clamp stretch ratio to meaningful extreme scaling boundaries
        s.stretchRatio = juce::jlimit(0.01f, 100.0f, s.stretchRatio);
    }

    if (!segments.empty())
    {
        const auto& first = segments.front();
        const auto& last  = segments.back();

        LOGI(kCat, "updateSegments(): completed"
                   " | segmentCount = " + juce::String((int) segments.size())
                   + " | degenerateFixed = " + juce::String(degenerateCount)
                   + " | nonFiniteDetected = " + juce::String(nonFiniteCount)
                   + " | firstStart = " + juce::String(first.startTime, 6)
                   + " | lastEnd = " + juce::String(last.endTime, 6));
    }
    else
    {
        LOGW(kCat, "updateSegments(): no segments stored after update");
    }

    PERF_END(segmentmapper_update);
}

// [SEGMENT MAPPER] Find Segment Index
//------------------------------------------------------------------------------
// Returns the index of the segment containing outTime (seconds) in the OUTPUT timeline.
int SegmentMapper::findSegmentIndex(double outTime,
                                    const std::vector<SegmentMapping>& segments) noexcept
{
    if (segments.empty())
        return -1;

    // Linear search: acceptable for small segment counts
    for (int i = 0; i < (int)segments.size(); ++i)
    {
        const auto& s = segments[(size_t)i];
        if (outTime >= s.startTime && outTime < s.endTime)
            return i;
    }

    // Clamp outside-range queries to closest segment
    if (outTime >= segments.back().endTime)
        return (int)segments.size() - 1;
    if (outTime < segments.front().startTime)
        return 0;

    // Gap between segments
    return -1;
}

// [SEGMENT MAPPER] Calculate Source Position
//------------------------------------------------------------------------------
// Accumulates source time by integrating 1/r over the output timeline.
double SegmentMapper::calculateSourcePosition(double outTime,
                                              const std::vector<SegmentMapping>& segments) noexcept
{
    if (segments.empty())
        return outTime;

    // Anchor source time at the start of the first segment
    double srcTime = segments.front().startTime;

    for (const auto& seg : segments)
    {
        const double outStart = seg.startTime;
        const double outEnd   = seg.endTime;
        const double outDur   = outEnd - outStart;
        const double srcDur   = safeDiv((float)outDur,
                                        seg.stretchRatio,
                                        (float)outDur);

        if (outTime >= outEnd)
        {
            srcTime += srcDur;
        }
        else if (outTime >= outStart)
        {
            const double outOff = outTime - outStart;
            srcTime += safeDiv((float)outOff, seg.stretchRatio, (float)outOff);
            return srcTime;
        }
        else
        {
            return srcTime;
        }
    }

    return srcTime;
}


//==============================================================================
// Phase Vocoder
//==============================================================================

// Log helpers — route all vocoder logging through a single category
void PhaseVocoder::logInfo   (const juce::String& msg) const { LOGI    (kCat, msg); }
void PhaseVocoder::logWarn   (const juce::String& msg) const { LOGW    (kCat, msg); }
void PhaseVocoder::logError  (const juce::String& msg) const { LOGE    (kCat, msg); }
void PhaseVocoder::logInfoRT (const char* msg)         const { LOGI_RT (kCat, msg); }
void PhaseVocoder::logWarnRT (const char* msg)         const { LOGW_RT (kCat, msg); }


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
// Initialize phase vocoder buffers, FFT, and window state
PhaseVocoder::PhaseVocoder (int fftSize, int hopSize)
    : N       (fftSize),
      Ha      (std::min(fftSize/4, hopSize)),
      numBins (fftSize / 2 + 1),
      fft     (static_cast<int> (std::log2 (fftSize))),
      window  (fftSize, juce::dsp::WindowingFunction<float>::hann, true)
{
    logInfo ("PhaseVocoder(): begin"
             " | fftSize = " + juce::String (fftSize)
             + " | hopSize = " + juce::String (hopSize)
             + " | numBins = " + juce::String (numBins));

    // FFT scratch (interleaved real/imag, length 2N)
    fftData.resize ((size_t) (2 * N), 0.0f);

    // Per-frame I/O
    frameIn .resize ((size_t) N, 0.0f);
    frameOut.resize ((size_t) N, 0.0f);

    // Spectral data (half-spectrum + DC)
    magnitude.resize ((size_t) numBins, 0.0f);
    phase    .resize ((size_t) numBins, 0.0f);

    // Cross-synthesis envelopes
    sourceEnv     .assign ((size_t) numBins, 0.0f);
    targetEnv     .assign ((size_t) numBins, 0.0f);
    morphMagnitude.assign ((size_t) numBins, 0.0f);

    // Precomputed Hann window
    cachedWindow.resize ((size_t) N, 1.0f);
    window.multiplyWithWindowingTable (cachedWindow.data(), N);
}


//------------------------------------------------------------------------------
// Morph Debug Forwarding
//------------------------------------------------------------------------------
void PhaseVocoder::setMorphDebugCaptureEnabled (bool shouldEnable)
{
    morph.setDebugCaptureEnabled (shouldEnable);
}

void PhaseVocoder::clearMorphDebugFrames()
{
    morph.clearDebugFrames();
}

const std::vector<MorsMorph::DebugFrame>& PhaseVocoder::getMorphDebugFrames() const noexcept
{
    return morph.getDebugFrames();
}


//------------------------------------------------------------------------------
// Cross Synthesis Enable
//------------------------------------------------------------------------------
// Toggle morph processing path for runtime testing
void PhaseVocoder::setCrossSynthesisEnabled (bool shouldEnable)
{
    const bool prev = crossSynthesisEnabled;
    crossSynthesisEnabled = shouldEnable;

    logStateChange (kCat, "setCrossSynthesisEnabled",
                    "crossSynthesis", (int) prev, (int) crossSynthesisEnabled,
                    "targetFrames = " + juce::String (morph.getTargetNumFrames()));
}


//------------------------------------------------------------------------------
// Validate Target Envelope
//------------------------------------------------------------------------------
// Probe frame 0 of the target envelope to catch silent or corrupt analysis
// results early. Fires once per analyzeTargetFile() call.
void PhaseVocoder::validateTargetEnvelope (int framesAnalyzed) const
{
    std::vector<float> probe (MorsMorph::kNumBins, 0.0f);
    morph.getTargetEnvelopeAtFrame (0.0f, probe.data());

    float probeMin = probe[0];
    float probeMax = probe[0];
    float probeSum = 0.0f;
    int   probeNaN = 0;

    for (int k = 0; k < MorsMorph::kNumBins; ++k)
    {
        const float v = probe[static_cast<size_t> (k)];
        probeMin  = std::min (probeMin, v);
        probeMax  = std::max (probeMax, v);
        probeSum += v;
        if (! std::isfinite (v))
            ++probeNaN;
    }

    const float probeMean = probeSum / static_cast<float> (MorsMorph::kNumBins);

    logInfo ("analyzeMorphTarget(): success"
             " | framesAnalyzed = " + juce::String (framesAnalyzed)
             + " | frame[0] env min = " + juce::String (probeMin,  6)
             + " | max = "              + juce::String (probeMax,  6)
             + " | mean = "             + juce::String (probeMean, 6)
             + " | nanCount = "         + juce::String (probeNaN));

    if (probeNaN > 0)
        logError ("analyzeMorphTarget(): NaN/Inf in target envelope frame 0"
                  " — cepstral chain likely broken");

    if (probeMax < 1.0e-10f)
        logWarn ("analyzeMorphTarget(): target envelope frame 0 near-zero"
                 " — target audio may be silent");
}


//------------------------------------------------------------------------------
// Analyze Morph Target
//------------------------------------------------------------------------------
// Precompute target spectral envelopes for runtime cross-synthesis lookup.
void PhaseVocoder::analyzeMorphTarget (const juce::AudioBuffer<float>& targetBuffer,
                                       double sampleRate)
{
    LOG_SCOPE (kCat);
    PERF_START (pv_analyzeMorphTarget);

    logInfo ("analyzeMorphTarget(): begin"
             " | numSamples = "    + juce::String (targetBuffer.getNumSamples())
             + " | numChannels = " + juce::String (targetBuffer.getNumChannels())
             + " | sampleRate = "  + juce::String (sampleRate, 2));

    morph.analyzeTargetFile (targetBuffer, sampleRate);

    const int framesAnalyzed = morph.getTargetNumFrames();

    if (framesAnalyzed <= 0)
    {
        logError ("analyzeMorphTarget(): FAILED — 0 frames analyzed"
                  " | numSamples = "    + juce::String (targetBuffer.getNumSamples())
                  + " | required min = " + juce::String (MorsMorph::kFFTSize));
    }
    else
    {
        validateTargetEnvelope (framesAnalyzed);
    }

    PERF_END (pv_analyzeMorphTarget);
}


//------------------------------------------------------------------------------
// Prepare
//------------------------------------------------------------------------------
void PhaseVocoder::prepare (double sampleRate, int maxBlockSize)
{
    LOG_SCOPE (kCat);

    sr = (sampleRate > 0.0) ? sampleRate : 44100.0;

    logInfo ("prepare(): configured"
             " | sampleRateIn = "    + juce::String (sampleRate)
             + " | sampleRateUsed = " + juce::String (sr)
             + " | maxBlockSize = "   + juce::String (maxBlockSize)
             + " | fftSize = "        + juce::String (N)
             + " | hopSize = "        + juce::String (Ha));
}


//------------------------------------------------------------------------------
// Seek
//------------------------------------------------------------------------------
// Reset streaming state so the next block starts cleanly at outputTimeSeconds
void PhaseVocoder::seek (double outputTimeSeconds)
{
    LOG_SCOPE (kCat);

    logInfo ("seek(): resetting state"
             " | outputTimeSeconds = " + juce::String (outputTimeSeconds, 6)
             + " | channelStates = "   + juce::String ((int) channelStates.size()));

    for (auto& chState : channelStates)
    {
        std::fill (chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
        std::fill (chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
        chState.nextFrameOutTime = outputTimeSeconds;
    }

    for (auto& v : prevPhase) std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : sumPhase)  std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : magLast)   std::fill (v.begin(), v.end(), 0.0f);
}


//------------------------------------------------------------------------------
// Analyze Frame
//------------------------------------------------------------------------------
// Window, FFT, and extract magnitude/phase from one input frame
void PhaseVocoder::analyzeFrame (const float*        input,
                                 std::vector<float>& mag,
                                 std::vector<float>& ph)
{
    std::fill (fftData.begin(), fftData.end(), 0.0f);

    for (int i = 0; i < N; ++i)
        fftData[(size_t) i] = input[i];

    fft.performRealOnlyForwardTransform (fftData.data());

    for (int bin = 0; bin < numBins; ++bin)
    {
        const float re = fftData[(size_t) (2 * bin)];
        const float im = fftData[(size_t) (2 * bin + 1)];

        mag[(size_t) bin] = std::sqrt (re * re + im * im);
        ph [(size_t) bin] = std::atan2 (im, re);
    }
}


//------------------------------------------------------------------------------
// Synthesize Frame
//------------------------------------------------------------------------------
// Rebuild complex spectrum from magnitude/phase and inverse-FFT
void PhaseVocoder::synthesizeFrame (const std::vector<float>& mag,
                                    const std::vector<float>& ph,
                                    float* output)
{
    std::fill (fftData.begin(), fftData.end(), 0.0f);

    for (int bin = 0; bin < numBins; ++bin)
    {
        fftData[(size_t) (2 * bin)]     = mag[(size_t) bin] * std::cos (ph[(size_t) bin]);
        fftData[(size_t) (2 * bin + 1)] = mag[(size_t) bin] * std::sin (ph[(size_t) bin]);
    }

    fft.performRealOnlyInverseTransform (fftData.data());

    for (int i = 0; i < N; ++i)
        output[i] = fftData[(size_t) i];
}


//------------------------------------------------------------------------------
// Process Phases
//------------------------------------------------------------------------------
// Advance synthesis phases for time-scaling with transient-aware resets
void PhaseVocoder::processPhases (float               ratio,
                                  int                 channel,
                                  std::vector<float>& mag,
                                  std::vector<float>& ph)
{
    const float ratioIn = ratio;
    ratio = juce::jlimit (0.0001f, 1000.0f, ratio);

    if (ratio != ratioIn)
    {
        char m[128];
        std::snprintf (m, sizeof (m),
                       "RATIO_CLAMP: in=%f out=%f ch=%d",
                       ratioIn, ratio, channel);
        logWarnRT (m);
    }

    constexpr float transientThreshold = 2.5f;

    const float pi    = juce::MathConstants<float>::pi;
    const float HsOut = (float) Ha;
    const float HaIn  = (float) Ha / ratio;

    auto& prevPh = prevPhase[(size_t) channel];
    auto& sumPh  = sumPhase [(size_t) channel];
    auto& magL   = magLast  [(size_t) channel];

    for (int bin = 0; bin < numBins; ++bin)
    {
        const float omega     = 2.0f * pi * (float) bin / (float) N;
        const float phaseNow  = ph    [(size_t) bin];
        const float phasePrev = prevPh[(size_t) bin];

        const float expected = omega * HaIn;

        float delta = phaseNow - phasePrev - expected;
        delta = wrapPhase (delta);

        const float trueFreq = omega + delta / HaIn;

        sumPh [(size_t) bin] += trueFreq * HsOut;
        prevPh[(size_t) bin]  = phaseNow;

        const float magPrev = magL[(size_t) bin];
        const float jump    = safeDiv (mag[(size_t) bin], magPrev, 1.0f);

        if (jump > transientThreshold)
            sumPh[(size_t) bin] = phaseNow;

        magL[(size_t) bin] = mag[(size_t) bin];
        ph  [(size_t) bin] = sumPh[(size_t) bin];
    }
}


//==============================================================================
// processBlock helpers
//==============================================================================

//------------------------------------------------------------------------------
// Ensure Channel State
//------------------------------------------------------------------------------
// Lazily (re)allocates per-channel streaming state when channel count changes.
void PhaseVocoder::ensureChannelState (int numCh, double playheadSeconds)
{
    if ((int) channelStates.size() == numCh)
        return;

    channelStates.clear();
    channelStates.resize ((size_t) numCh);

    prevPhase.assign ((size_t) numCh, std::vector<float> ((size_t) numBins, 0.0f));
    sumPhase .assign ((size_t) numCh, std::vector<float> ((size_t) numBins, 0.0f));
    magLast  .assign ((size_t) numCh, std::vector<float> ((size_t) numBins, 0.0f));

    for (auto& chState : channelStates)
    {
        chState.olaBuffer.assign ((size_t) N, 0.0f);
        chState.olaWindow.assign ((size_t) N, 0.0f);
        chState.nextFrameOutTime = playheadSeconds;
    }
}


//------------------------------------------------------------------------------
// Passthrough Copy
//------------------------------------------------------------------------------
// Direct copy used when no segment map is supplied.
void PhaseVocoder::passthroughCopy (juce::AudioBuffer<float>&       outputBuffer,
                                    const juce::AudioBuffer<float>& inputBuffer,
                                    double                          playheadSeconds) const
{
    const int numCh      = outputBuffer.getNumChannels();
    const int numSamples = outputBuffer.getNumSamples();
    const int srcLen     = inputBuffer.getNumSamples();
    const int srcStart   = (int) std::floor (playheadSeconds * sr);

    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* src = inputBuffer.getReadPointer (std::min (ch, inputBuffer.getNumChannels() - 1));
        float*       dst = outputBuffer.getWritePointer (ch);

        for (int n = 0; n < numSamples; ++n)
        {
            const int idx = juce::jlimit (0, srcLen - 1, srcStart + n);
            dst[n] = src[idx];
        }
    }
}


//------------------------------------------------------------------------------
// Extract Interpolated Frame
//------------------------------------------------------------------------------
// Fills frameIn with an N-sample windowed slice of src, linearly interpolated
// around the fractional source position srcPos.
void PhaseVocoder::extractInterpolatedFrame (const float* src, int srcLen, double srcPos)
{
    for (int i = 0; i < N; ++i)
    {
        const double p    = srcPos + (double) i;
        const int    i0   = (int) std::floor (p);
        const int    i1   = i0 + 1;
        const float  frac = (float) (p - (double) i0);

        if (i0 < 0 || i0 >= srcLen)
        {
            frameIn[(size_t) i] = 0.0f;
        }
        else
        {
            const float s0 = src[i0];
            const float s1 = (i1 >= 0 && i1 < srcLen) ? src[i1] : s0;
            frameIn[(size_t) i] = s0 + frac * (s1 - s0);
        }
    }

    // Apply cached Hann window
    for (int i = 0; i < N; ++i)
        frameIn[(size_t) i] *= cachedWindow[(size_t) i];
}


//------------------------------------------------------------------------------
// Overlap-Add Frame
//------------------------------------------------------------------------------
// Accumulate one synthesized frame into OLA buffers, emit normalized output,
// and shift the accumulators forward.
void PhaseVocoder::overlapAddFrame (ChannelState& chState,
                                    float*        out,
                                    int           samplesRemaining)
{
    // Accumulate windowed frame and window-squared envelope
    for (int i = 0; i < N; ++i)
    {
        chState.olaBuffer[(size_t) i] += frameOut[(size_t) i] * cachedWindow[(size_t) i];
        chState.olaWindow[(size_t) i] += cachedWindow[(size_t) i] * cachedWindow[(size_t) i];
    }

    // Normalize by running window sum and emit
    for (int n = 0; n < samplesRemaining; ++n)
    {
        const float w = chState.olaWindow[(size_t) n];
        out[n] = (w > 1.0e-4f) ? (chState.olaBuffer[(size_t) n] / w) : 0.0f;
    }

    // Shift buffers left by samplesRemaining
    shiftOlaBuffers (chState, samplesRemaining);
}


//------------------------------------------------------------------------------
// Shift OLA Buffers
//------------------------------------------------------------------------------
// Slide overlap-add buffer and window accumulator left, zero-fill the tail.
void PhaseVocoder::shiftOlaBuffers (ChannelState& chState, int samplesRemaining)
{
    const int tail = (int) chState.olaBuffer.size() - samplesRemaining;

    if (tail > 0)
    {
        std::memmove (chState.olaBuffer.data(),
                      chState.olaBuffer.data() + samplesRemaining,
                      (size_t) (N - samplesRemaining) * sizeof (float));
        std::memmove (chState.olaWindow.data(),
                      chState.olaWindow.data() + samplesRemaining,
                      (size_t) (N - samplesRemaining) * sizeof (float));

        std::fill (chState.olaBuffer.begin() + (N - samplesRemaining), chState.olaBuffer.end(), 0.0f);
        std::fill (chState.olaWindow.begin() + (N - samplesRemaining), chState.olaWindow.end(), 0.0f);
    }
    else
    {
        std::fill (chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
        std::fill (chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
    }
}


//------------------------------------------------------------------------------
// Process Block
//------------------------------------------------------------------------------
// Render one output block: analysis → phase propagation → optional morph →
// synthesis → overlap-add.
void PhaseVocoder::processBlock (juce::AudioBuffer<float>&          outputBuffer,
                                 const juce::AudioBuffer<float>&    inputBuffer,
                                 double                             playheadSeconds,
                                 const std::vector<SegmentMapping>& segments)
{
    const int numCh      = outputBuffer.getNumChannels();
    const int numSamples = outputBuffer.getNumSamples();

    //--------------------------------------------------------------------------
    // Safety checks
    //--------------------------------------------------------------------------
    if (numCh <= 0 || numSamples <= 0)
    {
        logWarnRT ("BAD_OUTPUT_STATE: numCh<=0 or numSamples<=0");
        return;
    }

    if (inputBuffer.getNumSamples() <= 0 || inputBuffer.getNumChannels() <= 0)
    {
        logWarnRT ("INPUT_EMPTY: clearing output");
        outputBuffer.clear();
        return;
    }

    ensureChannelState (numCh, playheadSeconds);

    //--------------------------------------------------------------------------
    // No segment map → bypass the vocoder entirely
    //--------------------------------------------------------------------------
    if (segments.empty())
    {
        passthroughCopy (outputBuffer, inputBuffer, playheadSeconds);
        return;
    }

    //--------------------------------------------------------------------------
    // Main per-channel streaming loop
    //--------------------------------------------------------------------------
    outputBuffer.clear();

    const int srcNumCh = inputBuffer.getNumChannels();
    const int srcLen   = inputBuffer.getNumSamples();

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto&  chState = channelStates[(size_t) ch];
        float* out     = outputBuffer.getWritePointer (ch);

        int outPos = 0;

        while (outPos < numSamples)
        {
            const int samplesRemaining = std::min (Ha, numSamples - outPos);

            //------------------------------------------------------------------
            // Keep the per-channel time cursor aligned with the host
            //------------------------------------------------------------------
            if (chState.nextFrameOutTime < playheadSeconds)
                chState.nextFrameOutTime = playheadSeconds;

            const double expectedTime = playheadSeconds + (double) outPos / sr;

            if (chState.nextFrameOutTime > expectedTime + 1.0e-9)
                seek (expectedTime);

            const double frameOutTime = expectedTime;

            //------------------------------------------------------------------
            // Segment lookup → stretch ratio and source position
            //------------------------------------------------------------------
            const int   segIdx = SegmentMapper::findSegmentIndex (frameOutTime, segments);
            const float ratio  = (segIdx >= 0) ? segments[(size_t) segIdx].stretchRatio : 1.0f;

            const double srcTime = SegmentMapper::calculateSourcePosition (frameOutTime, segments);
            const double srcPos  = srcTime * sr;

            if (! std::isfinite (srcTime) || ! std::isfinite (srcPos))
                break;

            const float* src = inputBuffer.getReadPointer (std::min (ch, srcNumCh - 1));

            //------------------------------------------------------------------
            // Extract windowed frame → analyze → phase propagation
            //------------------------------------------------------------------
            extractInterpolatedFrame (src, srcLen, srcPos);
            analyzeFrame  (frameIn.data(), magnitude, phase);
            processPhases (ratio, ch, magnitude, phase);

            //------------------------------------------------------------------
            // One-shot diagnostic: first time we reach the cross-synthesis gate
            //------------------------------------------------------------------
            if (ch == 0 && outPos == 0)
            {
                static std::atomic<bool> sFirstCheck { false };
                if (! sFirstCheck.exchange (true, std::memory_order_relaxed))
                {
                    char m[256];
                    std::snprintf (m, sizeof (m),
                                   "CROSS_SYNTH_CHECK: enabled=%d targetFrames=%d srcLen=%d",
                                   (int) crossSynthesisEnabled,
                                   morph.getTargetNumFrames(),
                                   srcLen);
                    logInfoRT (m);
                }
            }

            //------------------------------------------------------------------
            // Cross-synthesis path (morph) vs. vanilla resynthesis
            //------------------------------------------------------------------
            if (crossSynthesisEnabled && morph.getTargetNumFrames() > 0)
            {
                // One-shot "we entered the morph branch" log
                static std::atomic<bool> sFirstEntry { false };
                if (! sFirstEntry.exchange (true, std::memory_order_relaxed))
                {
                    char m[256];
                    std::snprintf (m, sizeof (m),
                                   "CROSS_SYNTH_ENTERED: ch=%d srcPos=%.3f srcLen=%d targetFrames=%d",
                                   ch, (float) srcPos, srcLen, morph.getTargetNumFrames());
                    logInfoRT (m);
                }

                const float normalizedPos = juce::jlimit (
                    0.0f, 1.0f,
                    (float) (srcPos / (double) juce::jmax (1, srcLen - 1)));

                const int frameIndex = juce::jmax (
                    0, (int) std::floor (srcPos / (double) Ha));

                morph.buildMorphFrame (frameIndex, normalizedPos,
                                       magnitude.data(), phase.data(),
                                       morphMagnitude.data());

                synthesizeFrame (morphMagnitude, phase, frameOut.data());

                // NOTE: per-frame OLA / frameOut peak+RMS diagnostics removed;
                // see git history if needed.
            }
            else
            {
                synthesizeFrame (magnitude, phase, frameOut.data());
            }

            //------------------------------------------------------------------
            // Overlap-add, emit, advance
            //------------------------------------------------------------------
            overlapAddFrame (chState, out + outPos, samplesRemaining);

            outPos += samplesRemaining;
            chState.nextFrameOutTime = playheadSeconds + (double) outPos / sr;
        }
    }
}


//------------------------------------------------------------------------------
// Wrap Phase
//------------------------------------------------------------------------------
// Constrains phase to principal range [-π, π].
float PhaseVocoder::wrapPhase (float phase) noexcept
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr float pi    = juce::MathConstants<float>::pi;

    phase = std::fmod (phase + pi, twoPi);
    if (phase < 0.0f)
        phase += twoPi;
    return phase - pi;
}


//==============================================================================
// SOLA
//==============================================================================

// [SOLA] Constructor
//------------------------------------------------------------------------------
Sola::Sola(int windowSize, int hopSize)
    :   analysisWindowSize  (windowSize),
        analysisHopSize     (hopSize),
        synthesisWindowSize (analysisWindowSize),
        window              (synthesisWindowSize,
                             juce::dsp::WindowingFunction<float>::hann,
                             true)
{
    constexpr const char* kCat = "SOLA";
    LOG_SCOPE(kCat);
    PERF_START(sola_ctor);

    frame.resize((size_t)synthesisWindowSize, 0.0f);

    cachedWindow.resize((size_t)synthesisWindowSize, 1.0f);
    window.multiplyWithWindowingTable(cachedWindow.data(), synthesisWindowSize);

    LOGI(kCat, "Sola(): initialized"
               " | analysisWindowSize = " + juce::String(analysisWindowSize)
               + " | analysisHopSize = " + juce::String(analysisHopSize)
               + " | synthesisWindowSize = " + juce::String(synthesisWindowSize));

    PERF_END(sola_ctor);
}

// [SOLA] Prepare
//------------------------------------------------------------------------------
void Sola::prepare(double samplerate, int maxBlockSize)
{
    constexpr const char* kCat = "SOLA";
    LOG_SCOPE(kCat);

    this->samplerate = (samplerate > 0.0) ? samplerate : 44100.0;
    synthesisWindowSize = analysisWindowSize;
    frame.assign((size_t)synthesisWindowSize, 0.0f);

    cachedWindow.assign((size_t)synthesisWindowSize, 1.0f);
    window.fillWindowingTables(cachedWindow.data(),
                               (size_t)synthesisWindowSize,
                               juce::dsp::WindowingFunction<float>::hann,
                               true);
    window.multiplyWithWindowingTable(cachedWindow.data(), synthesisWindowSize);

    LOGI(kCat, "prepare(): configured"
               " | sampleRateIn = " + juce::String(samplerate, 2)
               + " | sampleRateUsed = " + juce::String(this->samplerate, 2)
               + " | maxBlockSize = " + juce::String(maxBlockSize)
               + " | windowSize = " + juce::String(synthesisWindowSize)
               + " | hopSize = " + juce::String(analysisHopSize));
}

// [SOLA] Configure
//------------------------------------------------------------------------------
void Sola::configure(int analysisWindowSize, int analysisHopSize)
{
    constexpr const char* kCat = "SOLA";
    LOG_SCOPE(kCat);

    this->analysisWindowSize = analysisWindowSize;
    this->analysisHopSize    = analysisHopSize;
    synthesisWindowSize      = analysisWindowSize;

    for (auto& chState : channelStates)
    {
        chState.olaBuffer.assign(analysisWindowSize, 0.0f);
        chState.olaWindow.assign(analysisWindowSize, 0.0f);
        chState.nextFrameOutTime = 0.0;
    }

    LOGI(kCat, "configure(): updated"
               " | analysisWindowSize = " + juce::String(this->analysisWindowSize)
               + " | analysisHopSize = " + juce::String(this->analysisHopSize)
               + " | channelStates = " + juce::String((int) channelStates.size()));
}

// [SOLA] Seek
//------------------------------------------------------------------------------
void Sola::seek(double outputTimeSeconds)
{
    constexpr const char* kCat = "SOLA";
    LOG_SCOPE(kCat);

    LOGI(kCat, "seek(): resetting state"
               " | outputTimeSeconds = " + juce::String(outputTimeSeconds, 6)
               + " | channelStates = " + juce::String((int) channelStates.size()));

    for (auto& chState : channelStates)
    {
        std::fill(chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
        std::fill(chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
        chState.nextFrameOutTime = outputTimeSeconds;
    }
}

// [SOLA] Interpolate Range of Samples
//------------------------------------------------------------------------------
void Sola::interpolateRange(float *outFrame,
                            int outLength,
                            const float *inputData,
                            int inputLength,
                            double inputPos)
{
    for (int i = 0; i < outLength; ++i)
    {
        const double pos   = inputPos + (double)i;
        const int    idxA  = (int)std::floor(pos);
        const int    idxB  = idxA + 1;
        const float  frac  = (float)(pos - (double)idxA);

        if (idxA < 0 || idxA >= inputLength)
        {   outFrame[(size_t)i] = 0.0f;   }
        else
        {
            const float sampA = inputData[idxA];
            const float sampB = (idxB >= 0 && idxB < inputLength)
                              ? inputData[idxB]
                              : sampA;

            outFrame[(size_t)i] = sampA + frac * (sampB - sampA);
        }
    }
}

// [SOLA] Tile
//------------------------------------------------------------------------------
void Sola::tile(ChannelState& chState,
                const float* frame,
                float* outputPtr,
                int& outputPos,
                int outputLength,
                int synthesisWindowSize,
                int synthesisHopSize,
                const float* cachedWindow)
{
    const int W = synthesisWindowSize;
    const int H = synthesisHopSize;

    const int targetOutputPos = outputPos + H;
    int windowPos = 0;

    while (outputPos < targetOutputPos && outputPos < outputLength)
    {
        const int remainingInHop    = targetOutputPos - outputPos;
        const int remainingInOutput = outputLength - outputPos;
        const int remainingInWindow = W - windowPos;

        const int availableLen = std::min({ remainingInHop, remainingInOutput, remainingInWindow });

        if (availableLen <= 0)
            break;

        const int hopOffset = outputPos - (targetOutputPos - H);

        for (int i = 0; i < availableLen; ++i)
        {
            const int olaIndex = hopOffset + i;
            const int winIndex = windowPos + i;

            const float w    = cachedWindow[winIndex];
            const float samp = frame[winIndex];

            chState.olaBuffer[(size_t)olaIndex] += samp * w;
            chState.olaWindow[(size_t)olaIndex] += w;
        }

        for (int i = 0; i < availableLen; ++i)
        {
            const int outIndex = outputPos + i;
            const int olaIndex = hopOffset + i;

            const float energy = chState.olaWindow[(size_t)olaIndex];
            outputPtr[outIndex] = (energy > 1.0e-6f)
                                ? (chState.olaBuffer[(size_t)olaIndex] / energy)
                                : 0.0f;
        }

        outputPos += availableLen;
        windowPos += availableLen;

        if (windowPos >= W)
            windowPos = 0;
    }
}

// [SOLA] Shift
//------------------------------------------------------------------------------
void Sola::shift(ChannelState& chState,
                 double playheadSeconds,
                 int& outputPos,
                 const int synthesisWindowSize,
                 const int synthesisHopSize)
{
    const int consumed = std::min(synthesisHopSize, synthesisWindowSize);
    if (consumed < synthesisWindowSize)
    {
        std::memmove(chState.olaBuffer.data(),
                     chState.olaBuffer.data() + consumed,
                     (size_t)(synthesisWindowSize - consumed) * sizeof(float));
        std::memmove(chState.olaWindow.data(),
                     chState.olaWindow.data() + consumed,
                     (size_t)(synthesisWindowSize - consumed) * sizeof(float));

        std::fill(chState.olaBuffer.begin() + (synthesisWindowSize - consumed),
                  chState.olaBuffer.end(),
                  0.0f);
        std::fill(chState.olaWindow.begin() + (synthesisWindowSize - consumed),
                  chState.olaWindow.end(),
                  0.0f);
    }
    else
    {
        std::fill(chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
        std::fill(chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
    }

    chState.nextFrameOutTime = playheadSeconds + (double)outputPos / samplerate;
}

// [SOLA] Process Block
//------------------------------------------------------------------------------
void Sola::processBlock(juce::AudioBuffer<float>& outputBuffer,
                        const juce::AudioBuffer<float>& inputBuffer,
                        double playheadSeconds,
                        const std::vector<SegmentMapping>& segments)
{
    constexpr const char* kCat = "SOLA";

    const int outputNumCh  = outputBuffer.getNumChannels();
    const int outputLength = outputBuffer.getNumSamples();

    const int inputNumCh  = inputBuffer.getNumChannels();
    const int inputLength = inputBuffer.getNumSamples();

    if (samplerate <= 0.0 || outputLength <= 0 || outputNumCh <= 0)
    {
        LOGE_RT(kCat, "BAD_STATE: samplerate<=0 or output dims invalid");
        outputBuffer.clear();
        return;
    }

    if (synthesisWindowSize <= 0 || analysisHopSize <= 0)
    {
        LOGE_RT(kCat, "BAD_STATE: synthesisWindowSize<=0 or analysisHopSize<=0");
        outputBuffer.clear();
        return;
    }

    if (segments.empty())
    {
        LOGW_RT(kCat, "SEGMENTS_EMPTY: pass-through");
        const int srcLen   = inputBuffer.getNumSamples();
        const int srcStart = (int)std::floor(playheadSeconds * samplerate);

        for (int ch = 0; ch < outputNumCh; ++ch)
        {
            const float* src = inputBuffer.getReadPointer(std::min(ch, inputNumCh - 1));
            float* dst       = outputBuffer.getWritePointer(ch);

            for (int n = 0; n < outputLength; ++n)
            {
                const int idx = juce::jlimit(0, srcLen - 1, srcStart + n);
                dst[n] = src[idx];
            }
        }
        return;
    }

    if ((int)channelStates.size() != outputNumCh)
    {
        LOGW_RT(kCat, "CHANNEL_STATE_RESIZE");
        channelStates.clear();
        channelStates.resize((size_t)outputNumCh);

        for (auto& chState : channelStates)
        {
            chState.olaBuffer.assign((size_t)synthesisWindowSize, 0.0f);
            chState.olaWindow.assign((size_t)synthesisWindowSize, 0.0f);
            chState.nextFrameOutTime = playheadSeconds;
        }
    }
    else
    {
        for (auto& chState : channelStates)
        {
            if ((int)chState.olaBuffer.size() != synthesisWindowSize)
            {
                LOGW_RT(kCat, "OLA_SIZE_FIXUP");
                chState.olaBuffer.assign((size_t)synthesisWindowSize, 0.0f);
                chState.olaWindow.assign((size_t)synthesisWindowSize, 0.0f);
            }
        }
    }

    // Rate-limited logging (every 256 blocks)
    static std::atomic<uint32_t> g_blockCounter { 0 };
    const uint32_t blockIdx     = g_blockCounter.fetch_add(1, std::memory_order_relaxed);
    const bool     logThisBlock = ((blockIdx & 0xFFu) == 0u);

    if (logThisBlock)
    {
        char m[256];
        std::snprintf(m, sizeof(m),
                      "BLOCK: playhead=%.6f outLen=%d inLen=%d win=%d hop=%d segN=%d",
                      playheadSeconds, outputLength, inputLength,
                      synthesisWindowSize, analysisHopSize, (int)segments.size());
        LOGD_RT(kCat, m);
    }

    outputBuffer.clear();

    for (int ch = 0; ch < outputNumCh; ++ch)
    {
        auto& chState    = channelStates[(size_t)ch];
        const float* inputData = inputBuffer.getReadPointer(std::min(ch, inputNumCh - 1));

        float* outputPtr = outputBuffer.getWritePointer(ch);
        int outputPos = 0;

        while (outputPos < outputLength)
        {
            if (chState.nextFrameOutTime < playheadSeconds)
                chState.nextFrameOutTime = playheadSeconds;

            const double expectedTime = playheadSeconds + (double)outputPos / samplerate;
            if (chState.nextFrameOutTime > expectedTime + 1.0e-9)
            {
                LOGW_RT(kCat, "SEEK_TRIGGER");
                seek(expectedTime);
                chState.nextFrameOutTime = expectedTime;
            }

            const double frameOutTime = expectedTime;
            const int segmentIndex = SegmentMapper::findSegmentIndex(frameOutTime, segments);
            float segmentStretchRatio = 1.0f;

            if (segmentIndex >= 0 && segmentIndex < (int)segments.size())
                segmentStretchRatio = std::abs(segments[(size_t)segmentIndex].stretchRatio);
            else if (logThisBlock)
                LOGW_RT(kCat, "SEGMENT_INDEX_INVALID: using stretch=1");

            if (!std::isfinite(segmentStretchRatio) || segmentStretchRatio <= 0.0f)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "BAD_STRETCH_RATIO: value=%f)", segmentStretchRatio);
                LOGE_RT(kCat, msg);
                segmentStretchRatio = juce::jlimit(0.00001f, 99999.0f, segmentStretchRatio);
            }

            int synthesisHopSize = (int)std::lround((double)analysisHopSize * (double)segmentStretchRatio);

            if (synthesisHopSize < 1)
            {
                LOGE_RT(kCat, "HOP_CLAMP: synthesisHopSize<1 -> 1");
                synthesisHopSize = 1;
            }

            const double inputTime = SegmentMapper::calculateSourcePosition(frameOutTime, segments);
            const double inputPos  = inputTime * samplerate;

            if (!std::isfinite(inputTime) || !std::isfinite(inputPos))
            {
                LOGE_RT(kCat, "BAD_INPUT_POSITION");
                break;
            }

            if (logThisBlock && ch == 0 && outputPos == 0)
            {
                char m[256];
                std::snprintf(m, sizeof(m),
                              "SEG: idx=%d stretch=%.6f synHop=%d inputTime=%.6f",
                              segmentIndex, segmentStretchRatio, synthesisHopSize, inputTime);
                LOGD_RT(kCat, m);
            }

            Sola::interpolateRange(frame.data(),
                                   synthesisWindowSize,
                                   inputData, inputLength, inputPos);

            Sola::tile(chState,
                       frame.data(),
                       outputPtr, outputPos, outputLength,
                       synthesisWindowSize, synthesisHopSize,
                       cachedWindow.data());

            Sola::shift(chState,
                        playheadSeconds,
                        outputPos,
                        synthesisWindowSize,
                        synthesisHopSize);
        }
    }
}


//==============================================================================
// Granular
//==============================================================================

// [Granular] Constructor
//------------------------------------------------------------------------------
Granular::Granular(int windowSize,
                   int hopSize,
                   double grainSizeMs,
                   int grainDensity,
                   double samplerate)
        : window((int)(grainSizeMs / 1000.0 * samplerate),
                 juce::dsp::WindowingFunction<float>::hann,
                 true),
          samplerate(samplerate),
          grainSizeSamples((int)(grainSizeMs / 1000.0 * samplerate))
{
    constexpr const char* kCat = "GRAN";
    LOG_SCOPE(kCat);
    PERF_START(gran_ctor);

    cachedWindow.resize((int)(grainSizeSamples), 1.0f);
    window.multiplyWithWindowingTable(cachedWindow.data(), (int) grainSizeSamples);

    LOGI(kCat, "Granular(): initialized"
               " | windowSizeArg = " + juce::String(windowSize)
               + " | hopSizeArg = " + juce::String(hopSize)
               + " | grainSizeMs = " + juce::String(grainSizeMs, 3)
               + " | grainDensity = " + juce::String(grainDensity)
               + " | sampleRate = " + juce::String(samplerate, 2)
               + " | grainSizeSamples = " + juce::String(grainSizeSamples));

    PERF_END(gran_ctor);
}

// [Granular] Prepare
//------------------------------------------------------------------------------
void Granular::prepare(double inSampleRate, int maxBlockSize)
{
    constexpr const char* kCat = "GRAN";
    LOG_SCOPE(kCat);

    this->samplerate = (inSampleRate > 0.0) ? inSampleRate : 44100.0;
    grainSizeSamples = (int)(grainSizeMs / 1000.0 * this->samplerate);

    grain.assign((size_t)grainSizeSamples, 0.0f);
    cachedWindow.assign((size_t)grainSizeSamples, 1.0f);
    window.fillWindowingTables(cachedWindow.data(),
                               (size_t)grainSizeSamples,
                               juce::dsp::WindowingFunction<float>::hann,
                               true);

    for (auto& chState : channelStates)
    {
        chState.olaBuffer.assign((size_t)grainSizeSamples, 0.0f);
        chState.olaWindow.assign((size_t)grainSizeSamples, 0.0f);
        chState.nextFrameOutTime = 0.0;
    }

    atomicGrainSizeMs.store(grainSizeMs, std::memory_order_relaxed);

    seek(0.0);

    LOGI(kCat, "prepare(): configured"
               " | sampleRateIn = " + juce::String(inSampleRate)
               + " | sampleRateUsed = " + juce::String(this->samplerate)
               + " | maxBlockSize = " + juce::String(maxBlockSize)
               + " | grainSizeMs = " + juce::String(grainSizeMs)
               + " | grainSizeSamples = " + juce::String(grainSizeSamples)
               + " | channelStates = " + juce::String((int) channelStates.size()));
}

// [Granular] Seek
//------------------------------------------------------------------------------
void Granular::seek(double outputTimeSeconds)
{
    constexpr const char* kCat = "GRAN";
    LOG_SCOPE(kCat);

    LOGI(kCat, "seek(): resetting state"
               " | outputTimeSeconds = " + juce::String(outputTimeSeconds, 6)
               + " | channelStates = " + juce::String((int) channelStates.size()));

    for (auto& chState : channelStates)
    {
        std::fill(chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
        std::fill(chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
        chState.nextFrameOutTime = outputTimeSeconds;
    }
}


// [Granular] Configure
//------------------------------------------------------------------------------
void Granular::configure(int samplerate, double grainSizeMs, int grainDensity)
{
    constexpr const char* kCat = "GRAN";
    LOG_SCOPE(kCat);

    this->samplerate = samplerate;
    this->grainSizeMs = grainSizeMs;
    grainSizeSamples = ((int)(grainSizeMs / 1000.0 * samplerate));

    for (auto& chState : channelStates)
    {
        chState.olaBuffer.assign(grainSizeSamples, 0.0f);
        chState.olaWindow.assign(grainSizeSamples, 0.0f);
        chState.nextFrameOutTime = 0.0;
    }

    LOGI(kCat, "configure(): updated"
               " | sampleRate = " + juce::String(this->samplerate)
               + " | grainSizeMs = " + juce::String(this->grainSizeMs)
               + " | grainDensity = " + juce::String(grainDensity)
               + " | grainSizeSamples = " + juce::String(grainSizeSamples));
}

void Granular::setGrainParams(double sizeMs,
                              float scatterMs,
                              float ampJitter,
                              int density)
{
    constexpr const char* kCat = "GRAN";
    LOG_SCOPE(kCat);

    atomicGrainSizeMs.store(sizeMs,    std::memory_order_relaxed);
    atomicScatterMs.store(scatterMs,   std::memory_order_relaxed);
    atomicAmpJitter.store(ampJitter,   std::memory_order_relaxed);
    atomicGrainDensity.store(density,  std::memory_order_relaxed);

    LOGD(kCat, "setGrainParams(): updated"
               " | sizeMs = " + juce::String(sizeMs, 3)
               + " | scatterMs = " + juce::String(scatterMs, 3)
               + " | ampJitter = " + juce::String(ampJitter, 3)
               + " | density = " + juce::String(density));
}


// [Granular] Process Block
//------------------------------------------------------------------------------
void Granular::processBlock(juce::AudioBuffer<float>& outputBuffer,
                            const juce::AudioBuffer<float>& inputBuffer,
                            double playheadSeconds,
                            const std::vector<SegmentMapping>& segments)
{
    PERF_START(granProcessBlock);
    constexpr const char* kCat = "GRAN";

    const double currentGrainSizeMs  = atomicGrainSizeMs.load(std::memory_order_relaxed);
    const float  currentScatterMs    = atomicScatterMs.load(std::memory_order_relaxed);
    const float  currentAmpJitter    = atomicAmpJitter.load(std::memory_order_relaxed);
    const int    currentDensity      = juce::jmax(1, (int) atomicGrainDensity.load(std::memory_order_relaxed));
    const int    currentGrainSamples = juce::jmax(1, (int) std::round(currentGrainSizeMs / 1000.0 * samplerate));

    const int outputNumCh  = outputBuffer.getNumChannels();
    const int outputLength = outputBuffer.getNumSamples();
    const int inputNumCh   = inputBuffer.getNumChannels();
    const int inputLength  = inputBuffer.getNumSamples();

    if (samplerate <= 0.0 || outputNumCh <= 0 || outputLength <= 0)
    {
        LOGE_RT(kCat, "BAD_STATE: invalid samplerate/output");
        outputBuffer.clear();
        PERF_END(granProcessBlock);
        return;
    }

    if (inputNumCh <= 0 || inputLength <= 0)
    {
        LOGW_RT(kCat, "INPUT_EMPTY: clearing output");
        outputBuffer.clear();
        PERF_END(granProcessBlock);
        return;
    }

    if (segments.empty())
    {
        LOGW_RT(kCat, "SEGMENTS_EMPTY: pass-through");

        const int srcStart = (int) std::floor(playheadSeconds * samplerate);

        for (int ch = 0; ch < outputNumCh; ++ch)
        {
            const float* src = inputBuffer.getReadPointer(std::min(ch, inputNumCh - 1));
            float* dst       = outputBuffer.getWritePointer(ch);

            for (int n = 0; n < outputLength; ++n)
            {
                const int idx = juce::jlimit(0, inputLength - 1, srcStart + n);
                dst[n] = src[idx];
            }
        }

        PERF_END(granProcessBlock);
        return;
    }

    if ((int) cachedWindow.size() != currentGrainSamples || (int) grain.size() != currentGrainSamples)
    {
        grain.assign((size_t) currentGrainSamples, 0.0f);
        cachedWindow.assign((size_t) currentGrainSamples, 1.0f);
        window.fillWindowingTables(cachedWindow.data(),
                                   (size_t) currentGrainSamples,
                                   juce::dsp::WindowingFunction<float>::hann,
                                   true);
        LOGD_RT(kCat, "WINDOW_REBUILD");
    }

    if ((int) channelStates.size() != outputNumCh)
    {
        channelStates.clear();
        channelStates.resize((size_t) outputNumCh);

        for (auto& chState : channelStates)
        {
            chState.olaBuffer.clear();
            chState.olaWindow.clear();
            chState.nextFrameOutTime = playheadSeconds;
        }

        LOGW_RT(kCat, "CHANNEL_STATE_RESIZE");
    }

    outputBuffer.clear();

    const int scatterSamples = (int) std::round(currentScatterMs / 1000.0f * samplerate);
    const int synthesisHop   = juce::jmax(1, currentGrainSamples / currentDensity);

    for (int ch = 0; ch < outputNumCh; ++ch)
    {
        auto& chState    = channelStates[(size_t) ch];
        const float* in  = inputBuffer.getReadPointer(std::min(ch, inputNumCh - 1));
        float* out       = outputBuffer.getWritePointer(ch);

        const int neededSize = outputLength + currentGrainSamples + synthesisHop;

        if ((int) chState.olaBuffer.size() < neededSize)
        {
            chState.olaBuffer.resize((size_t) neededSize, 0.0f);
            chState.olaWindow.resize((size_t) neededSize, 0.0f);
        }

        if (chState.nextFrameOutTime < playheadSeconds)
            chState.nextFrameOutTime = playheadSeconds;

        int grainOutPos = 0;

        while (grainOutPos < outputLength)
        {
            const double grainOutTime = playheadSeconds + (double) grainOutPos / samplerate;
            const double srcTime      = SegmentMapper::calculateSourcePosition(grainOutTime, segments);

            if (!std::isfinite(srcTime))
            {
                LOGE_RT(kCat, "BAD_SOURCE_TIME");
                break;
            }

            const double srcPosBase = srcTime * samplerate;

            for (int g = 0; g < currentDensity; ++g)
            {
                std::uniform_int_distribution<int> posDist(-scatterSamples, scatterSamples);
                std::uniform_real_distribution<float> ampDist(1.0f - currentAmpJitter, 1.0f + currentAmpJitter);

                const int scatter = (scatterSamples > 0) ? posDist(rng) : 0;
                const float gain  = ampDist(rng);

                const double srcPos = srcPosBase + (double) scatter;

                for (int i = 0; i < currentGrainSamples; ++i)
                {
                    const double p   = srcPos + (double) i;
                    const int i0     = (int) std::floor(p);
                    const int i1     = i0 + 1;
                    const float frac = (float) (p - (double) i0);

                    float s = 0.0f;
                    if (i0 >= 0 && i0 < inputLength)
                    {
                        const float s0 = in[i0];
                        const float s1 = (i1 >= 0 && i1 < inputLength) ? in[i1] : s0;
                        s = s0 + frac * (s1 - s0);
                    }

                    const float w = cachedWindow[(size_t) i];
                    const int outIdx = grainOutPos + i;

                    if (outIdx >= 0 && outIdx < (int) chState.olaBuffer.size())
                    {
                        chState.olaBuffer[(size_t) outIdx] += s * w * gain;
                        chState.olaWindow[(size_t) outIdx] += w;
                    }
                }
            }

            grainOutPos += synthesisHop;
        }

        for (int n = 0; n < outputLength; ++n)
        {
            const float olaWindow = chState.olaWindow[(size_t) n];
            out[n] = (olaWindow > 1.0e-6f) ? (chState.olaBuffer[(size_t) n] / olaWindow) : 0.0f;
        }

        const int tail = (int) chState.olaBuffer.size() - outputLength;
        if (tail > 0)
        {
            std::memmove(chState.olaBuffer.data(),
                         chState.olaBuffer.data() + outputLength,
                         (size_t) tail * sizeof(float));
            std::memmove(chState.olaWindow.data(),
                         chState.olaWindow.data() + outputLength,
                         (size_t) tail * sizeof(float));

            std::fill(chState.olaBuffer.begin() + tail, chState.olaBuffer.end(), 0.0f);
            std::fill(chState.olaWindow.begin() + tail, chState.olaWindow.end(), 0.0f);
        }
        else
        {
            std::fill(chState.olaBuffer.begin(), chState.olaBuffer.end(), 0.0f);
            std::fill(chState.olaWindow.begin(), chState.olaWindow.end(), 0.0f);
        }

        chState.nextFrameOutTime = playheadSeconds + (double) outputLength / samplerate;
    }

    PERF_END(granProcessBlock);
}
