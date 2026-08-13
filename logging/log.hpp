#pragma once

#include <cstdint>
#include <iostream>
#include <ostream>

namespace bs {
namespace log {

/*
 * -----------------------------------------------------------
 * Log levels
 * -----------------------------------------------------------
 *
 * Higher levels are more severe.  A message is emitted when
 * its level is greater than or equal to the current threshold.
 *
 *     Trace < Debug < Info < Warn < Error
 */

enum class Level : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

namespace detail {

struct Sink {
    std::ostream* stream = &std::cerr;

#if defined(BS_ENABLE_TRACE)
    Level threshold = Level::Trace;
#else
    Level threshold = Level::Info;
#endif
};

inline Sink& sink()
{
    static Sink instance;
    return instance;
}

inline const char* level_name(Level level)
{
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

/*
 * Stream proxy returned by line().
 *
 * The threshold is checked in the constructor so that the
 * [LEVEL] prefix is only written when the message is active.
 * operator<< relays only while active, so a runtime-suppressed
 * message is never written.
 */
class Line {
public:
    explicit Line(Level level)
        : active_(level >= sink().threshold),
          prefix_(level == Level::Trace || level == Level::Debug)
    {
        if (active_ && prefix_) {
            *sink().stream
                << '['
                << level_name(level)
                << "] ";
        }
    }

    template <typename T>
    Line& operator<<(const T& value)
    {
        if (active_) {
            *sink().stream << value;
        }
        return *this;
    }

private:
    bool active_;
    bool prefix_;
};

inline Line line(Level level)
{
    return Line(level);
}

} // namespace detail

/*
 * Runtime configuration.
 *
 * set_level() filters among the levels compiled in.  When
 * BS_ENABLE_TRACE is not defined, Debug/Trace statements are
 * removed entirely and cannot be re-enabled at runtime.
 */
inline void set_level(Level level)
{
    detail::sink().threshold = level;
}

inline Level level()
{
    return detail::sink().threshold;
}

inline void set_stream(std::ostream* stream)
{
    detail::sink().stream = stream;
}

/*
 * -----------------------------------------------------------
 * Logging macros
 * -----------------------------------------------------------
 *
 * Each macro takes a single parenthesised argument which is a
 * stream expression, e.g.
 *
 *     BS_LOG_TRACE("width = " << width << '\n');
 *
 * The leading operand is the Line proxy, so any valid stream
 * expression works.  Info/Warn/Error are always compiled in;
 * Debug/Trace are removed entirely unless BS_ENABLE_TRACE is
 * defined at build time.
 */

#define BS_LOG_ERROR(expr) \
    ::bs::log::detail::line(::bs::log::Level::Error) << expr

#define BS_LOG_WARN(expr) \
    ::bs::log::detail::line(::bs::log::Level::Warn) << expr

#define BS_LOG_INFO(expr) \
    ::bs::log::detail::line(::bs::log::Level::Info) << expr

#if defined(BS_ENABLE_TRACE)

#define BS_LOG_DEBUG(expr) \
    ::bs::log::detail::line(::bs::log::Level::Debug) << expr

#define BS_LOG_TRACE(expr) \
    ::bs::log::detail::line(::bs::log::Level::Trace) << expr

#else

#define BS_LOG_DEBUG(expr) ((void)0)

#define BS_LOG_TRACE(expr) ((void)0)

#endif

} // namespace log
} // namespace bs
