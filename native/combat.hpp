#pragma once
#include "entity.hpp"
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace yami::combat {
struct Weapon {
    std::string_view model, sound;
    float step, damage;
    std::uint32_t attacksPerSecond;
    float regeneration, energyCost;
    std::uint32_t lifetimeTicks;
};
const std::array<Weapon,8>& weapons() noexcept; //004323a0, exact table stride48
const Weapon& weapon(std::int32_t index); // Invalid script indices are errors, not defaults.
void regenerate(gameplay::EntityState&, entity::Runtime&, std::uint32_t frameNow);
struct Projectile {
    std::size_t owner = 0; // stable entity identity; do not compact the entity array
    Vec3 previous, committed, rendered, direction;
    std::uint32_t lifetime = 0; //FFFFFFFF remains finite original unsigned countdown
    float yaw = 0, unusedSecondYaw = 0;
};
struct World {
    std::vector<Projectile> projectiles;
    std::vector<std::uint32_t> meleeDelays; // player+194 list, countdown in fixed ticks
    std::uint32_t lastPlayerAttack = 0; // shared004693a8; persists across level reload
    std::uint32_t projectileTickStart = 0; //004ff6a0
    bool firstMeleeHit = true; // global004ade08, initialized004173a1; survives level reload
};
struct Context {
    std::vector<gameplay::EntityState>& entities;
    std::vector<entity::Runtime>& runtime;
    const std::vector<gameplay::Obb>& blocking;
    std::size_t player;
    std::int32_t level;
    std::int32_t& score;
    bool& levelEnabled;
    enemy_ai::Random& random; // shared with AI/idle animations; reseeds retained
    std::uint32_t frameNow, wallClock, unixSeconds;
    entity::Events& events;
    // REQUIRED for a lethal NPC hit/special kill: drain preceding events, then
    // VM.invoke_event(name,"_on_dead") synchronously. Storage changes are deferred
    // until this operation returns; callback score writes must remain visible.
    void* deathHost = nullptr;
    void (*invokeOnDead)(void*,std::string_view) = nullptr;
};
void select_weapon(gameplay::EntityState&,entity::Runtime&,std::int32_t next,
                   std::int32_t level,entity::Events&);
// Original00405950 does NOT add a second cooldown/energy/dead guard.
void attack_enemy(World&, Context&, std::size_t attacker);
// Original0040c3f0: cooldown uses strict >1000/rate and energy strict >cost;
// melee consumes immediately but emits projectiles after recovered tick delays.
bool attack_player(World&, Context&, std::int32_t requestedAction,
                   const Mat4& camera, bool automaticAim);
void apply_player_input(World&,Context&,gameplay::ForceState&,const gameplay::PlayerInput&,
                         const Mat4& camera,bool automaticAim);
void tick_melee(World&, Context&); //0040bed0; BEFORE body/AI fixed tick
void tick_projectiles(World&, Context&,std::uint32_t fixedTick); //00425fc0; AFTER bodies
void interpolate_projectiles(World&,std::uint32_t frameNow) noexcept;
Mat4 projectile_transform(const Projectile&) noexcept; // rendered model upright yaw only
// State/score is GameplayRecovery's damage(); this adds exact audio/flash/hook.
gameplay::DamageResult damage(World&,Context&,std::size_t target,float amount);
// SPACE/special input00416de0,level3 examines ONLY first linked-list actor.
void special_attack(Context&);
} // namespace yami::combat
