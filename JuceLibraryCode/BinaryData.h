/* =========================================================================================

   This is an auto-generated file: Any edits you make may be overwritten!

*/

#pragma once

namespace BinaryData
{
    extern const char*   JostBlack_ttf;
    const int            JostBlack_ttfSize = 61692;

    extern const char*   JostBlackItalic_ttf;
    const int            JostBlackItalic_ttfSize = 66120;

    extern const char*   JostBold_ttf;
    const int            JostBold_ttfSize = 61612;

    extern const char*   JostBoldItalic_ttf;
    const int            JostBoldItalic_ttfSize = 66036;

    extern const char*   JostExtraBold_ttf;
    const int            JostExtraBold_ttfSize = 61712;

    extern const char*   JostExtraBoldItalic_ttf;
    const int            JostExtraBoldItalic_ttfSize = 66116;

    extern const char*   JostExtraLight_ttf;
    const int            JostExtraLight_ttfSize = 61688;

    extern const char*   JostExtraLightItalic_ttf;
    const int            JostExtraLightItalic_ttfSize = 65932;

    extern const char*   JostItalic_ttf;
    const int            JostItalic_ttfSize = 66064;

    extern const char*   JostLight_ttf;
    const int            JostLight_ttfSize = 61624;

    extern const char*   JostLightItalic_ttf;
    const int            JostLightItalic_ttfSize = 66100;

    extern const char*   JostMedium_ttf;
    const int            JostMedium_ttfSize = 61652;

    extern const char*   JostMediumItalic_ttf;
    const int            JostMediumItalic_ttfSize = 66160;

    extern const char*   JostRegular_ttf;
    const int            JostRegular_ttfSize = 61524;

    extern const char*   JostSemiBold_ttf;
    const int            JostSemiBold_ttfSize = 61648;

    extern const char*   JostSemiBoldItalic_ttf;
    const int            JostSemiBoldItalic_ttfSize = 66148;

    extern const char*   JostThin_ttf;
    const int            JostThin_ttfSize = 61584;

    extern const char*   JostThinItalic_ttf;
    const int            JostThinItalic_ttfSize = 65724;

    // Number of elements in the namedResourceList and originalFileNames arrays.
    const int namedResourceListSize = 18;

    // Points to the start of a list of resource names.
    extern const char* namedResourceList[];

    // Points to the start of a list of resource filenames.
    extern const char* originalFilenames[];

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding data and its size (or a null pointer if the name isn't found).
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding original, non-mangled filename (or a null pointer if the name isn't found).
    const char* getNamedResourceOriginalFilename (const char* resourceNameUTF8);
}
