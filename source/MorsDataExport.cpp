#include "MorsDataExport.h"
#include "MorsStretch.h"
#include "MorsMorph.h"
#include "MorsDebug.h"
#include <array>
#include <cstring>

//==============================================================================
// Static member definitions
//==============================================================================
juce::AbstractFifo                                               MorsDataExport::s_perfFifo   { MorsDataExport::kPerfCapacity };
std::array<MorsDataExport::PerfSample, MorsDataExport::kPerfCapacity> MorsDataExport::s_perfBuf {};
juce::File                                                       MorsDataExport::s_sessionDir {};
bool                                                             MorsDataExport::s_initialized { false };

//==============================================================================
// Directory helpers (mirrors MorsDebug pattern)
//==============================================================================
static juce::File findDirUpwards(juce::File start, const juce::String& name)
{
    auto dir = start;
    while (dir.exists() && !dir.isRoot())
    {
        if (dir.getFileName().equalsIgnoreCase(name))
            return dir;
        dir = dir.getParentDirectory();
    }
    return {};
}

//==============================================================================
// Helper: write one float vector as frame-index,value csv
//==============================================================================
void MorsDataExport::writeVectorCsv(const juce::File& dir,
                                    const juce::String& filename,
                                    const std::vector<float>& values)
{
    juce::FileOutputStream out(dir.getChildFile(filename));
    if (!out.openedOk()) return;

    out << "bin,value\n";
    for (size_t i = 0; i < values.size(); ++i)
        out << (int) i << "," << juce::String(values[i], 9) << "\n";
}

//==============================================================================
// Export per-frame morph debug arrays to a subfolder
//==============================================================================
void MorsDataExport::writeMorphDebugFrames(const std::vector<MorsMorph::DebugFrame>& frames)
{
    if (!s_sessionDir.exists() || frames.empty())
        return;

    const auto root = s_sessionDir.getChildFile("morph_debug");
    root.createDirectory();

    for (const auto& f : frames)
    {
        const auto frameDir = root.getChildFile("frame_" + juce::String(f.frameIndex));
        frameDir.createDirectory();

        {
            juce::FileOutputStream meta(frameDir.getChildFile("meta.csv"));
            if (meta.openedOk())
            {
                meta << "frame_index,normalized_pos\n";
                meta << f.frameIndex << "," << juce::String(f.normalizedPos, 9) << "\n";
            }
        }

        writeVectorCsv(frameDir, "source_mag.csv", f.sourceMag);
        writeVectorCsv(frameDir, "source_phase.csv", f.sourcePhase);
        writeVectorCsv(frameDir, "source_envelope.csv", f.sourceEnvelope);
        writeVectorCsv(frameDir, "target_envelope_interpolated.csv", f.targetEnvelopeInterpolated);
        writeVectorCsv(frameDir, "target_envelope_normalized.csv", f.targetEnvelopeNormalized);
        writeVectorCsv(frameDir, "source_structure.csv", f.sourceStructure);
        writeVectorCsv(frameDir, "morphed_magnitude.csv", f.morphedMagnitude);
    }

    LOGI("EXPORT", "Morph debug frames written to: " + root.getFullPathName());
}


juce::File MorsDataExport::findDebugDirectory()
{
    const auto appFile  = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    const auto morsRoot = findDirUpwards(appFile.getParentDirectory(), "mors");
    auto dir = (morsRoot.exists() ? morsRoot : appFile.getParentDirectory()).getChildFile("debug");
    dir.createDirectory();
    return dir;
}

//==============================================================================
void MorsDataExport::init(const juce::File& overrideDir)
{
    if (s_initialized) return;
    s_initialized = true;

    if (overrideDir.exists())
        overrideDir.createDirectory();
    else
        findDebugDirectory();   // ensure debug/ exists
}

//==============================================================================
void MorsDataExport::beginSession(const juce::String& label)
{
    const auto ts   = juce::Time::getCurrentTime().formatted("mors_export_%Y_%m_%d_%Hh%Mm%Ss");
    const auto name = label.isEmpty() ? ts : ts + "_" + label;
    s_sessionDir    = findDebugDirectory().getChildFile(name);
    s_sessionDir.createDirectory();
    LOGI("EXPORT", "Session folder: " + s_sessionDir.getFullPathName());
}

//==============================================================================
juce::File MorsDataExport::getSessionDir() { return s_sessionDir; }

//==============================================================================
void MorsDataExport::writeOnsets(const juce::String& role,
                                  const std::vector<double>& times)
{
    if (!s_sessionDir.exists()) return;
    juce::FileOutputStream out(s_sessionDir.getChildFile("onsets_" + role + ".csv"));
    if (!out.openedOk()) return;

    out << "index,time_s\n";
    for (size_t i = 0; i < times.size(); ++i)
        out << (int)i << "," << juce::String(times[i], 6) << "\n";
}

//==============================================================================
void MorsDataExport::writeNovelty(const juce::String& role,
                                   const std::vector<float>& novelty,
                                   double fsFeature)
{
    if (!s_sessionDir.exists()) return;
    juce::FileOutputStream out(s_sessionDir.getChildFile("novelty_" + role + ".csv"));
    if (!out.openedOk()) return;

    out << "frame,time_s,novelty\n";
    for (size_t i = 0; i < novelty.size(); ++i)
    {
        const double t = (fsFeature > 0.0) ? (double)i / fsFeature : 0.0;
        out << (int)i << ","
            << juce::String(t, 6) << ","
            << juce::String(novelty[i], 6) << "\n";
    }
}

//==============================================================================
// Helper: write a 2-D double matrix as CSV (no header row)
//==============================================================================
static void writeMatrix(const juce::File& dir,
                        const juce::String& filename,
                        const std::vector<std::vector<double>>& M)
{
    juce::FileOutputStream out(dir.getChildFile(filename));
    if (!out.openedOk()) return;

    for (const auto& row : M)
    {
        for (size_t j = 0; j < row.size(); ++j)
        {
            out << juce::String(row[j], 6);
            if (j + 1 < row.size()) out << ",";
        }
        out << "\n";
    }
}

//==============================================================================
void MorsDataExport::writeDTW(const std::vector<std::vector<double>>& costMatrix,
                               const std::vector<std::vector<double>>& accumMatrix,
                               const std::vector<std::vector<int64_t>>& path,
                               int    sourceOnsetCount,
                               int    refOnsetCount,
                               double finalCost,
                               double avgCost)
{
    if (!s_sessionDir.exists()) return;

    writeMatrix(s_sessionDir, "dtw_cost_matrix.csv",  costMatrix);
    writeMatrix(s_sessionDir, "dtw_accum_matrix.csv", accumMatrix);

    // Warping path
    {
        juce::FileOutputStream out(s_sessionDir.getChildFile("dtw_path.csv"));
        if (out.openedOk())
        {
            out << "source_idx,ref_idx\n";
            for (const auto& p : path)
                if (p.size() >= 2)
                    out << p[0] << "," << p[1] << "\n";
        }
    }

    // Summary stats
    {
        juce::FileOutputStream out(s_sessionDir.getChildFile("dtw_stats.csv"));
        if (out.openedOk())
        {
            out << "path_length,source_onset_count,ref_onset_count,final_cost,avg_cost\n";
            out << (int)path.size() << ","
                << sourceOnsetCount << ","
                << refOnsetCount << ","
                << juce::String(finalCost, 6) << ","
                << juce::String(avgCost, 6) << "\n";
        }
    }
}

//==============================================================================
void MorsDataExport::writeSegments(const std::vector<SegmentMapping>& segments)
{
    if (!s_sessionDir.exists()) return;
    juce::FileOutputStream out(s_sessionDir.getChildFile("segments.csv"));
    if (!out.openedOk()) return;

    out << "index,start_s,end_s,duration_s,stretch_ratio\n";
    for (size_t i = 0; i < segments.size(); ++i)
    {
        const auto& s   = segments[i];
        const double dur = s.endTime - s.startTime;
        out << (int)i << ","
            << juce::String(s.startTime,    6) << ","
            << juce::String(s.endTime,      6) << ","
            << juce::String(dur,            6) << ","
            << juce::String(s.stretchRatio, 6) << "\n";
    }
}

//==============================================================================
// RT-safe perf push (audio thread — no alloc, no lock, no I/O)
//==============================================================================
void MorsDataExport::pushPerfSample(int    blockIdx,
                                     int    blockSize,
                                     double sampleRate,
                                     const char* algo,
                                     double cpuMs) noexcept
{
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    s_perfFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 <= 0) return;   // buffer full — drop sample

    auto& slot      = s_perfBuf[(size_t)s1];
    slot.blockIdx   = blockIdx;
    slot.blockSize  = blockSize;
    slot.sampleRate = sampleRate;
    slot.cpuMs      = cpuMs;
    std::strncpy(slot.algo, algo != nullptr ? algo : "?", sizeof(slot.algo) - 1);
    slot.algo[sizeof(slot.algo) - 1] = '\0';

    s_perfFifo.finishedWrite(1);
}

//==============================================================================
// Flush perf FIFO → performance.csv (append so multiple sessions accumulate)
//==============================================================================
void MorsDataExport::flushPerfLog()
{
    if (!s_sessionDir.exists()) return;

    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    s_perfFifo.prepareToRead(kPerfCapacity, s1, n1, s2, n2);
    const int total = n1 + n2;
    if (total <= 0) return;

    auto f = s_sessionDir.getChildFile("performance.csv");
    const bool needsHeader = !f.existsAsFile();

    juce::FileOutputStream out(f);
    if (!out.openedOk()) { s_perfFifo.finishedRead(total); return; }

    // Seek to end so we always append
    out.setPosition(f.getSize());

    if (needsHeader)
        out << "block_idx,block_size,sample_rate,algo,cpu_ms\n";

    auto flush = [&](int start, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& s = s_perfBuf[(size_t)(start + i)];
            out << s.blockIdx << ","
                << s.blockSize << ","
                << juce::String(s.sampleRate, 1) << ","
                << s.algo << ","
                << juce::String(s.cpuMs, 4) << "\n";
        }
    };

    flush(s1, n1);
    flush(s2, n2);
    s_perfFifo.finishedRead(total);
}


