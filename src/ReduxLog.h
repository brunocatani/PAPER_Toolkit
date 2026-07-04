#pragma once

#if defined(REDUX_POLICY_TEST_NO_RUNTIME_LOGGER)

#define RDX_LOG_TRACE(category, ...) do { } while (false)
#define RDX_LOG_DEBUG(category, ...) do { } while (false)
#define RDX_LOG_INFO(category, ...) do { } while (false)
#define RDX_LOG_WARN(category, ...) do { } while (false)
#define RDX_LOG_ERROR(category, ...) do { } while (false)
#define RDX_LOG_CRITICAL(category, ...) do { } while (false)

#else

#include <string_view>

using namespace std::literals;

#include "Logger.h"

#define RDX_LOG_TRACE(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isTraceEnabled()) {                         \
            ::f4cf::logger::trace("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define RDX_LOG_DEBUG(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isDebugEnabled()) {                         \
            ::f4cf::logger::debug("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define RDX_LOG_INFO(category, ...)                                     \
    do {                                                                \
        if (::f4cf::logger::isInfoEnabled()) {                          \
            ::f4cf::logger::info("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define RDX_LOG_WARN(category, ...)                                     \
    do {                                                                \
        if (::f4cf::logger::isWarnEnabled()) {                          \
            ::f4cf::logger::warn("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define RDX_LOG_ERROR(category, ...)                                    \
    do {                                                                \
        if (::f4cf::logger::isErrorEnabled()) {                         \
            ::f4cf::logger::error("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                               \
    } while (false)

#define RDX_LOG_CRITICAL(category, ...)                                    \
    do {                                                                   \
        if (::f4cf::logger::isCriticalEnabled()) {                         \
            ::f4cf::logger::critical("[REDUX::" #category "] " __VA_ARGS__); \
        }                                                                  \
    } while (false)

#endif
