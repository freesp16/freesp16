// TRACK SETTING (07-ui-design/screens/track-setting.md; UI-212/213, UID-525…531): the pad settings of all 16 tracks —
// template content+meter, context bar menu (TRACKS selected), the TrackTile grid in its setting variant (VEL / CHOKE /
// #NN badges, chip and frame in the pad colour, so a PAD COLOR turn shows at once), the MASTER meter and the strip
// VELOCITY, CHOKE, PAD COLOR, MOTION REC, MUTE MODE of the selected track (track parameters, none lockable: a held
// step disables them, UI-204). Two track selections exchange track contents and sequences. The grid widget lives in
// ui/screens/home/.
#include "ui/descriptors/ScreenSpec.h"

namespace
{
constexpr ui::StripSpec kTrackSettingStrip{
    "TRACK {track}",
    {"track.padVelocity", "track.chokeGroup", "track.padColor", "track.motionRec", "track.muteMode", ""}};
} // namespace

APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "trackSetting",
                                    .title = "TRACK SETTING",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::ContentMeter,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "TrackTile",
                                    .strip = kTrackSettingStrip,
                                    .meter = "master"}))
