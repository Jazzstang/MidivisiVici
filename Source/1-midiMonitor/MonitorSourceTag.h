/**
 * @file MonitorSourceTag.h
 * @brief Source tagging for output monitor lines.
 *
 * Threading:
 * - Plain POD enum values, safe from any thread.
 */
#pragma once

#include <cstdint>

/**
 * @brief Encodes where a monitored output event comes from.
 */
enum class MonitorSourceKind : std::uint8_t
{
    Unknown = 0,
    Lane = 1,
    MainBarController = 2
};

