#pragma once
#include <JuceHeader.h>

//==============================================================================
// Centralized logging for MoRS
//==============================================================================

enum class MorsLogLevel { trace = 0, debug, info, warn, error };

class MorsDebug
{
public:
    // Constructor (subsequent calls are ignored)
    static void init();

    //  Destructor
    static void shutdown();

    // Set Minimum Level
    static void setMinLevel (MorsLogLevel level);

    // Non-realtime logging (may allocate + write to disk). NOT audio-thread-safe.
    static void write (MorsLogLevel level,
                       const char* category,
                       const juce::String& message,
                       const char* file,
                       int line,
                       const char* func);

    // Realtime-safe logging (no allocations, no locks, no disk I/O) message must be a stable C string for the duration of this call
    static void pushRealtime (MorsLogLevel level,
                              const char* category,
                              const char* message,
                              const char* file,
                              int line,
                              const char* func) noexcept;

    // Flush queued realtime messages to the current logger (call from message thread / background thread)
    static void flushRealtime();

    // Helper retained
    static juce::String currentTimeString();

private:
    static juce::File findDebugDirectory();
    static juce::File makeLogFilePath();

    MorsDebug() = delete;
};

//==============================================================================
// Convenience macros (non-realtime)
//==============================================================================
#define MORS_LOG(level, category, message) \
    MorsDebug::write((level), (category), (message), __FILE__, __LINE__, __func__)

#define LOGT(cat, msg) MORS_LOG(MorsLogLevel::trace, cat, msg)
#define LOGD(cat, msg) MORS_LOG(MorsLogLevel::debug, cat, msg)
#define LOGI(cat, msg) MORS_LOG(MorsLogLevel::info,  cat, msg)
#define LOGW(cat, msg) MORS_LOG(MorsLogLevel::warn,  cat, msg)
#define LOGE(cat, msg) MORS_LOG(MorsLogLevel::error, cat, msg)

//==============================================================================
// Convenience macros (realtime-safe; use in audio thread)
// msg must be a stable C string for the duration of the call
//==============================================================================
#define MORS_LOG_RT(level, category, msg_cstr) \
    MorsDebug::pushRealtime((level), (category), (msg_cstr), __FILE__, __LINE__, __func__)

#define LOGT_RT(cat, msg_cstr) MORS_LOG_RT(MorsLogLevel::trace, cat, msg_cstr)
#define LOGD_RT(cat, msg_cstr) MORS_LOG_RT(MorsLogLevel::debug, cat, msg_cstr)
#define LOGI_RT(cat, msg_cstr) MORS_LOG_RT(MorsLogLevel::info,  cat, msg_cstr)
#define LOGW_RT(cat, msg_cstr) MORS_LOG_RT(MorsLogLevel::warn,  cat, msg_cstr)
#define LOGE_RT(cat, msg_cstr) MORS_LOG_RT(MorsLogLevel::error, cat, msg_cstr)

//==============================================================================
// Optional function scope tracing (compile-time toggle)
//==============================================================================
#ifndef MORS_ENABLE_SCOPE_TRACE
#define MORS_ENABLE_SCOPE_TRACE 0
#endif

#if MORS_ENABLE_SCOPE_TRACE
class MorsLogScope
{
public:
    MorsLogScope(const char* category, const char* file, int line, const char* func)
        : cat(category), f(file), l(line), fn(func)
    {
        MorsDebug::write(MorsLogLevel::trace, cat, "ENTER", f, l, fn);
    }
    ~MorsLogScope()
    {
        MorsDebug::write(MorsLogLevel::trace, cat, "EXIT", f, l, fn);
    }
private:
    const char* cat;
    const char* f;
    int l;
    const char* fn;
};
#define LOG_SCOPE(cat) MorsLogScope _morsLogScope((cat), __FILE__, __LINE__, __func__)
#else
#define LOG_SCOPE(cat) do {} while (0)
#endif

//==============================================================================
// Performance timing macros (realtime-safe)
//==============================================================================
#define PERF_START(name) \
    const auto _perf_start_##name = juce::Time::getHighResolutionTicks()

#define PERF_END(name) \
    { \
        static std::atomic<uint32_t> _perf_counter_##name { 0 }; \
        const auto _perf_end_##name = juce::Time::getHighResolutionTicks(); \
        const double _perf_ms_##name = juce::Time::highResolutionTicksToSeconds( \
            _perf_end_##name - _perf_start_##name) * 1000.0; \
        if ((_perf_counter_##name.fetch_add(1, std::memory_order_relaxed) & 0xFFu) == 0u) \
        { \
            char _perf_msg_##name[64]; \
            std::snprintf(_perf_msg_##name, sizeof(_perf_msg_##name), \
                          #name ": %.3f ms", _perf_ms_##name); \
            LOGD_RT("PERF", _perf_msg_##name); \
        } \
    }


// In a shared header (e.g. MorsDebug.h or a new MorsLogHelpers.h):
template <typename T>
void logStateChange (const char*        category,
                     const char*        functionName,
                     const char*        fieldName,
                     const T&           prev,
                     const T&           now,
                     const juce::String& extra = {})
{
    juce::String msg = juce::String (functionName) + "(): " + fieldName + " changed"
                       " | prev = " + juce::String (prev)
                       + " | now = " + juce::String (now);
    if (extra.isNotEmpty())
        msg += " | " + extra;

    LOGI (category, msg);
}
