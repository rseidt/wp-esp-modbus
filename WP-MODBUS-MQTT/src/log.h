#ifndef SRC_LOG_H_
#define SRC_LOG_H_

#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4

#define MAX_LOG_LEVEL 2 // Change this to set the maximum log level

#include "Arduino.h"
#include "modbus_registers.h"

void log(int16_t level, const String &message_s);

#endif // SRC_LOG_H_
