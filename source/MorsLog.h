#pragma once
#include <JuceHeader.h>

//==============================================================================
// Structured CSV logger for quantitative analysis
// Separate from MorsDebug — this is for data collection, not debugging
//==============================================================================
class MorsLog
{
public:
    enum class Sheet
    {
        BlockPerf,   // one row per audio block (sampled)
        OnsetMap,    // one row per reanalyze() call
        SegmentMap,  // one row per segment built
    };

    static void init();
    static void shutdown();

    // Call from audio thread (via FIFO like MorsDebug)
    static void logBlockPerf(double blockMs,
                             double budgetMs,
                             double loadPct,
                             bool   bandsOn) noexcept;

    // Call from message thread after reanalyze()
    static void logOnsetMap(int    srcOnsetCount,
                            int    refOnsetCount,
                            int    dtwPathLength,
                            double dtwFinalCost,
                            double avgCost,
                            int    band = -1);  // -1 = full band, 0-3 = per band

    // Call from message thread after rebuildSegments()
    static void logSegment(int    segIndex,
                           double startTime,
                           double endTime,
                           float  stretchRatio,
                           int    band = -1);

    static void flushRealtime();

private:
    static juce::File makeFilePath(const juce::String& name);

    static std::unique_ptr<juce::FileOutputStream> blockPerfStream;
    static std::unique_ptr<juce::FileOutputStream> onsetMapStream;
    static std::unique_ptr<juce::FileOutputStream> segmentMapStream;
    static std::atomic<bool> g_initStatus;
};
