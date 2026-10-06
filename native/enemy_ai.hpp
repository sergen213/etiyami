#pragma once
#include "gameplay.hpp"
#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>

namespace yami::enemy_ai {
// Addresses identify the evidence, not function-pointer ABI contracts.
enum class Kind : std::uint8_t { Utu = 1, Yayli, Bisiklet, Pervane, Soba };
struct Random {
    std::uint32_t state = 1; // 00449411: shared MSVC rand stream, not one per enemy.
    std::uint32_t next() noexcept;
    float between(float low, float high) noexcept; // 00413570
};
// 00401000/004010d0/004011c0: call once before all AI in a fixed tick.
struct MotionHistory {
    std::array<Vec3,10> samples{};
    Vec3 total, average;
    std::uint32_t ticks = 0;
    std::size_t nextSample = 0, lastSample = 0;
    void sample_tick(float playerForceSpeed, Vec3 playerForceDirection) noexcept;
    const Vec3& latest() const noexcept { return samples[lastSample]; }
};
struct Event {
    std::uint8_t type = 0; // recovered 004082d0 opcodes 0..15
    float parameter = 0;
    std::uint32_t repeats = 1;
    bool immediate = false;
};
struct State {
    Kind kind = Kind::Utu;
    bool enabled = false; // base +4; caller activates through script/entity setup.
    std::int32_t mode = 10; // +c: 10 idle,30 search,40 evade,50 engage,60 retreat
    float lastHealth = 1; // +10
    std::uint32_t lastTime = 0; // +14
    std::int32_t collisionDirection = 0; // +18
    float distanceSquared = 0; // +1c
    std::uint32_t elapsed20 = 0, elapsed24 = 0, unseenTime = 0;
    bool detectStuck = true, stuck = false, repeatedStuck = false;
    bool evadeAfterAttack = false, evadeDamage = true, reactHit = true, alternateEvade = false;
    bool blocked = false, blockedForward = false, blockedBack = false;
    bool blockedLeft = false, blockedRight = false, hasTarget = false, flag3d = false;
    std::uint32_t moveTicks = 0, stationaryTicks = 0, freeTicks = 0, stuckTicks = 0;
    Vec3 lastPosition;
    std::int32_t escapeDirection = 0, distanceBand = 2;
    // Run-length storage retains original event-count/tick behavior without one
    // heap allocation per original linked-list node.
    std::vector<Event> events;
    std::size_t firstEvent = 0;
    std::uint32_t queuedEvents = 0;
};
State initialize(Kind, Vec3 position);
struct Input {
    std::uint32_t now = 0;
    Vec3 forwardAxis, rightAxis; // model rows extracted by 004232a0/00423260
    Vec3 playerVelocity; // 004011c0: MotionHistory::latest(), sampled every5ticks
    Vec3 collisionNormal; // 00404bb0 latest force collision normal
    float forceScalar = 0; // ForceState.speed: same00422970 value returned00404bb0
    float targetRadiusSquared = 0; // entity+17c; 00427310 compares squared distance
    Vec3 targetPosition; // entity +170
};
struct Operation {
    enum class Type { Move, Turn, Action, Attack } type;
    gameplay::Move move = gameplay::Move::Stop;
    float degrees = 0;
    std::int32_t action = 0;
};
// Execute all operations in order: entity virtual movement/turn, action through
// gameplay::transition_action, and 00405950 weapon attack. The AI updates the
// +98/+99/+b0 guards; action is emitted after movement, preserving original order.
void tick(State&, gameplay::EntityState& entity, gameplay::EntityState& player,
          const Input&, const std::vector<gameplay::Obb>& blocking,
          const std::vector<gameplay::EntityState>& entities, std::size_t entityIndex,
          std::size_t playerIndex, Random&, std::vector<Operation>& operations);
} // namespace yami::enemy_ai
