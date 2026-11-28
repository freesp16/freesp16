# tokens.json -> generated C++ theme (ui/theme). The token file is a pinned copy of the spec's
# 07-ui-design/tokens.json (resources/design/tokens.json, spec version in resources/design/SPEC_VERSION);
# tools/gen_theme.py fails on unknown or missing tokens, so a spec change cannot silently drop a colour.
# resources/design/app-tokens.json holds the app's own palettes (the Classic skin), which the spec does not define.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(APP_TOKENS_JSON ${CMAKE_SOURCE_DIR}/resources/design/tokens.json)
set(APP_OWN_TOKENS_JSON ${CMAKE_SOURCE_DIR}/resources/design/app-tokens.json)
set(APP_THEME_HEADER ${APP_GENERATED_DIR}/ui/ThemeTokens.h)
add_custom_command(
    OUTPUT ${APP_THEME_HEADER}
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_theme.py ${APP_TOKENS_JSON} ${APP_THEME_HEADER}
            ${APP_OWN_TOKENS_JSON}
    DEPENDS ${APP_TOKENS_JSON} ${APP_OWN_TOKENS_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_theme.py
    COMMENT "Generating theme from tokens.json and app-tokens.json"
    VERBATIM)
add_custom_target(app_theme DEPENDS ${APP_THEME_HEADER})
