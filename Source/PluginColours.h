/**
 * @file PluginColours.h
 * @brief Centralized color palette and semantic color helpers.
 *
 * Threading:
 * - Read-only constants, safe from any thread.
 */
#pragma once
#include <array>
#include <utility>

/** @brief Global palette namespace used by all UI modules. */
namespace PluginColours
{
    // ========================================================================
    // Palette principale
    // ========================================================================

    // Accent principal
    inline const juce::Colour primary         = juce::Colour::fromString ("#FFadc5cf");
    inline const juce::Colour onPrimary       = juce::Colour::fromString ("#FF343434");

    // Accent secondaire
    inline const juce::Colour secondary       = juce::Colour::fromString ("#FF9db6c0");
    inline const juce::Colour onSecondary     = juce::Colour::fromString ("#FF343434");

    // Contrast
    inline const juce::Colour contrast       = juce::Colour::fromString ("#FFfabad6");
    inline const juce::Colour onContrast     = juce::Colour::fromString ("#FF9b60a1");

    // Fond général
    inline const juce::Colour background      = juce::Colour::fromString ("#FF5d7a89");
    inline const juce::Colour onBackground    = juce::Colour::fromString ("#FFFFFFFF");

    // Surface (panneaux, cartes)
    inline const juce::Colour surface         = juce::Colour::fromString ("#FF83a5b3");
    inline const juce::Colour onSurface       = juce::Colour::fromString ("#FF343434");

    // Styles specifiques pour MainMenu
    inline const juce::Colour mainMenuSurface  = juce::Colour::fromString ("#FF6c8a99");
    inline const juce::Colour mainMenuPrimary  = juce::Colour::fromString ("#FF7c9baa");


    // ========================================================================
    // États sémantiques
    // ========================================================================

    // Erreur
    inline const juce::Colour error           = juce::Colour::fromString ("#FFFF453A");
    inline const juce::Colour onError         = juce::Colour::fromString ("#FF343434");

    // Succès
    inline const juce::Colour success         = juce::Colour::fromString ("#FF30D158");
    inline const juce::Colour onSuccess       = juce::Colour::fromString ("#FF343434");

    // Avertissement
    inline const juce::Colour warning         = juce::Colour::fromString ("#FFFFD60A");
    inline const juce::Colour onWarning       = juce::Colour::fromString ("#FF343434");

    // Info
    inline const juce::Colour info            = juce::Colour::fromString ("#FF64D2FF");
    inline const juce::Colour onInfo          = juce::Colour::fromString ("#FF343434");

    // ========================================================================
    // États d'interaction
    // ========================================================================
    inline const juce::Colour hover           = juce::Colour::fromString ("#FF9db6c0");
    inline const juce::Colour pressed         = juce::Colour::fromString ("#FFadc5cf");
    inline const juce::Colour disabled        = juce::Colour::fromString ("#FF83a5b3");
    inline const juce::Colour onDisabled      = juce::Colour::fromString ("#FF5d7a89");

    // Dividers
    inline const juce::Colour divider         = juce::Colour::fromString ("#FF343434");

    // ========================================================================
    // Arpeggiator
    // ========================================================================

    // --- LAY BACKGROUNDS ---
    const juce::Colour lay1 = juce::Colour::fromString("ffb6d6b1");
    const juce::Colour lay2 = juce::Colour::fromString("fff2c2a0");
    const juce::Colour lay3 = juce::Colour::fromString("fff4e4a8");
    const juce::Colour lay4 = juce::Colour::fromString("ffa5d1cc");
    const juce::Colour lay5 = juce::Colour::fromString("ffc4b8d8");
    const juce::Colour lay6 = juce::Colour::fromString("fff5d1a3");
    const juce::Colour lay7 = juce::Colour::fromString("ffb2dcd4");
    const juce::Colour lay8 = juce::Colour::fromString("ffd3c2b8");

    // --- LAY ON-COLOURS (texte, icônes) ---
    const juce::Colour onLay1 = juce::Colour::fromString("ff5e765a");
    const juce::Colour onLay2 = juce::Colour::fromString("ff9b6442");
    const juce::Colour onLay3 = juce::Colour::fromString("ff9b8a40");
    const juce::Colour onLay4 = juce::Colour::fromString("ff417a74");
    const juce::Colour onLay5 = juce::Colour::fromString("ff625985");
    const juce::Colour onLay6 = juce::Colour::fromString("ff9b7443");
    const juce::Colour onLay7 = juce::Colour::fromString("ff4a7f77");
    const juce::Colour onLay8 = juce::Colour::fromString("ff6f5e53");

    // ========================================================================
    // Style des Ombres
    // ========================================================================
    struct ShadowStyle
    {
        juce::Colour colour;
        int radius;
        juce::Point<int> offset;
    };

    // Styles d'ombre globaux
    struct Shadows
    {
        static ShadowStyle moduleBackground()
        {
            return { juce::Colours::black, 0, { 0, 0 } };
        }

        static ShadowStyle footerPill()
        {
            return { juce::Colours::black, 0, { 0, 0 } };
        }
    };

// ========================================================================
// Fonction utilitaire arpeggiator
// ========================================================================
    struct LayerColours
    {
        juce::Colour background;
        juce::Colour text;
    };

    inline int defaultPaletteIndexForOrder(int zeroBasedOrder) noexcept
    {
        int wrapped = zeroBasedOrder % 16;
        if (wrapped < 0)
            wrapped += 16;
        return wrapped;
    }

    inline std::pair<juce::Colour, juce::Colour> getIndexedNameColours(int colourIndex)
    {
        static const std::array<std::pair<juce::Colour, juce::Colour>, 8> kLay =
        {{
            { lay1, onLay1 },
            { lay2, onLay2 },
            { lay3, onLay3 },
            { lay4, onLay4 },
            { lay5, onLay5 },
            { lay6, onLay6 },
            { lay7, onLay7 },
            { lay8, onLay8 }
        }};

        const int wrapped = defaultPaletteIndexForOrder(colourIndex);
        const auto [bg, fg] = kLay[(size_t) (wrapped % 8)];
        return (wrapped < 8) ? std::make_pair(bg, fg)
                             : std::make_pair(fg, bg);
    }

    inline LayerColours getLayerColours(int layerIndex)
    {
        switch (layerIndex)
        {
            case 0: return { PluginColours::lay1, PluginColours::onLay1 };
            case 1: return { PluginColours::lay2, PluginColours::onLay2 };
            case 2: return { PluginColours::lay3, PluginColours::onLay3 };
            case 3: return { PluginColours::lay4, PluginColours::onLay4 };
            case 4: return { PluginColours::lay5, PluginColours::onLay5 };
            case 5: return { PluginColours::lay6, PluginColours::onLay6 };
            case 6: return { PluginColours::lay7, PluginColours::onLay7 };
            case 7: return { PluginColours::lay8, PluginColours::onLay8 };
            default: return { PluginColours::background, PluginColours::onSurface };
        }
    }
}
