#include "MorsDebug.h"
#include <array>
#include <atomic>
#include <cstring>

//==============================================================================
// Directory helpers
//==============================================================================
static juce::File findDirectoryUpwards (juce::File startDir,
                                        const juce::String& folderName)
{
    auto dir = startDir;

    while (dir.exists() && ! dir.isRoot())
    {
        if (dir.getFileName().equalsIgnoreCase (folderName))
            return dir;
        dir = dir.getParentDirectory();
    }

    return {};
}

juce::File MorsDebug::findDebugDirectory()
{
    const auto appFile = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    const auto appDir  = appFile.getParentDirectory();

    // Try to find ".../mors" in parent chain; otherwise fall back to appDir
    const auto morsRoot = findDirectoryUpwards (appDir, "mors");

    auto dir = (morsRoot.exists() ? morsRoot : appDir).getChildFile ("debug");
    dir.createDirectory();

    return dir;
}

//==============================================================================
// Make Log File Path
//==============================================================================
juce::File MorsDebug::makeLogFilePath()
{
    // Find the debug directory
    const auto debugDir = findDebugDirectory();
    // Use "mors_" + current timestamp as filename
    const auto name = "mors_" + juce::Time::getCurrentTime().formatted ("%Y_%m_%d_%Hh%Mm%Ss") + ".log";
    // Return log file pathname
    return debugDir.getChildFile (name);
}

//==============================================================================
// Timestamp helper
//==============================================================================
juce::String MorsDebug::currentTimeString()
{
    const auto now = juce::Time::getCurrentTime();
    return now.formatted ("%Y-%m-%d %H:%M:%S")
         + "." + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
}

//==============================================================================
// Logger state
//==============================================================================
static std::unique_ptr<juce::FileLogger> g_fileLogger;                  // FileLogger instance
static juce::File g_logFile;                                            // Current log file path
static std::atomic<bool> g_initStatus { false };                        // Thread-safe "init" flag
static std::atomic<MorsLogLevel> g_minLevel { MorsLogLevel::debug };    // Thread-safe level thres

//==============================================================================
// Realtime queue (fixed-size; lock-free via juce::AbstractFifo)
//==============================================================================
namespace
{
    constexpr int rtCapacity = 2048;        // number of messages
    constexpr int rtMsgLen     = 256;       // max bytes per message (including '\0')

    struct RtMsg
    {
        MorsLogLevel level {};              // severity level of message
        const char* category = nullptr;     // pointer: category label (enum)
        const char* file = nullptr;         // pointer: source file string
        const char* func = nullptr;         // pointer: function name
        int line = 0;                       // line number
        char text[rtMsgLen] {};             // formatted message text
    };

    juce::AbstractFifo  g_fifo  (rtCapacity);               // FIFO controller with preset capacity
    std::array<RtMsg,   (size_t) rtCapacity> g_rtBuffer;    // storage for ring buffer
}

//==============================================================================
// INIT
//==============================================================================
void MorsDebug::init()
{
    // only change the "g_initStatus" flag when "expected" becomes true
    bool expected = false;
    if (! g_initStatus.compare_exchange_strong (expected, true))
        return;

    // set the global log file path
    g_logFile = makeLogFilePath();

    // Create file logger and set as current logger
    g_fileLogger = std::make_unique<juce::FileLogger> (g_logFile, "mors log", 0);
    juce::Logger::setCurrentLogger (g_fileLogger.get());

    // Also visible in debugger output.
    DBG ("[MorsDebug] Log file: " + g_logFile.getFullPathName());
    juce::Logger::writeToLog ("[MorsDebug] Logger initialized: " + g_logFile.getFullPathName());
}

//==============================================================================
// SHUTDOWN
//==============================================================================
void MorsDebug::shutdown()
{
    // Safe to call multiple times.
    if (g_initStatus.exchange (false))
    {
        juce::Logger::writeToLog ("[MorsDebug] Logger shutdown");
        juce::Logger::setCurrentLogger (nullptr);
        g_fileLogger.reset();
    }
}

//==============================================================================
//  SET MIN LEVEL
//==============================================================================
void MorsDebug::setMinLevel (MorsLogLevel level)
{
    g_minLevel.store (level);
}

//==============================================================================
//  SET LEVEL ENABLED (INLINE)
//==============================================================================
static inline bool levelEnabled (MorsLogLevel lvl) noexcept
{
    return (int) lvl >= (int) g_minLevel.load();
}

//==============================================================================
//  SET LEVEL TO STRING (INLINE)
//==============================================================================
static inline const char* levelToString (MorsLogLevel lvl) noexcept
{
    switch (lvl)
    {
        case MorsLogLevel::trace: return "TRACE";
        case MorsLogLevel::debug: return "DEBUG";
        case MorsLogLevel::info:  return "INFO";
        case MorsLogLevel::warn:  return "WARN";
        case MorsLogLevel::error: return "ERROR";
        default:                  return "UNKNOWN";
    }
}

//==============================================================================
//  SET LEVEL TO STRING (INLINE)
//==============================================================================
void MorsDebug::write (MorsLogLevel level,
                       const char* category,
                       const juce::String& message,
                       const char* file,
                       int line,
                       const char* func)
{
    if (! levelEnabled (level))
        return;

    if (! g_initStatus.load())
        init();

    const auto fileLine = juce::File(file).getFileName() + ":" + juce::String(line);
    const auto funcStr  = juce::String(func);
    const auto lineOut  = juce::String::repeatedString(" ", 125) + "\n"
                        + currentTimeString()
                        + juce::String::formatted("%-5s%s", " | ", levelToString(level))
                        + juce::String::formatted("%-5s%s", " | ", (category != nullptr ? category : "UNCAT"))
                        + juce::String::formatted("%-5s%s", " | ", fileLine.toRawUTF8())
                        + juce::String::formatted("%-5s%s", " | ", funcStr.toRawUTF8())
                        + "\n" + juce::String::repeatedString("-", 125) + "\n"
                        + message;

    // Safe print to console if debugger somehow fails
    if (juce::Logger::getCurrentLogger() == nullptr)
        DBG(lineOut);

    // File output (current logger)
    juce::Logger::writeToLog (lineOut);
}

//==============================================================================
//  Push Real-Time
//==============================================================================
void MorsDebug::pushRealtime (MorsLogLevel level,
                              const char* category,
                              const char* message,
                              const char* file,
                              int line,
                              const char* func) noexcept
{
    //  if the level is too low terminate
    if (! levelEnabled (level))
        return;
    
    //  Don't init from the real-time thread
    if (! g_initStatus.load())
        return;
    
    //  init indicies and size params for two contiguous writeable ranges
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    g_fifo.prepareToWrite (1, start1, size1, start2, size2);

    // buffer full -> drop
    if (size1 <= 0)
        return;

    //  init ring buffer with size and starting position
    auto& slot = g_rtBuffer[(size_t) start1];
    
    //  fill metadata (needs to be valid until flushRealTime())
    slot.level = level;
    slot.category = category;
    slot.file = file;
    slot.func = func;
    slot.line = line;

    //  copy the text into the buffer and force a null termination as the last char
    if (message != nullptr)
    {
        std::strncpy (slot.text, message, rtMsgLen - 1);
        slot.text[rtMsgLen - 1] = '\0';
    }
    else
    {
        slot.text[0] = '\0';
    }

    //  status flag indicating the data is now readable
    g_fifo.finishedWrite (1);
}

//==============================================================================
//  Flush Real-Time
//==============================================================================
void MorsDebug::flushRealtime()
{
    //  Immediately terminate if no init
    if (! g_initStatus.load())
        return;
    
    //  same as writing: prepare a ring buffer with pos/size
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    g_fifo.prepareToRead (rtCapacity, start1, size1, start2, size2);

    //  create an array with previous pos/size values
    auto flushRange = [] (int start, int count)
    {
        //  while the buffer can still be filled with data...
        for (int i = 0; i < count; ++i)
        {
            //  let "m" (message) be an alias for the ring buffer...
            const auto& m = g_rtBuffer[(size_t) (start + i)];
            //  exctract the information stored in it with the following formatting
            const auto lineOut =
                currentTimeString()
                + " | " + levelToString (m.level)
                + " | " + (m.category != nullptr ? m.category : "UNCAT")
                + " | " + juce::String (juce::File(m.file).getFileName()) + ":" + juce::String (m.line)
                + " | " + juce::String (m.func)
                + " | " + juce::String (m.text);


            // Safe print to console if debugger somehow fails
            if (juce::Logger::getCurrentLogger() == nullptr)
                DBG(lineOut);
            
            //  send it to the current log file
            juce::Logger::writeToLog (lineOut);
        }
    };

    //  set the pos/size for both ranges
    flushRange (start1, size1);
    flushRange (start2, size2);

    //  status flag indicates
    g_fifo.finishedRead (size1 + size2);
}
