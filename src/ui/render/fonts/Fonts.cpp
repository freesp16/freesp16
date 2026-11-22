#include "ui/render/fonts/Fonts.h"

namespace ui::fonts
{
Blob blob(Face f) noexcept
{
    switch (f)
    {
    case Face::InterLight:
        return {data::interLight, data::interLightSize, "Inter-Light"};
    case Face::InterRegular:
        return {data::interRegular, data::interRegularSize, "Inter-Regular"};
    case Face::InterMedium:
        return {data::interMedium, data::interMediumSize, "Inter-Medium"};
    case Face::InterSemiBold:
        return {data::interSemiBold, data::interSemiBoldSize, "Inter-SemiBold"};
    case Face::Mono:
        return {data::jetBrainsMonoRegular, data::jetBrainsMonoRegularSize, "JetBrainsMono-Regular"};
    case Face::Symbols:
        return {data::dejaVuSymbols, data::dejaVuSymbolsSize, "DejaVuSans-Symbols"};
    case Face::Count:
        break;
    }
    return {};
}

Face faceForWeight(int weight) noexcept
{
    if (weight <= 350)
        return Face::InterLight;
    if (weight < 450)
        return Face::InterRegular;
    if (weight < 550)
        return Face::InterMedium;
    return Face::InterSemiBold;
}
} // namespace ui::fonts
