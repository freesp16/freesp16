// Every value the engine needs for a project (the composition root's project forwarding, factored out of
// app/Document.cpp so the live engine, the offline render service (services/render: a
// services::render::ProjectFeeder) and the render CLI (tools/render-cli, which compiles this file) post the same
// messages in the same order).
//
//   forwardAll         the 16 scenes' track tables and FX (the engine keeps them all: a scene switch is an index
//                      change), the 256 patterns and the sequencer values, the ribbon, the scene values and — last
//                      — the switch to the project's current scene/pattern, the routing, the MIDI settings and MIDI
//                      tracks. ≈ 15,000 messages: a QueuedBridge hands them on as the FIFO drains.
//   postPlan           the arrangement plan (ObjectSwap SceneConfig 0/15); `status` = where the engine
//                      publishes the song position (the live engine's; the offline render's own or null)
//   postPlay           song mode (arrangement.play) from the project
//   feeder(withPlan)   forwardAll [+ postPlan] as a services::render::ProjectFeeder
// Any thread that owns the bridge (the message thread live; the render worker offline). Reads the project only.
#pragma once

#include "core/model/Project.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "engine/EngineApi.h"
#include "services/render/OfflineEngine.h"

namespace app::feed
{
void forwardAll(const core::model::Project&, engine::EngineBridge&);
void postRouting(const core::model::Project&, engine::EngineBridge&);
void postPlan(const core::model::Project&, engine::EngineBridge&, core::arrangement::SongStatus* status);
void postPlay(const core::model::Project&, engine::EngineBridge&);
// Every track's slice table for the snapshot's buffers (frames 0 = no sample: the slot is cleared).
void postSlices(const core::model::Project&, engine::EngineBridge&, const services::render::SampleSnapshot&);
// forwardAll, then (withPlan) postPlan with the render's status, then postSlices. Song mode itself is the render's (it
// posts arrangement.play for its source).
services::render::ProjectFeeder feeder(bool withPlan = true);
} // namespace app::feed
