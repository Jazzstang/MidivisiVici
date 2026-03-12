/**
 * @file UiMetrics.h
 * @brief Centralized UI spacing metrics shared by all editor modules.
 */
#pragma once

namespace UiMetrics
{
    // Unified module spacing policy (requested): 5 px everywhere.
    static constexpr int kModuleOuterMargin = 5;
    static constexpr int kModuleInnerMargin = 5;
    // Unified module column width used by all lane/main/LFO modules.
    static constexpr int kModuleWidth = 300;
    static constexpr float kModuleCornerRadius = 14.0f;
}
