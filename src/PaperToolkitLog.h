#pragma once

#if defined(PAPER_TOOLKIT_POLICY_TEST_NO_RUNTIME_LOGGER)

#define PAPER_TOOLKIT_LOG_TRACE(category, ...) do { } while (false)
#define PAPER_TOOLKIT_LOG_DEBUG(category, ...) do { } while (false)
#define PAPER_TOOLKIT_LOG_INFO(category, ...) do { } while (false)
#define PAPER_TOOLKIT_LOG_WARN(category, ...) do { } while (false)
#define PAPER_TOOLKIT_LOG_ERROR(category, ...) do { } while (false)
#define PAPER_TOOLKIT_LOG_CRITICAL(category, ...) do { } while (false)

#else

#include <string_view>

using namespace std::literals;

#include "Logger.h"

#define PAPER_TOOLKIT_LOG_TRACE(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isTraceEnabled()) {                         \
            ::f4cf::logger::trace("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define PAPER_TOOLKIT_LOG_DEBUG(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isDebugEnabled()) {                         \
            ::f4cf::logger::debug("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define PAPER_TOOLKIT_LOG_INFO(category, ...)                                     \
    do {                                                                \
        if (::f4cf::logger::isInfoEnabled()) {                          \
            ::f4cf::logger::info("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define PAPER_TOOLKIT_LOG_WARN(category, ...)                                     \
    do {                                                                \
        if (::f4cf::logger::isWarnEnabled()) {                          \
            ::f4cf::logger::warn("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define PAPER_TOOLKIT_LOG_ERROR(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isErrorEnabled()) {                         \
            ::f4cf::logger::error("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define PAPER_TOOLKIT_LOG_CRITICAL(category, ...)                                    \
    do {                                                                   \
        if (::f4cf::logger::isCriticalEnabled()) {                         \
            ::f4cf::logger::critical("[PAPER_TOOLKIT::" #category "] " __VA_ARGS__); \
        }                                                                  \
    } while (false)

#endif
