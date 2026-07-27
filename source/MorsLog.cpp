#include "MorsLog.h"

std::unique_ptr<juce::FileOutputStream> MorsLog::blockPerfStream;
std::unique_ptr<juce::FileOutputStream> MorsLog::onsetMapStream;
std::unique_ptr<juce::FileOutputStream> MorsLog::segmentMapStream;
std::atomic<bool> MorsLog::g_initStatus { false };

static juce::File getDataDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                   .getParentDirectory()
                   .getChildFile("data");
    dir.createDirectory();
    return dir;
}

juce::File MorsLog::makeFilePath(const juce::String& name)
{
    const auto ts = juce::Time::getCurrentTime().formatted("%Y_%m_%d_%Hh%Mm%Ss");
    return getDataDir().getChildFile(name + "_" + ts + ".csv");
}

void MorsLog::init()
{
    bool expected = false;
    if (!g_initStatus.compare_exchange_strong(expected, true))
        return;

    // Block performance CSV
    blockPerfStream = std::make_unique<juce::FileOutputStream>(makeFilePath("block_perf"));
    if (blockPerfStream->openedOk())
        blockPerfStream->writeText("timestamp_ms,block_ms,budget_ms,load_pct,bands_on\n",
                                   false, false, nullptr);

    // Onset map CSV
    onsetMapStream = std::make_unique<juce::FileOutputStream>(makeFilePath("onset_map"));
    if (onsetMapStream->openedOk())
        onsetMapStream->writeText("timestamp_ms,band,src_onsets,ref_onsets,dtw_path_len,dtw_final_cost,dtw_avg_cost\n",
                                  false, false, nullptr);

    // Segment map CSV
    segmentMapStream = std::make_unique<juce::FileOutputStream>(makeFilePath("segment_map"));
    if (segmentMapStream->openedOk())
        segmentMapStream->writeText("timestamp_ms,band,seg_index,start_time,end_time,stretch_ratio\n",
                                    false, false, nullptr);
}

void MorsLog::shutdown()
{
    if (!g_initStatus.exchange(false))
        return;

    blockPerfStream.reset();
    onsetMapStream.reset();
    segmentMapStream.reset();
}

void MorsLog::logBlockPerf(double blockMs, double budgetMs,
                               double loadPct, bool bandsOn) noexcept
{
    if (!g_initStatus.load() || !blockPerfStream || !blockPerfStream->openedOk())
        return;

    const auto now = juce::Time::currentTimeMillis();
    const auto row = juce::String(now) + ","
                   + juce::String(blockMs, 4) + ","
                   + juce::String(budgetMs, 4) + ","
                   + juce::String(loadPct,  2) + ","
                   + juce::String(bandsOn ? 1 : 0) + "\n";

    blockPerfStream->writeText(row, false, false, nullptr);
}

void MorsLog::logOnsetMap(int srcOnsetCount, int refOnsetCount,
                              int dtwPathLength, double dtwFinalCost,
                              double avgCost,    int band)
{
    if (!g_initStatus.load() || !onsetMapStream || !onsetMapStream->openedOk())
        return;

    const auto now = juce::Time::currentTimeMillis();
    const auto row = juce::String(now)           + ","
                   + juce::String(band)           + ","
                   + juce::String(srcOnsetCount)  + ","
                   + juce::String(refOnsetCount)  + ","
                   + juce::String(dtwPathLength)  + ","
                   + juce::String(dtwFinalCost, 6) + ","
                   + juce::String(avgCost,      6) + "\n";

    onsetMapStream->writeText(row, false, false, nullptr);
}

void MorsLog::logSegment(int segIndex, double startTime,
                             double endTime, float stretchRatio, int band)
{
    if (!g_initStatus.load() || !segmentMapStream || !segmentMapStream->openedOk())
        return;

    const auto now = juce::Time::currentTimeMillis();
    const auto row = juce::String(now)              + ","
                   + juce::String(band)              + ","
                   + juce::String(segIndex)          + ","
                   + juce::String(startTime,  6)     + ","
                   + juce::String(endTime,    6)     + ","
                   + juce::String(stretchRatio, 4)   + "\n";

    segmentMapStream->writeText(row, false, false, nullptr);
}
