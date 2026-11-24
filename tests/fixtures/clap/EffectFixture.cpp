// A multi-descriptor CLAP bundle for the host's discovery and audio-routing tests.
#include <clap/clap.h>

#include <algorithm>
#include <cstring>

namespace
{
const char* const fx[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const char* const instrument[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
const clap_plugin_descriptor_t descriptors[] = {
    {.clap_version = CLAP_VERSION, .id = "test.stereo", .name = "Stereo gain", .features = fx},
    {.clap_version = CLAP_VERSION, .id = "test.mono", .name = "Mono gain", .features = fx},
    {.clap_version = CLAP_VERSION, .id = "test.unsupported", .name = "Surround", .features = fx},
    {.clap_version = CLAP_VERSION, .id = "test.instrument", .name = "Instrument", .features = instrument}};

bool ports(const clap_plugin_t* p, uint32_t index, bool, clap_audio_port_info_t* info)
{
    if (index != 0)
        return false;
    *info = {};
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = p->desc == &descriptors[2] ? 6u : (p->desc == &descriptors[1] ? 1u : 2u);
    return true;
}
const clap_plugin_audio_ports_t audioPorts{[](const clap_plugin_t*, bool) -> uint32_t { return 1; }, ports};

constexpr clap_id gainId = 42;
void applyEvents(const clap_plugin_t* plugin, const clap_input_events_t* events)
{
    for (uint32_t i = 0; events && i < events->size(events); ++i)
    {
        const auto* event = events->get(events, i);
        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* param = reinterpret_cast<const clap_event_param_value_t*>(event);
            if (param->param_id == gainId)
                *static_cast<uint8_t*>(plugin->plugin_data) = static_cast<uint8_t>(param->value);
        }
    }
}
const clap_plugin_params_t params{.count = [](const clap_plugin_t*) -> uint32_t { return 1; },
                                  .get_info =
                                      [](const clap_plugin_t*, uint32_t i, clap_param_info_t* info)
                                  {
                                      if (i != 0)
                                          return false;
                                      *info = {};
                                      info->id = gainId;
                                      info->flags = CLAP_PARAM_IS_STEPPED;
                                      std::strcpy(info->name, "Gain");
                                      info->max_value = 100;
                                      info->default_value = 25;
                                      return true;
                                  },
                                  .get_value =
                                      [](const clap_plugin_t* p, clap_id id, double* value)
                                  {
                                      if (id != gainId)
                                          return false;
                                      *value = *static_cast<uint8_t*>(p->plugin_data);
                                      return true;
                                  },
                                  .value_to_text = nullptr,
                                  .text_to_value = nullptr,
                                  .flush = [](const clap_plugin_t* p, const clap_input_events_t* in,
                                              const clap_output_events_t*) { applyEvents(p, in); }};
const clap_plugin_state_t state{
    [](const clap_plugin_t* p, const clap_ostream_t* out) { return out->write(out, p->plugin_data, 1) == 1; },
    [](const clap_plugin_t* p, const clap_istream_t* in) { return in->read(in, p->plugin_data, 1) == 1; }};

const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id)
{
    for (const auto& desc : descriptors)
        if (std::strcmp(id, desc.id) == 0)
            return new clap_plugin_t{
                .desc = &desc,
                .plugin_data = new uint8_t{25},
                .init = [](const clap_plugin_t*) { return true; },
                .destroy =
                    [](const clap_plugin_t* p)
                {
                    delete static_cast<uint8_t*>(p->plugin_data);
                    delete p;
                },
                .activate = [](const clap_plugin_t*, double, uint32_t, uint32_t) { return true; },
                .deactivate = [](const clap_plugin_t*) {},
                .start_processing = [](const clap_plugin_t*) { return true; },
                .stop_processing = [](const clap_plugin_t*) {},
                .reset = [](const clap_plugin_t*) {},
                .process = [](const clap_plugin_t* p, const clap_process_t* block) -> clap_process_status
                {
                    applyEvents(p, block->in_events);
                    if (block->audio_inputs_count == 0)
                    {
                        for (uint32_t c = 0; c < block->audio_outputs[0].channel_count; ++c)
                            std::fill_n(block->audio_outputs[0].data32[c], block->frames_count, 0.0f);
                        return CLAP_PROCESS_SLEEP;
                    }
                    const auto& in = block->audio_inputs[0];
                    const auto& out = block->audio_outputs[0];
                    if (in.channel_count != out.channel_count || in.constant_mask)
                        return CLAP_PROCESS_ERROR;
                    for (uint32_t c = 0; c < in.channel_count; ++c)
                        for (uint32_t i = 0; i < block->frames_count; ++i)
                            out.data32[c][i] =
                                in.data32[c][i] * (static_cast<float>(*static_cast<uint8_t*>(p->plugin_data)) / 100.0f);
                    return CLAP_PROCESS_SLEEP;
                },
                .get_extension = [](const clap_plugin_t* p, const char* ext) -> const void*
                {
                    if (std::strcmp(ext, CLAP_EXT_PARAMS) == 0)
                        return &params;
                    if (p->desc != &descriptors[3] && std::strcmp(ext, CLAP_EXT_STATE) == 0)
                        return &state;
                    return std::strcmp(ext, CLAP_EXT_AUDIO_PORTS) == 0 ? &audioPorts : nullptr;
                },
                .on_main_thread = [](const clap_plugin_t*) {}};
    return nullptr;
}
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*) -> uint32_t { return 4; },
                                    [](const clap_plugin_factory_t*, uint32_t i)
                                    { return i < 4 ? &descriptors[i] : nullptr; }, create};
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION, [](const char*) { return true; }, [] {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }};
