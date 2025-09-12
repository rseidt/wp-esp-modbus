#include "log.h"

void log(int16_t level, const String &message_s)
{
    if (level <= MAX_LOG_LEVEL)
    {
        Serial.println("[" + String(level) + "]: " + message_s);
    }
}
