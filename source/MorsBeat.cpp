#include "MorsBeat.h"


Onset::Onset(int N, int H) : window (N, juce::dsp::WindowingFunction<float>::hann, true)
{
    cachedWindow.resize((size_t)N, 1.0f);
    window.multiplyWithWindowingTable(cachedWindow.data(), N);
}
Onset::~Onset()
{
}

void Onset::reset()
{
    onsetTimes.clear();
    noveltyEnergy.clear();
}

void Onset::prepare() { onsetTimes.clear(); }

// COMPUTE ONSET NOVELTY CURVE
//==============================================================================
std::pair<std::vector<float>, double> Onset::novelty(const std::vector<float>& data,
                                                      double Fs,
                                                      int    N,
                                                      int    H,
                                                      float  gamma,
                                                      bool   norm)
{
    // INITIALIZE VARIABLES
    // ----------------------------------------------------------------
    // Precompute squared window
    cachedWindow.resize((size_t)N, 1.0f);
    window.multiplyWithWindowingTable(cachedWindow.data(), N);
    
    // instantaneous power/energy
    std::vector<float> w2(N);
    for (int i = 0; i < N; ++i)
    { w2[i] = cachedWindow[i] * cachedWindow[i];}
    
    // LOCAL ENERGY PER FRAME
    // ----------------------------------------------------------------
    const int totalSamples = (int)data.size();
    const int numFrames    = totalSamples / H;
    
    // Declare vector to collect frame-wise energy
    std::vector<float> energyLocal(numFrames, 0.0f);
    
    // Loop over all frames
    for (int frame = 0; frame < numFrames; ++frame)
    {
        // Center the window on the current hop position
        int center = frame * H;
        
        // Accumulate windowed energy for this frame
        float energy = 0.0f;
        
        // Walk through all N samples of the window
        for (int k = 0; k < N; ++k)
        {
            // Offset index so window is centred on frame, not left-aligned
            int idx = center - N / 2 + k;
            
            // Skip samples that fall outside the signal boundary
            if (idx >= 0 && idx < totalSamples)
            {
                float x = data[idx];
                
                // Accumulate squared signal weighted by squared window
                energy += (x * x) * w2[k];
            }
        }
        energyLocal[frame] = energy;
    }
    // LOGARITHMIC COMPRESSION
    // ----------------------------------------------------------------
    if (gamma > 0.0f)
    {
        for (auto& frame : energyLocal)
        { frame = std::log(1.0f + gamma * frame); }
    }
    double fsFeature = Fs / (double)H;
    
    // LOCAL ENERGY DIFFERENCE, HALF-WAVE RECTIFICATION
    // ----------------------------------------------------------------
    std::vector<float> energyLocalDiff(numFrames - 1, 0.0f);
    
    for (int i = 0; i < numFrames - 1; ++i)
    {
        float diff = energyLocal[i + 1] - energyLocal[i];
        energyLocalDiff[i] = std::max(0.0f, diff); // half-wave rectify
    }
    
    // NORMALIZATION
    // ----------------------------------------------------------------
    if (norm)
    {
        float maxEnergy = *std::max_element(energyLocalDiff.begin(),
                                         energyLocalDiff.end());
        if (maxEnergy > 0.0f)
        {
            for (auto& energy : energyLocalDiff)
                { energy /= maxEnergy; }
        }
    }
    
    return std::make_pair(energyLocalDiff, fsFeature);
}

// PICK PEAKS FROM NOVELTY CURVE
//==============================================================================
// Helper Function - Sliding Maximum
// ----------------------------------------------------------------
// Sliding maximum with asymmetric origin offset
std::vector<float> Onset::slidingMax(const std::vector<float>& x, int windowSize, int origin)
{
    const int N    = (int)x.size();
    const int half = windowSize / 2 + origin;
    std::vector<float> result(N, 0.0f);
    std::deque<int> dq;

    for (int i = 0; i < N; ++i)
    {
        while (!dq.empty() && dq.front() < i - half)
            dq.pop_front();
        while (!dq.empty() && x[dq.back()] <= x[i])
            dq.pop_back();
        dq.push_back(i);
        result[i] = x[dq.front()];
    }
    return result;
}
// Helper Function - Sliding Average
// ----------------------------------------------------------------
// Sliding average with asymmetric origin offset
std::vector<float> Onset::slidingAvg(const std::vector<float>& x, int windowSize, int origin)
{
    const int N    = (int)x.size();
    const int half = windowSize / 2;
    std::vector<float> result(N, 0.0f);

    for (int i = 0; i < N; ++i)
    {
        int start = std::max(0, i - half - origin);
        int end   = std::min(N, i + half - origin + 1);
        float sum = 0.0f;
        for (int j = start; j < end; ++j)
            sum += x[j];
        result[i] = sum / (float)(end - start);
    }
    return result;
}

// Peak Picking
// ----------------------------------------------------------------
std::vector<double> Onset::peak(std::vector<float>  activations,
                                double fsFeature,
                                float  threshold,
                                bool   scores,
                                bool   minMsGate,
                                double minMsValue,
                                float  preAvg,
                                float  postAvg,
                                float  preMax,
                                float  postMax)
{
    // CONVERSIONS
    // -------------------------------------------------------------------------
    // Convert all time parameters from milliseconds to seconds
    float preAvg_s  = preAvg  / 1000.0f;
    float postAvg_s = postAvg / 1000.0f;
    float preMax_s  = preMax  / 1000.0f;
    float postMax_s = postMax / 1000.0f;
    
    // Total span of the local maximum window in novelty frames (pre + post converted to frames)
    int maxLength = (int)((preMax_s + postMax_s) * fsFeature) + 1;
    // Shifts the max window left or right so it is asymmetric when preMax != postMax
    int maxOrigin = (int)((preMax_s - postMax_s) * fsFeature / 2);
    
    // Same span and offset calculation for the moving average window
    int avgLength = (int)((preAvg_s + postAvg_s) * fsFeature) + 1;
    int avgOrigin = (int)((preAvg_s - postAvg_s) * fsFeature / 2);
    
    const int N = (int)activations.size();
    
    // Convert minimum onset gap from ms to frames — 0 disables the gate entirely
    const int combineFrames = minMsGate ? (int)(minMsValue / 1000.0 * fsFeature) : 0;

    // MOVING MAXIMUM — keep only local maxima
    // -------------------------------------------------------------------------
    // All frames start zeroed — only frames that survive peak tests will be non-zero
    std::vector<float> detections(N, 0.0f);

    if (maxLength > 1)
    {
        // Compute the rolling maximum of activations over the window around each frame
        auto movMax = slidingMax(activations, maxLength, maxOrigin);

        // A frame survives only if its activation equals the local maximum — all others are zeroed
        for (int i = 0; i < N; ++i)
            { detections[i] = (activations[i] == movMax[i]) ? activations[i] : 0.0f; }
    }
    else
    {
        // Window too small to compare neighbours — pass all frames through as candidates
        detections = activations;
    }

    // MOVING AVERAGE — detections must exceed local average + threshold
    // -------------------------------------------------------------------------
    std::vector<float> movAvg(N, 0.0f);

    if (avgLength > 1)
    {
        // Compute rolling average of the original activations (not detections) around each frame
        movAvg = slidingAvg(activations, avgLength, avgOrigin);

        // Suppress any peak whose activation does not clear the local average by at least threshold
        for (int i = 0; i < N; ++i)
            {
                detections[i] = (detections[i] >= movAvg[i] + threshold)
                              ? detections[i]
                              : 0.0f;
            }
    }
    else
    {
        // No meaningful average window — apply a flat global threshold instead
        for (int i = 0; i < N; ++i)
            detections[i] = (detections[i] >= threshold) ? detections[i] : 0.0f;
    }

    // MINIMUM GAP — enforce minimum spacing between accepted onsets
    // -------------------------------------------------------------------------
    std::vector<int> positions;

    if (combineFrames > 0)
    {
        // Initialise to -combineFrames so the first onset at frame 0 is always eligible
        int lastOnset = -combineFrames;

        for (int i = 0; i < N; ++i)
        {
            // Accept onset only if it is at least combineFrames away from the previous accepted onset
            if (detections[i] > 0.0f && (i - lastOnset) >= combineFrames)
            {
                positions.push_back(i);
                lastOnset = i;
            }
        }
    }
    else
    {
        // Gate disabled — collect every frame that survived the peak and threshold tests
        for (int i = 0; i < N; ++i)
            if (detections[i] > 0.0f)
                positions.push_back(i);
    }

    // RETURN — convert frame indices to seconds by dividing by fsFeature
    // -------------------------------------------------------------------------
    onsetTimes.clear();                          // ← add this
    onsetTimes.reserve(positions.size()); // pre-allocate to avoid reallocation
    for (int idx : positions)
    {    onsetTimes.push_back((double)idx / (double)fsFeature);   }
    
    if (onsetTimes.empty() || onsetTimes.front() > 0.01)
    {   onsetTimes.insert(onsetTimes.begin(), 0.0);   }
    return onsetTimes;
}
//==============================================================================
// Onset Detection — init
//==============================================================================
OnsetAubio::OnsetAubio() = default;
OnsetAubio::~OnsetAubio()
{
    if (onset)      del_aubio_onset(onset);
    if (aubioInput) del_fvec(aubioInput);
    if (aubioOutput)del_fvec(aubioOutput);
    aubio_cleanup(); // optional global cleanup
}


// Onset Detection — reset
//==============================================================================
void OnsetAubio::reset()
{
    writeIndex = 0;
    totalSamplesProcessed = 0;
    onsetTimes.clear();
}


// Onset Detection — Prepare
//==============================================================================
void OnsetAubio::prepare(double sr,
                         int winSize,
                         int hop,
                         float onsetThresh,
                         float onsetSilence,
                         float onsetMinMs)
{
    // Clean up any previous state
    // -------------------------------------------------------------------------
    if (onset)      { del_aubio_onset(onset); onset = nullptr; }
    if (aubioInput) { del_fvec(aubioInput);  aubioInput = nullptr; }
    if (aubioOutput){ del_fvec(aubioOutput); aubioOutput = nullptr; }
    aubio_cleanup(); // global cleanup
    
    sampleRate = sr;
    hopSize    = hop;
    writeIndex = 0;
    totalSamplesProcessed = 0;
    onsetTimes.clear();

    // Create aubio onset detector
    // -------------------------------------------------------------------------
    // method: energy, hfc, complex, phase, wphase, specdiff, kl, mkl, specflux
    onset = new_aubio_onset((char*)"specflux",
                            winSize,
                            hopSize,
                            static_cast<uint_t>(sr));
    
    // Configure Onset Detection
    // -------------------------------------------------------------------------
    aubio_onset_set_threshold(onset, onsetThresh);
    aubio_onset_set_silence(onset, onsetSilence);
    aubio_onset_set_minioi_ms(onset, onsetMinMs);
    
    // Allocate aubio I/O vectors
    // -------------------------------------------------------------------------
    aubioInput  = new_fvec(hopSize);    // new_fvec creates a bufffer
    aubioOutput = new_fvec(1);          // 0: no onset; !0: onset
}


// Onset Detection — Process
//==============================================================================
void OnsetAubio::processBlock(const float* samples, int numSamples)
{
    // Error Check
    // +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    if (!onset || !aubioInput || !aubioOutput || hopSize <= 0)
        return;

    // MAIN PROCESSING LOOP
    // -------------------------------------------------------------------------
    for (int n = 0; n < numSamples; ++n)
    {
        // Fill aubio sample by sample
        aubioInput->data[writeIndex] = samples[n];
        ++writeIndex;   // advance writeIndex

        // run onset detection with  hop-sized collected samples
        if (writeIndex >= hopSize)
        {
            aubio_onset_do(onset, aubioInput, aubioOutput);

            // if non-zero then onset detected in current frame
            if (aubioOutput->data[0] != 0.0f)
            {
                std::uint64_t onsetPosition = totalSamplesProcessed;
                double onsetTimeSec = static_cast<double>(onsetPosition) / sampleRate;

                // Store for later inspection
                onsetTimes.push_back(onsetTimeSec);
            }
            // Reset index to start filling next frame
            writeIndex = 0;
        }
        // Advance counter
        ++totalSamplesProcessed;
    }
}


//// Tempo — init
////==============================================================================
//Tempo::Tempo() = default;
//Tempo::~Tempo()
//{
//    if (tempo)       { del_aubio_tempo(tempo);  tempo       = nullptr; }
//    if (aubioInput)  { del_fvec(aubioInput);     aubioInput  = nullptr; }
//    if (aubioOutput) { del_fvec(aubioOutput);    aubioOutput = nullptr; }
//}
//
//
//// Tempo — reset
////==============================================================================
//void Tempo::reset()
//{
//    totalSamplesProcessed = 0;
//}
//
//
//// Tempo — Prepare
////==============================================================================
//void Tempo::prepare(double inSampleRate, int inWinSize, int inHopSize)
//{
//    // Free existing aubio objects before reallocating
//    if (tempo)       { del_aubio_tempo(tempo);  tempo       = nullptr; }
//    if (aubioInput)  { del_fvec(aubioInput);     aubioInput  = nullptr; }
//    if (aubioOutput) { del_fvec(aubioOutput);    aubioOutput = nullptr; }
//
//    sampleRate = inSampleRate;
//    winSize    = inWinSize;
//    hopSize    = inHopSize;
//
//    aubioInput  = new_fvec(hopSize);
//    aubioOutput = new_fvec(2);
//    tempo       = new_aubio_tempo("default", winSize, hopSize, (uint_t)sampleRate);
//}
//
//
//// Tempo — Process
////==============================================================================
//void Tempo::processBlock(const float* samples, int numSamples)
//{
//    if (!tempo || !aubioInput || !aubioOutput || hopSize <= 0)
//        return;
//
//    int numHops = numSamples / hopSize;
//
//    for (int hop = 0; hop < numHops; ++hop)
//    {
//        // Copy one hop's worth of samples into aubio's input buffer
//        const float* hopStart = samples + (hop * hopSize);
//        for (int n = 0; n < hopSize; ++n)
//            aubioInput->data[n] = hopStart[n];
//
//        aubio_tempo_do(tempo, aubioInput, aubioOutput);
//        
////        if (aubioOutput->data[0] != 0.0f)
////        {
////            LOGD("TEMPO", "beat at "
////                + juce::String(aubio_tempo_get_last_ms(tempo), 3) + "ms, "
////                + juce::String(aubio_tempo_get_last_s(tempo),  3) + "s, "
////                + "frame " + juce::String(aubio_tempo_get_last(tempo)) + ", "
////                + juce::String(aubio_tempo_get_bpm(tempo),        2) + "bpm, "
////                + "confidence " + juce::String(aubio_tempo_get_confidence(tempo), 2));
////        }
//    }
//
//    // Optional: handle the remaining samples that don't fill a full hop
//    // aubio will just not process them, which is standard offline behaviour
//    totalSamplesProcessed += numSamples;
//}
//
Tempo::Tempo()  {}
Tempo::~Tempo() {}

//==============================================================================
void Tempo::prepare(double inSampleRate, int inHopSize, int inMinBpm, int inMaxBpm)
{
    sampleRate = inSampleRate;
    hopSize    = inHopSize;
    minBpm     = inMinBpm;
    maxBpm     = inMaxBpm;

    // Downsample the novelty to ~100 Hz — matches Python's Fs_feature
    // This keeps winLength small and autocorrelation fast
    downsampleFactor = (int)(sampleRate / inHopSize); // e.g. 44100/512 = 86
    fsFeature        = sampleRate / (double)(hopSize * downsampleFactor);

    // winLength in novelty frames: 60 * fsFeature / minBpm
    winLength = (int)(60.0 * fsFeature / minBpm);
    winLength = std::max(winLength, 4); // safety floor

    estimatedBpms.clear();

    LOGD("TEMPO", "fsFeature: "       + juce::String(fsFeature, 2)
                + " winLength: "      + juce::String(winLength)
                + " downsample: x"   + juce::String(downsampleFactor));
}

//==============================================================================
void Tempo::reset()
{
    estimatedBpms.clear();
}

//==============================================================================
// Half-wave rectified first difference — novelty function
//==============================================================================
std::vector<float> Tempo::computeNovelty(const float* samples, int numSamples)
{
    std::vector<float> novelty;
    novelty.reserve(numSamples);
    novelty.push_back(0.0f); // first sample has no predecessor

    for (int i = 1; i < numSamples; ++i)
    {
        float diff = samples[i] - samples[i - 1];
        novelty.push_back(std::max(0.0f, diff));
    }
    return novelty;
}

//==============================================================================
// Decimate by averaging blocks of `factor` samples
//==============================================================================
std::vector<float> Tempo::downsample(const std::vector<float>& signal, int factor)
{
    if (factor <= 1) return signal;

    const int outLen = (int)signal.size() / factor;
    std::vector<float> out(outLen, 0.0f);

    for (int i = 0; i < outLen; ++i)
    {
        float sum = 0.0f;
        for (int j = 0; j < factor; ++j)
            sum += signal[i * factor + j];
        out[i] = sum / (float)factor;
    }
    return out;
}

//==============================================================================
// Normalised autocorrelation of a windowed segment
//==============================================================================
std::vector<float> Tempo::autocorrelate(const std::vector<float>& segment)
{
    const int L = (int)segment.size();
    std::vector<float> ac(L, 0.0f);

    for (int lag = 0; lag < L; ++lag)
        for (int n = 0; n < L - lag; ++n)
            ac[lag] += segment[n] * segment[n + lag];

    if (ac[0] > 0.0f)
        for (float& v : ac) v /= ac[0];

    return ac;
}

//==============================================================================
void Tempo::processBlock(const float* samples, int numSamples)
{
    if (sampleRate <= 0.0 || winLength <= 0) return;

    // -------------------------------------------------------------------------
    // 1. Novelty function on raw samples
    // -------------------------------------------------------------------------
    auto novelty = computeNovelty(samples, numSamples);

    // -------------------------------------------------------------------------
    // 2. Downsample novelty to fsFeature — makes winLength manageable
    // -------------------------------------------------------------------------
    auto novDS = downsample(novelty, downsampleFactor);
    const int N = (int)novDS.size();

    if (N < winLength) return;

    // -------------------------------------------------------------------------
    // 3. BPM axis — computed in novelty-frame domain
    // -------------------------------------------------------------------------
    std::vector<float> bpmAxis;
    std::vector<int>   validLags;

    for (int lag = 1; lag <= winLength; ++lag)
    {
        float bpm = (float)(60.0 * fsFeature / lag);
        if (bpm >= minBpm && bpm <= maxBpm)
        {
            bpmAxis.push_back(bpm);
            validLags.push_back(lag);
        }
    }

    if (validLags.empty()) return;

    // -------------------------------------------------------------------------
    // 4. Hann window
    // -------------------------------------------------------------------------
    std::vector<float> hann(winLength);
    for (int i = 0; i < winLength; ++i)
        hann[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (winLength - 1)));

    // -------------------------------------------------------------------------
    // 5. Slide frames over downsampled novelty, hop = 1 novelty frame
    //    (matches Python's hop=8 at low fsFeature)
    // -------------------------------------------------------------------------
    estimatedBpms.clear();
    const int frameHop = std::max(1, winLength / 8); // ~equivalent to Python hop=8

    for (int center = winLength / 2; center < N - winLength / 2; center += frameHop)
    {
        // Window the segment
        std::vector<float> segment(winLength);
        for (int i = 0; i < winLength; ++i)
            segment[i] = novDS[center - winLength / 2 + i] * hann[i];

        auto ac = autocorrelate(segment);

        // Peak BPM in valid lag range
        float bestAc  = -1.0f;
        float bestBpm =  0.0f;

        for (int k = 0; k < (int)validLags.size(); ++k)
        {
            int lag = validLags[k];
            if (lag < (int)ac.size() && ac[lag] > bestAc)
            {
                bestAc  = ac[lag];
                bestBpm = bpmAxis[k];
            }
        }

        if (bestBpm > 0.0f)
            estimatedBpms.push_back(bestBpm);
    }
}

//==============================================================================
// Median across all frames — matches np.median(estimated_bpm)
//==============================================================================
float Tempo::getEstimatedBpm() const
{
    if (estimatedBpms.empty()) return 0.0f;

    std::vector<float> sorted = estimatedBpms;
    std::sort(sorted.begin(), sorted.end());

    const int n = (int)sorted.size();
    return (n % 2 == 0)
        ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0f
        : sorted[n / 2];
}
