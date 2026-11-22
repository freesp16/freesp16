// The effect screens (screens/effects.md "Layout"; 03-ui/screens.md UI-260…268): FX1 / FX2 insert pages under TRACK
// MENU and the MASTER/SEND FX bus page under MIXER, each a full-width EffectPanel body. The strip descriptor carries
// only the tab: the panel binds the six columns to the slot's current effect (UID-125, UID-826).
//   track.fx1 / track.fx2: Full template, track context bar (BACK in slot A, the track's sample box in slot E),
//     breadcrumb HOME › TRACK MENU › FX1.
//   mixer.busFx: Full template, menu context bar with MIXER lit and PROJECT in slot A (UID-840), breadcrumb
//     HOME › MIXER › MASTER/SEND FX; BACK returns to MIXER.
// SELECT FX is the popup fx.select (a grid; the panel presents it through ShellApi::showGrid with FxGrid's request).
#include "ui/descriptors/PopupSpec.h"
#include "ui/descriptors/ScreenSpec.h"

APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.fx1",
                                    .title = "FX1",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = {},
                                    .bodyComponent = "EffectPanel",
                                    .strip = {"FX1", {}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.fx2",
                                    .title = "FX2",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = {},
                                    .bodyComponent = "EffectPanel",
                                    .strip = {"FX2", {}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "mixer.busFx",
                                    .title = "MASTER/SEND FX",
                                    .parent = "mixer",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "EffectPanel",
                                    .strip = {"SEND FX", {}}}))
APP_REGISTER_POPUP((ui::PopupSpec{.kind = ui::PopupSpec::Kind::Grid, .id = "fx.select", .title = "SELECT FX"}))
