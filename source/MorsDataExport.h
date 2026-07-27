#pragma once

#include <JuceHeader.h>
#include <vector>
#include <array>
#include "MorsStretch.h"
#include "MorsMorph.h"

class MorsMorph;

class MorsDataExport
{
public:
    static void init(const juce::File& overrideDir = {});
    static void beginSession(const juce::String& label);
    static juce::File getSessionDir();

    static void writeOnsets(const juce::String& role,
                            const std::vector<double>& times);

    static void writeNovelty(const juce::String& role,
                             const std::vector<float>& novelty,
                             double fsFeature);

    static void writeDTW(const std::vector<std::vector<double>>& costMatrix,
                         const std::vector<std::vector<double>>& accumMatrix,
                         const std::vector<std::vector<int64_t>>& path,
                         int sourceOnsetCount,
                         int refOnsetCount,
                         double finalCost,
                         double avgCost);

    static void writeSegments(const std::vector<SegmentMapping>& segments);

    static void writeMorphDebugFrames(const std::vector<MorsMorph::DebugFrame>& frames);
    static void writeVectorCsv(const juce::File& dir,
                               const juce::String& filename,
                               const std::vector<float>& values);

    static void pushPerfSample(int    blockIdx,
                               int    blockSize,
                               double sampleRate,
                               const char* algo,
                               double cpuMs) noexcept;

    static void flushPerfLog();

private:
    static juce::File findDebugDirectory();

    static constexpr int kPerfCapacity = 4096;

    struct PerfSample
    {
        int    blockIdx   = 0;
        int    blockSize  = 0;
        double sampleRate = 0.0;
        char   algo[16]   = {};
        double cpuMs      = 0.0;
    };

    static juce::AbstractFifo s_perfFifo;
    static std::array<PerfSample, kPerfCapacity> s_perfBuf;
    static juce::File s_sessionDir;
    static bool s_initialized;

    MorsDataExport() = delete;
};
