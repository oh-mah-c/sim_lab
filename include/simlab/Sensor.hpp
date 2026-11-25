#pragma once
#include <array>
#include <cstdint>
#include "Types.hpp"

// CL-1 sensor encoder (Phase 1, shadow mode). Converts an agent's
// PERCEIVED world into fixed-width integer vectors — never reads World
// Truth beyond what AgentMind::memory/perceive() already recorded.
//
//   sensor vec : 112 x int8  (intero 16 | neuro 8 | entities 42 | social 4 | env 8 | spare 2 | act 32)
//   action vec : 32 x int8   (verb one-hot over the law vocabulary)
//   label vec  : 25 x int8   (16 intero deltas | 8 outcome classes | reward)
//
// All quantization is integer permille->int8; no floats anywhere.
//
// Phase "deep cognition": entity feats dropped the redundant `dist`
// channel (derivable from dx/dy) 8->7, freeing 6 channels; four became
// the social/temporal block (66..69) and two are reserved spare (78..79).
// Env moved 72..79 -> 70..77; action one-hot stays at 80..111.

namespace simlab {

class World;
struct InteractionContext;
struct CausalEvent;
struct Entity;

constexpr int kSensorChannels = 112;
constexpr int kInteroChannels = 16;    // sensor channels 0..15
constexpr int kNeuroChannels   = 8;    // channels 16..23
constexpr int kEntitySlots     = 6;    // channels 24..65 (6 x 7)
constexpr int kEntityFeats     = 7;
constexpr int kSocialChannels  = 4;    // channels 66..69
constexpr int kEnvChannels     = 8;    // channels 70..77
constexpr int kSpareChannels   = 2;    // channels 78..79 (reserved)
constexpr int kActChannels     = 32;   // channels 80..111 (last action)
// Named social/temporal channel indices (Phase deep-cognition).
constexpr int kChanKinship        = 66;  // tribal_kinship  -127..127
constexpr int kChanNeighborAffect = 67;  // neighbor_affect -127..127
constexpr int kChanInvSecurity    = 68;  // inventory_security 0..127
constexpr int kChanScarcityPhase  = 69;  // cycle_scarcity_phase -127..127
constexpr int kActionVerbs     = 32;
constexpr int kLabelChannels   = 25;   // 16 deltas | 8 outcomes | reward

using SensorVec = std::array<std::int8_t, kSensorChannels>;
using ActionVec = std::array<std::int8_t, kActionVerbs>;
using LabelVec  = std::array<std::int8_t, kLabelChannels>;

// Intero prop names, index-aligned with channels 0..15.
const std::array<const char*, kInteroChannels>& sensor_intero_names();
// Per-channel scale (prop units -> int8) and homeostatic sign:
// +1 = higher value is WORSE (needs/toxins), -1 = higher is BETTER.
const std::array<std::int64_t, kInteroChannels>& sensor_intero_scale();
const std::array<std::int32_t, kInteroChannels>& sensor_intero_sign();
// Optional per-channel offset applied before scaling (e.g. temperature).
const std::array<std::int64_t, kInteroChannels>& sensor_intero_offset();

// v -> int8: clamp(v*127/scale, -128, 127). scale must be > 0.
std::int8_t sensor_norm(std::int64_t v, std::int64_t scale);

// Encode the agent's CURRENT perceived state. `last_action` fills channels
// 80..95 (the verb the agent performed last tick); nullptr -> zeros.
// Only reads: entity props, terrain under the agent, mind memory/neuro.
SensorVec sensor_encode(const World& w, EntityId npc,
                        const ActionVec* last_action = nullptr);

// Verb -> one-hot index over the fixed 16-verb vocabulary; unknown ->
// all zeros (the brain learns "unrecognized" as a distinct pattern).
ActionVec sensor_action(const InteractionContext& ctx);
const char* const* sensor_verb_table();   // kActionVerbs names

// Extract the 25-channel training label from a committed CausalEvent:
// channels 0..15 = per-intero-prop deltas (channel units), 16..23 outcome
// classes, 24 = homeostatic reward (positive = needs improved).
// `pre` is the sensor vector captured BEFORE the action.
LabelVec sensor_label(const World& w, const CausalEvent& ev, EntityId actor,
                      const SensorVec& pre);

// Homeostatic cost of a sensor vector (lower = healthier). Used for the
// reward channel and Phase-3 curiosity bonuses.
std::int32_t sensor_homeo_cost(const SensorVec& s);

}  // namespace simlab
