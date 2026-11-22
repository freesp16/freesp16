// Static registration (the APP_REGISTER_* macros): a namespace-scope object whose
// initialiser adds a descriptor to a registry before main() runs, so a feature is added by adding a file — no central
// list is ever edited. Registries are function-local statics (`builtin()`), so registration order between translation
// units cannot touch an uninitialised registry. Registration order across files is unspecified: registries that
// expose an order sort by an explicit key (ParamCatalogue::order, registration name), never by link order.
//
// The linker drops object files nothing references, so every library holding registrars is linked whole-archive
// (cmake/Modules.cmake app_link_modules). Registration happens on the main thread before main(); registries are
// read-only afterwards and may be read from any thread (never from the audio thread: they allocate).
#pragma once

#define APP_DETAIL_CONCAT2(a, b) a##b
#define APP_DETAIL_CONCAT(a, b) APP_DETAIL_CONCAT2(a, b)

// APP_STATIC_REGISTRAR(expression): evaluates `expression` once during static initialisation of this translation unit.
#define APP_STATIC_REGISTRAR(...)                                                                                       \
    namespace                                                                                                           \
    {                                                                                                                   \
    [[maybe_unused]] const bool APP_DETAIL_CONCAT(appStaticRegistrar_, __COUNTER__) = ((__VA_ARGS__), true);            \
    }
