#include <JuceHeader.h>
#include "MorsFile.h"
#include "MorsDebug.h"
#include <vector>

//==============================================================================
// Constructor / Destructor
//==============================================================================
AudioFile::AudioFile() : audioThumbnail(512, formatManager, thumbnailCache)
{
    constexpr const char* kCat = "AUDIO_FILE";
    LOG_SCOPE(kCat);

    PERF_START(audiofile_ctor);

    // Register basic audio formats (WAV, AIFF, etc.)
    formatManager.registerBasicFormats();
    audioThumbnail.addChangeListener(this);

    LOGI(kCat, "AudioFile(): initialized"
               " | thumbnailSourceSamplesPerThumb = 512");

    PERF_END(audiofile_ctor);
}

AudioFile::~AudioFile()
{
    constexpr const char* kCat = "AUDIO_FILE";
    LOG_SCOPE(kCat);

    LOGI(kCat, "AudioFile::~AudioFile(): destroying"
               " | bufferSamples = " + juce::String(fileBuffer.getNumSamples())
               + " | bufferChannels = " + juce::String(fileBuffer.getNumChannels())
               + " | sampleRate = " + juce::String(fileSamplerate, 2));
}

//==============================================================================
// [UTILITY] PATH FINDER
//==============================================================================
juce::File AudioFile::path(const juce::String& folderName)
{
    constexpr const char* kCat = "FILE_PATH";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_path);

    LOGD(kCat, "path(): begin | requested folder = " + folderName);

    // Walk up from the app bundle to find the "mors" project root
    auto dir = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                          .getParentDirectory();

    juce::String startingDir = dir.getFullPathName();
    bool foundRoot = false;

    while (dir.exists() && !dir.isRoot())
    {
        if (dir.getFileName().equalsIgnoreCase("mors"))
        {
            foundRoot = true;
            break;
        }

        dir = dir.getParentDirectory();
    }

    if (!foundRoot)
    {
        LOGW(kCat, "path(): project root 'mors' not found while traversing parents"
                   " | startDir = " + startingDir
                   + " | using final dir = " + dir.getFullPathName());
    }

    auto outputDir = dir.getChildFile(folderName);
    const bool createdOk = outputDir.createDirectory();

    if (!createdOk && !outputDir.exists())
    {
        LOGE(kCat, "path(): failed to create output directory"
                   " | path = " + outputDir.getFullPathName());
    }
    else
    {
        LOGD(kCat, "path(): resolved"
                   " | rootFound = " + juce::String(foundRoot ? "true" : "false")
                   + " | outputDir = " + outputDir.getFullPathName());
    }

    PERF_END(audiofile_path);
    return outputDir;
}

//==============================================================================
// [UTILITY] OPEN FILE
//==============================================================================
void AudioFile::open(LoadCallback callback)
{
    constexpr const char* kCat = "FILE_OPEN";
    LOG_SCOPE(kCat);

    PERF_START(audiofile_open);

    onsetTimes.clear();
    gridTimes.clear();

    LOGI(kCat, "open(): preparing async file chooser");

    // File chooser flags
    using fc = juce::FileBrowserComponent;
    const int flags = fc::canSelectFiles;

    // Launch an async file chooser
    fileChooser = std::make_unique<juce::FileChooser>("Open audio file...",
                                                      juce::File{},
                                                      "*");

    // Capture 'this' and the callback in the lambda
    fileChooser->launchAsync(flags, [this, callback](const juce::FileChooser& fc)
    {
        constexpr const char* kAsyncCat = "FILE_OPEN";
        LOG_SCOPE(kAsyncCat);
        PERF_START(audiofile_open_async);

        LOGD(kAsyncCat, "open(): chooser returned");

        // Retrieve the selected file
        auto file = fc.getResult();
        if (file == juce::File{})
        {
            LOGI(kAsyncCat, "open(): user cancelled file selection");
            PERF_END(audiofile_open_async);
            return;
        }

        LOGI(kAsyncCat, "open(): selected file"
                        " | path = " + file.getFullPathName()
                        + " | exists = " + juce::String(file.existsAsFile() ? "true" : "false"));

        // Create a reader for the selected file
        std::unique_ptr<juce::AudioFormatReader> fileTempReader(
            formatManager.createReaderFor(file));

        if (!fileTempReader)
        {
            LOGE(kAsyncCat, "open(): failed to create reader"
                            " | path = " + file.getFullPathName());
            PERF_END(audiofile_open_async);
            return;
        }

        // Cache reference sample rate and compute total duration
        fileSamplerate = fileTempReader->sampleRate;
        const int totalSamples = (int) fileTempReader->lengthInSamples;
        const int numChannels  = (int) fileTempReader->numChannels;

        fileLengthSeconds = (fileSamplerate > 0.0)
            ? (double) totalSamples / fileSamplerate
            : 0.0;

        LOGI(kAsyncCat, "open(): reader created"
                        " | sampleRate = " + juce::String(fileSamplerate, 2)
                        + " | channels = " + juce::String(numChannels)
                        + " | totalSamples = " + juce::String(totalSamples)
                        + " | fileLengthSec = " + juce::String(fileLengthSeconds, 5));

        if (fileSamplerate <= 0.0 || totalSamples <= 0 || numChannels <= 0)
        {
            LOGW(kAsyncCat, "open(): suspicious file metadata"
                            " | sampleRate = " + juce::String(fileSamplerate, 2)
                            + " | channels = " + juce::String(numChannels)
                            + " | totalSamples = " + juce::String(totalSamples));
        }

        // Allocate and read the entire reference file into memory
        fileBuffer.setSize(numChannels, totalSamples);
        fileBuffer.clear();

        const bool readOk = fileTempReader->read(&fileBuffer,
                                                 0,                 // dest start sample
                                                 totalSamples,      // number of samples
                                                 0,                 // reader start sample
                                                 true,              // fill left channels
                                                 true);             // fill right channels

        if (!readOk)
        {
            LOGE(kAsyncCat, "open(): failed to read audio data into buffer"
                            " | path = " + file.getFullPathName());
            PERF_END(audiofile_open_async);
            return;
        }

        LOGD(kAsyncCat, "open(): buffer populated"
                        " | channels = " + juce::String(fileBuffer.getNumChannels())
                        + " | samples = " + juce::String(fileBuffer.getNumSamples()));

        // Wrap the reader in an AudioFormatReaderSource (takes ownership)
        fileReader = std::make_unique<juce::AudioFormatReaderSource>(
            fileTempReader.release(), true);

        if (!fileReader)
        {
            LOGE(kAsyncCat, "open(): failed to create AudioFormatReaderSource");
            PERF_END(audiofile_open_async);
            return;
        }

        onsetTimes.clear();
        gridTimes.clear();

        LOGD(kAsyncCat, "open(): analysis state reset"
                        " | onsetCount = " + juce::String((int) onsetTimes.size())
                        + " | gridCount = " + juce::String((int) gridTimes.size()));

        // Call the callback with the loaded data
        if (callback)
        {
            LOGD(kAsyncCat, "open(): invoking load callback");
            callback(file,
                     fileBuffer,
                     fileSamplerate,
                     std::move(fileReader));
            LOGD(kAsyncCat, "open(): load callback completed");
        }
        else
        {
            LOGW(kAsyncCat, "open(): no callback supplied");
        }

        // Update thumbnail
        audioThumbnail.setSource(new juce::FileInputSource(file));
        LOGD(kAsyncCat, "open(): thumbnail source updated"
                        " | path = " + file.getFullPathName());

        PERF_END(audiofile_open_async);
    });

    PERF_END(audiofile_open);
}

//==============================================================================
// [UTILITY] LOAD FROM FILE (no dialog — mirrors open() internals)
//==============================================================================
bool AudioFile::loadFromFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (!reader)
        return false;

    const int    totalSamples = (int) reader->lengthInSamples;
    const int    numChannels  = (int) reader->numChannels;
    fileSamplerate            = reader->sampleRate;
    fileLengthSeconds         = (fileSamplerate > 0.0)
                                    ? (double) totalSamples / fileSamplerate
                                    : 0.0;

    if (fileSamplerate <= 0.0 || totalSamples <= 0 || numChannels <= 0)
        return false;

    fileBuffer.setSize(numChannels, totalSamples);
    fileBuffer.clear();
    reader->read(&fileBuffer, 0, totalSamples, 0, true, true);

    // Wrap in a source so isLoaded() stays consistent with the open() path
    fileReader = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);

    onsetTimes.clear();
    gridTimes.clear();

    audioThumbnail.setSource(new juce::FileInputSource(file));

    if (thumbnailChanged)
        thumbnailChanged();

    return isLoaded();
}

//==============================================================================
// [UTILITY] CHECK IF LOADED
//==============================================================================
bool AudioFile::isLoaded() const
{
    return fileBuffer.getNumSamples() > 0;
}

void AudioFile::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    constexpr const char* kCat = "THUMBNAIL";
    LOG_SCOPE(kCat);

    if (source == &audioThumbnail)
    {
        LOGD(kCat, "changeListenerCallback(): thumbnail change received");

        if (thumbnailChanged)
        {
            LOGD(kCat, "changeListenerCallback(): invoking thumbnailChanged callback");
            thumbnailChanged();
        }
        else
        {
            LOGW(kCat, "changeListenerCallback(): thumbnailChanged callback not set");
        }
    }
}

//==============================================================================
// [UTILITY] SETUP OUTPUT FILE FOR RECORDING
//==============================================================================
bool AudioFile::setup(const juce::File& outputFile,
                      int sampleRate,
                      int numChannels)
{
    constexpr const char* kCat = "FILE_RECORD";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_setup);

    LOGI(kCat, "setup(): begin"
               " | outputFile = " + outputFile.getFullPathName()
               + " | sampleRate = " + juce::String(sampleRate)
               + " | numChannels = " + juce::String(numChannels));

    if (sampleRate <= 0 || numChannels <= 0)
    {
        LOGE(kCat, "setup(): invalid writer configuration"
                   " | sampleRate = " + juce::String(sampleRate)
                   + " | numChannels = " + juce::String(numChannels));
        PERF_END(audiofile_setup);
        return false;
    }

    /* ----------------- DELETE EXISTING FILE ----------------- */

    // Remove old file if it exists
    if (outputFile.existsAsFile())
    {
        const bool deleted = outputFile.deleteFile();

        if (!deleted)
        {
            LOGW(kCat, "setup(): failed to delete existing file"
                       " | path = " + outputFile.getFullPathName());
        }
        else
        {
            LOGD(kCat, "setup(): deleted existing file"
                       " | path = " + outputFile.getFullPathName());
        }
    }

    /* ----------------- CREATE OUTPUT STREAM ----------------- */

    // Create a stream to write data to the file
    fileStream = outputFile.createOutputStream();
    if (fileStream == nullptr)
    {
        LOGE(kCat, "setup(): failed to create output stream"
                   " | path = " + outputFile.getFullPathName());
        PERF_END(audiofile_setup);
        return false;
    }

    LOGD(kCat, "setup(): output stream created");

    /* ----------------- CREATE WAV WRITER ----------------- */

    // Setup WAV format writer with specified parameters
    juce::WavAudioFormat wavFormat;
    writer.reset(wavFormat.createWriterFor(
        fileStream.get(),
        sampleRate,
        (unsigned int) numChannels,
        16,      // Bits per sample
        {},      // No metadata
        0));     // Default quality

    /* ----------------- VERIFY WRITER CREATION ----------------- */

    // Check that writer was created successfully
    if (writer != nullptr)
    {
        // Transfer ownership of fileStream to writer
        fileStream.release();

        LOGI(kCat, "setup(): writer created successfully"
                   " | bitsPerSample = 16"
                   " | path = " + outputFile.getFullPathName());

        PERF_END(audiofile_setup);
        return true;
    }

    LOGE(kCat, "setup(): failed to create WAV writer"
               " | path = " + outputFile.getFullPathName());

    PERF_END(audiofile_setup);
    return false;
}

//==============================================================================
// [UTILITY] WRITE AUDIO BUFFER TO FILE
//==============================================================================
void AudioFile::record(const juce::AudioBuffer<float>& buffer)
{
    constexpr const char* kCat = "FILE_RECORD";
    LOG_SCOPE(kCat);

    if (writer != nullptr)
    {
        LOGD(kCat, "record(): writing buffer"
                   " | channels = " + juce::String(buffer.getNumChannels())
                   + " | samples = " + juce::String(buffer.getNumSamples()));

        // Write entire buffer to file
        writer->writeFromAudioSampleBuffer(
            buffer,
            0,                          // Start at beginning of buffer
            buffer.getNumSamples());    // Write all samples
    }
    else
    {
        LOGW(kCat, "record(): writer not initialized | skipping write"
                   " | samples = " + juce::String(buffer.getNumSamples()));
    }
}

//==============================================================================
// [UTILITY] CLOSE AND FINALIZE OUTPUT FILE
//==============================================================================
void AudioFile::close()
{
    constexpr const char* kCat = "FILE_RECORD";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_close);

    LOGI(kCat, "close(): begin");

    /* ----------------- RELEASE WRITER ----------------- */

    // Close and release the audio writer
    if (writer != nullptr)
    {
        writer.reset();  // Destructor finalizes WAV file
        LOGD(kCat, "close(): writer released");
    }
    else
    {
        LOGD(kCat, "close(): writer already null");
    }

    /* ----------------- FLUSH AND CLOSE STREAM ----------------- */

    // Ensure all data is written and file is closed
    if (fileStream != nullptr)
    {
        fileStream->flush();  // Write any remaining buffered data
        fileStream.reset();   // Close the file stream
        LOGD(kCat, "close(): stream flushed and released");
    }
    else
    {
        LOGD(kCat, "close(): file stream already null");
    }

    PERF_END(audiofile_close);
}

//==============================================================================
// [ANALYSIS] ANALYZE ONSETS
//==============================================================================
void AudioFile::analyze(juce::String method,
                        double onsetThreshold,
                        double onsetSilence,
                        double onsetMinMs)
{
    constexpr const char* kCat = "ANALYZE_FILE";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_analyze);

    const int totalSamples   = fileBuffer.getNumSamples();
    const int totalTimeSec   = (fileSamplerate > 0.0) ? (int) (totalSamples / fileSamplerate) : 0;
    const float* mono        = (totalSamples > 0 && fileBuffer.getNumChannels() > 0)
                                 ? fileBuffer.getReadPointer(0)
                                 : nullptr;

    // Window size = 1/64th of the file duration, clamped to a sensible range
    // Expressed in milliseconds for the peak picker
    const float windowMs = (float) juce::jlimit(50.0, 500.0, (totalTimeSec / 64.0) * 1000.0);

    LOGI(kCat, "analyze(): begin"
               " | method = " + method
               + " | totalSamples = " + juce::String(totalSamples)
               + " | channels = " + juce::String(fileBuffer.getNumChannels())
               + " | sampleRate = " + juce::String(fileSamplerate, 2)
               + " | onsetThreshold = " + juce::String(onsetThreshold, 4)
               + " | onsetSilence = " + juce::String(onsetSilence, 4)
               + " | onsetMinMs = " + juce::String(onsetMinMs, 2)
               + " | windowMs = " + juce::String(windowMs, 2));

    onsetTimes.clear();

    if (fileBuffer.getNumSamples() <= 0 || fileSamplerate <= 0.0 || mono == nullptr)
    {
        LOGW(kCat, "analyze(): invalid analysis state"
                   " | samples = " + juce::String(fileBuffer.getNumSamples())
                   + " | sampleRate = " + juce::String(fileSamplerate, 2)
                   + " | monoPtrNull = " + juce::String(mono == nullptr ? "true" : "false"));
        PERF_END(audiofile_analyze);
        return;
    }
    else
    {
        LOGD(kCat, "analyze(): using custom novelty/peak onset method");

        // Onset Detection & Extraction - Custom Method
        // --------------------------------------------------------------------------
        std::vector<float> bufferVector(mono, mono + totalSamples);

        auto [noveltyEnergy, fsFeature] = onset.novelty(bufferVector, fileSamplerate, 1024, 256);

        LOGD(kCat, "analyze(): novelty computed"
                   " | noveltyFrames = " + juce::String((int) noveltyEnergy.size())
                   + " | fsFeature = " + juce::String(fsFeature, 4));

        onsetTimes = onset.peak(noveltyEnergy,  // activations
                                fsFeature,      // fsFeature
                                0.125,          // threshold (0.0 - 1.0)
                                false,          // score (bool)
                                true,           // minMsGate (bool)
                                50,             // minMsValue
                                windowMs,       // preAvg
                                windowMs,       // postAvg
                                windowMs,       // preMax
                                windowMs);      // postMax

        LOGD(kCat, "analyze(): custom peak picking complete"
                   " | detectedOnsetsBeforeEOF = " + juce::String((int) onsetTimes.size()));
    }

    // Force end of file to be onset
    const double fileLength = getLengthSeconds();
    const bool appendedEOF = (onsetTimes.empty() || onsetTimes.back() < fileLength - 0.01);

    if (appendedEOF)
        onsetTimes.push_back(fileLength);

    LOGI(kCat, "analyze(): onset extraction complete"
               " | onsetCount = " + juce::String((int) onsetTimes.size())
               + " | fileLengthSec = " + juce::String(fileLength, 5)
               + " | appendedEOFMarker = " + juce::String(appendedEOF ? "true" : "false"));

    juce::String s;
    for (int i = 0; i < (int) onsetTimes.size(); ++i)
    {
        s << "onset: " << i << "\t|\t"
          << juce::String::formatted("%-5s%.5f", "pos: ", onsetTimes[(size_t) i]) << "\n";
    }
    LOGD(kCat, s);

    // Tempo Detection
    //--------------------------------------------------------------------------
    tempoDetection.prepare(fileSamplerate, 512, 20, 240);
    tempoDetection.processBlock(mono, totalSamples);

    LOGI("TEMPO", "Detected BPM: " + juce::String(tempoDetection.getEstimatedBpm(), 1)
                 + " | totalSamples = " + juce::String(totalSamples)
                 + " | sampleRate = " + juce::String(fileSamplerate, 2));

    PERF_END(audiofile_analyze);
}

//==============================================================================
// [PROCESSING] QUANTIZE
//==============================================================================
void AudioFile::quantize(double bpm,
                         double subdivision,
                         float strength)
{
    constexpr const char* kCat = "QUANTIZE_GRID";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_quantize);

    LOGI(kCat, "quantize(): begin"
               " | bpm = " + juce::String(bpm, 4)
               + " | subdivision = " + juce::String(subdivision, 4)
               + " | strength = " + juce::String(strength, 2)
               + " | onsetCountIn = " + juce::String((int) onsetTimes.size()));

    //--------------------------------------------------------------------------
    // BPM Grid Generation
    //--------------------------------------------------------------------------
    gridTimes.clear();

    if (fileSamplerate > 0.0 && bpm > 0.0 && subdivision > 0 && fileBuffer.getNumSamples() > 0)
    {
        const double gridSec = 60.0 / (bpm * (double) subdivision);
        const double totalTimeSec = (double) fileBuffer.getNumSamples() / fileSamplerate;
        const double anchor = 0.0;

        for (double t = anchor; t <= totalTimeSec; t += gridSec)
            gridTimes.push_back(t);

        LOGD(kCat, "quantize(): grid generated"
                   " | gridSec = " + juce::String(gridSec, 6)
                   + " | totalTimeSec = " + juce::String(totalTimeSec, 5)
                   + " | gridCount = " + juce::String((int) gridTimes.size()));
    }
    else
    {
        LOGW(kCat, "quantize(): unable to generate grid"
                   " | sampleRate = " + juce::String(fileSamplerate, 2)
                   + " | bpm = " + juce::String(bpm, 4)
                   + " | subdivision = " + juce::String(subdivision, 4)
                   + " | totalSamples = " + juce::String(fileBuffer.getNumSamples()));
    }

    //--------------------------------------------------------------------------
    // Apply Quantization (if enabled)
    //--------------------------------------------------------------------------
    if (!gridTimes.empty())
    {
        if (onsetTimes.empty() || strength <= 0.0f)
        {
            LOGW(kCat, "quantize(): conditions not met"
                       " | onsetCount = " + juce::String((int) onsetTimes.size())
                       + " | gridCount = " + juce::String((int) gridTimes.size())
                       + " | strength = " + juce::String(strength, 2));
            PERF_END(audiofile_quantize);
            return;
        }

        std::vector<double> quantized;
        quantized.reserve(onsetTimes.size());

        const float amount = juce::jlimit(0.0f, 1.0f, strength);
        double totalShiftAbs = 0.0;
        double maxShiftAbs = 0.0;

        for (double onset : onsetTimes)
        {
            // Find nearest grid point
            double nearestGrid = gridTimes[0];
            double minDist = std::abs(onset - gridTimes[0]);

            for (double gridPoint : gridTimes)
            {
                double dist = std::abs(onset - gridPoint);
                if (dist < minDist)
                {
                    minDist = dist;
                    nearestGrid = gridPoint;
                }
            }

            // Blend between original onset and grid point based on strength
            double quantizedOnset = onset + (nearestGrid - onset) * amount;
            const double shiftAbs = std::abs(quantizedOnset - onset);

            totalShiftAbs += shiftAbs;
            maxShiftAbs = std::max(maxShiftAbs, shiftAbs);

            quantized.push_back(quantizedOnset);
        }

        onsetTimes = quantized;

        const double meanShiftAbs = onsetTimes.empty() ? 0.0 : totalShiftAbs / (double) onsetTimes.size();

        LOGI(kCat, "quantize(): complete"
                   " | amount = " + juce::String(amount, 4)
                   + " | onsetCountOut = " + juce::String((int) onsetTimes.size())
                   + " | meanAbsShiftSec = " + juce::String(meanShiftAbs, 6)
                   + " | maxAbsShiftSec = " + juce::String(maxShiftAbs, 6));
    }

    //--------------------------------------------------------------------------
    // Log
    //--------------------------------------------------------------------------
    juce::String s;
    for (int i = 0; i < (int) onsetTimes.size(); ++i)
    {
        s << "quantized: " << i << "\t|\t"
          << juce::String::formatted("%-5s%.5f", "pos: ", onsetTimes[(size_t) i]) << "\n";
    }
    LOGD(kCat, s);

    PERF_END(audiofile_quantize);
}

//==============================================================================
// [ANALYSIS] ANALYZE BANDS (4-Band Crossover)
//==============================================================================
void AudioFile::analyzeBands(float crossover1,
                             float crossover2,
                             float crossover3,
                             double onsetThreshold,
                             double onsetSilence,
                             double onsetMinMs)
{
    // LOGGING
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    constexpr const char* kCat = "ANALYZE_BANDS";
    LOG_SCOPE(kCat);
    PERF_START(audiofile_analyze_bands);

    LOGI(kCat, "analyzeBands(): begin"
               " | crossover1 = " + juce::String(crossover1, 2)
               + " | crossover2 = " + juce::String(crossover2, 2)
               + " | crossover3 = " + juce::String(crossover3, 2)
               + " | onsetThreshold = " + juce::String(onsetThreshold, 4)
               + " | onsetSilence = " + juce::String(onsetSilence, 4)
               + " | onsetMinMs = " + juce::String(onsetMinMs, 2)
               + " | sampleRate = " + juce::String(fileSamplerate, 2)
               + " | totalSamples = " + juce::String(fileBuffer.getNumSamples()));

    if (!isLoaded() || fileSamplerate <= 0.0)
    {
        LOGW(kCat, "analyzeBands(): invalid state"
                   " | loaded = " + juce::String(isLoaded() ? "true" : "false")
                   + " | sampleRate = " + juce::String(fileSamplerate, 2));
        PERF_END(audiofile_analyze_bands);
        return;
    }

    if (!(crossover1 > 0.0f && crossover2 > crossover1 && crossover3 > crossover2))
    {
        LOGW(kCat, "analyzeBands(): crossover configuration may be invalid"
                   " | c1 = " + juce::String(crossover1, 2)
                   + " | c2 = " + juce::String(crossover2, 2)
                   + " | c3 = " + juce::String(crossover3, 2));
    }
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    
    const int numSamples = fileBuffer.getNumSamples();

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = fileSamplerate;
    spec.maximumBlockSize = (juce::uint32) numSamples;
    spec.numChannels      = 1;

    // Work on mono
    juce::AudioBuffer<float> monoBuffer(1, numSamples);
    monoBuffer.copyFrom(0, 0, fileBuffer, 0, 0, numSamples);

    // Helper: apply an LR filter to a buffer and return the result
    using LRType = juce::dsp::LinkwitzRileyFilterType;
    auto applyFilter = [&](const juce::AudioBuffer<float>& input,
                           LRType type,
                           float freq) -> juce::AudioBuffer<float>
    {
        juce::dsp::LinkwitzRileyFilter<float> filter;
        filter.setType(type);
        filter.setCutoffFrequency(freq);
        filter.prepare(spec);

        juce::AudioBuffer<float> output(1, numSamples);
        output.copyFrom(0, 0, input, 0, 0, numSamples);

        juce::dsp::AudioBlock<float> block(output);
        filter.process(juce::dsp::ProcessContextReplacing<float>(block));

        return output;
    };

    // Serial 4-band split:
    //  band1    = LP(f1)
    //  midHigh  = HP(f1)  →  band2 = LP(f2),  high = HP(f2)
    //  high              →  band3 = LP(f3),  band4 = HP(f3)
    auto band1Buffer    = applyFilter(monoBuffer,    LRType::lowpass,  crossover1);
    auto midHighBuffer  = applyFilter(monoBuffer,    LRType::highpass, crossover1);
    auto band2Buffer    = applyFilter(midHighBuffer, LRType::lowpass,  crossover2);
    auto highBuffer     = applyFilter(midHighBuffer, LRType::highpass, crossover2);
    auto band3Buffer    = applyFilter(highBuffer,    LRType::lowpass,  crossover3);
    auto band4Buffer    = applyFilter(highBuffer,    LRType::highpass, crossover3);

    LOGD(kCat, "analyzeBands(): band split complete");

    const std::array<juce::AudioBuffer<float>*, 4> bands = {
        &band1Buffer, &band2Buffer, &band3Buffer, &band4Buffer
    };

    const int totalTimeSec   = numSamples / (int) fileSamplerate;
    const float windowMs     = (float) juce::jlimit(50.0, 500.0, (totalTimeSec / 64.0) * 1000.0);
    const double fileLength  = getLengthSeconds();

    for (int b = 0; b < 4; ++b)
    {
        bandOnsetTimes[b].clear();

        const float* mono = bands[b]->getReadPointer(0);
        std::vector<float> bufVec(mono, mono + numSamples);

        auto [noveltyEnergy, fsFeature] = onset.novelty(bufVec, fileSamplerate, 1024, 256);

        bandOnsetTimes[b] = onset.peak(noveltyEnergy,
                                       fsFeature,
                                       (float) onsetThreshold,
                                       false,
                                       true,
                                       (int) onsetMinMs,
                                       windowMs,
                                       windowMs,
                                       windowMs,
                                       windowMs);

        const bool appendedEOF = (bandOnsetTimes[b].empty()
                                  || bandOnsetTimes[b].back() < fileLength - 0.01);

        if (appendedEOF)
            bandOnsetTimes[b].push_back(fileLength);

        LOGI(kCat, "analyzeBands(): band analyzed"
                   " | band = " + juce::String(b)
                   + " | noveltyFrames = " + juce::String((int) noveltyEnergy.size())
                   + " | fsFeature = " + juce::String(fsFeature, 4)
                   + " | onsetCount = " + juce::String((int) bandOnsetTimes[b].size())
                   + " | appendedEOFMarker = " + juce::String(appendedEOF ? "true" : "false"));

        juce::String s;
        for (int i = 0; i < (int) bandOnsetTimes[b].size(); ++i)
        {
            s << "band " << b << " onset " << i << "\t|\tpos: "
              << juce::String(bandOnsetTimes[b][(size_t) i], 5) << "\n";
        }
        LOGD(kCat, s);
    }

    PERF_END(audiofile_analyze_bands);
}
