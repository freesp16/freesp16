// Object swap slots (threading.md "Object swap protocol"): the immutable objects the audio thread reads by pointer —
// sample buffers per scene × track, the preview, patterns per scene × pattern, scene and project configuration, live
// takes. A slot kind is *accepted* once a consumer registers how to destroy its objects (message thread, before the
// device starts); ObjectSwaps for other kinds are refused by EngineBridge::post, so the sender keeps ownership.
//
//   message thread:  accept(kind, destroy)   valid(slot)
//   audio thread:    exchange(slot, ptr) at block start (returns the old pointer, which goes to the RetireQueue)
//                    get(slot) / as<T>(slot) from the stages
//   destructor:      destroys everything still installed (message thread; the audio device is stopped)
#pragma once

#include "engine/EngineApi.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::bridge
{
using DestroyFn = void (*)(const void*) noexcept;

class SwapTable
{
public:
    static constexpr int kKinds = static_cast<int>(SwapSlot::TrackPlugin) + 1; // the last kind
    static constexpr int kPerKind = 16 * 16; // scene 0–15 × index 0–15

    SwapTable() = default;
    SwapTable(const SwapTable&) = delete;
    SwapTable& operator= (const SwapTable&) = delete;
    ~SwapTable() { clear(); }

    // ---- message thread (before start) ----
    void accept(SwapSlot::Kind kind, DestroyFn destroy) noexcept { destroy_[static_cast<std::size_t>(kind)] = destroy; }
    bool accepts(SwapSlot::Kind kind) const noexcept
    {
        return static_cast<int>(kind) < kKinds && destroy_[static_cast<std::size_t>(kind)] != nullptr;
    }
    bool valid(const SwapSlot& s) const noexcept { return accepts(s.kind) && s.scene < 16 && s.index < 16; }
    DestroyFn destroyer(SwapSlot::Kind kind) const noexcept
    {
        return static_cast<int>(kind) < kKinds ? destroy_[static_cast<std::size_t>(kind)] : nullptr;
    }
    // Destroys every installed object (the audio thread must not be running).
    void clear() noexcept
    {
        for (int k = 0; k < kKinds; ++k)
            for (int i = 0; i < kPerKind; ++i)
                if (const void*& p = slots_[static_cast<std::size_t>(k * kPerKind + i)]; p != nullptr)
                {
                    if (destroy_[static_cast<std::size_t>(k)] != nullptr)
                        destroy_[static_cast<std::size_t>(k)](p);
                    p = nullptr;
                }
    }

    // ---- audio thread ----
    const void* exchange(const SwapSlot& s, const void* object) noexcept
    {
        const void*& p = slots_[index(s)];
        const void* old = p;
        p = object;
        return old;
    }
    const void* get(const SwapSlot& s) const noexcept { return slots_[index(s)]; }
    template <typename T>
    const T* as(const SwapSlot& s) const noexcept
    {
        return static_cast<const T*>(get(s));
    }

private:
    static std::size_t index(const SwapSlot& s) noexcept
    {
        return static_cast<std::size_t>(static_cast<int>(s.kind) * kPerKind + (s.scene & 15) * 16 + (s.index & 15));
    }

    std::array<DestroyFn, kKinds> destroy_ {};
    std::array<const void*, static_cast<std::size_t>(kKinds* kPerKind)> slots_ {};
};
} // namespace engine::bridge
