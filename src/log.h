#include <stdio.h>
#include <time.h>

// Define the log levels
#define LOG_DEBUG  0
#define LOG_INFO   1
#define LOG_WARN   2
#define LOG_ERROR  3

// Set the active log level here
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_DEBUG
#endif

// Helper macro to handle variable arguments with file and line info
#define LOG_PRINT(stream, level, fmt, ...) do { \
    time_t now = time(NULL); \
    struct tm time_info; \
    char time_str[20]; \
    localtime_r(&now, &time_info); \
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_info); \
    fprintf(stream, "[%s] [%s] %s:%d: " fmt "\n", \
            time_str, level, __FILE__, __LINE__, ##__VA_ARGS__); \
} while(0)

// Macros for logging messages
#if LOG_LEVEL <= LOG_DEBUG
    #define LOG_DEBUG_MSG(fmt, ...) LOG_PRINT(stdout, "DEBUG", fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG_MSG(fmt, ...)  // Disabled
#endif

#if LOG_LEVEL <= LOG_INFO
    #define LOG_INFO_MSG(fmt, ...) LOG_PRINT(stdout, "INFO", fmt, ##__VA_ARGS__)
#else
    #define LOG_INFO_MSG(fmt, ...)  // Disabled
#endif

#if LOG_LEVEL <= LOG_WARN
    #define LOG_WARN_MSG(fmt, ...) LOG_PRINT(stdout, "WARN", fmt, ##__VA_ARGS__)
#else
    #define LOG_WARN_MSG(fmt, ...)  // Disabled
#endif

#if LOG_LEVEL <= LOG_ERROR
    #define LOG_ERROR_MSG(fmt, ...) LOG_PRINT(stderr, "ERROR", fmt, ##__VA_ARGS__)
#else
    #define LOG_ERROR_MSG(fmt, ...)  // Disabled
#endif
