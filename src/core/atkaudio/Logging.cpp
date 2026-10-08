#include "Logging.h"

#include <obs-module.h>

namespace
{
int toObsLogLevel(atk::logging::Level level)
{
    switch (level)
    {
    case atk::logging::Level::debug:
        return LOG_DEBUG;
    case atk::logging::Level::info:
        return LOG_INFO;
    case atk::logging::Level::warning:
        return LOG_WARNING;
    case atk::logging::Level::error:
        return LOG_ERROR;
    }

    return LOG_INFO;
}
} // namespace

void atk::logging::log(Level level, const char* scope, const juce::String& message)
{
    // Debug-level messages are compiled in but only reach the OBS log in local debug builds.
#if !defined(ATK_DEBUG) || defined(ATK_CI_BUILD)
    if (level == Level::debug)
        return;
#endif

    const char* tag = (scope != nullptr && scope[0] != '\0') ? scope : "GENERAL";
    blog(toObsLogLevel(level), "[atkAudio][%s] %s", tag, message.toRawUTF8());
}
