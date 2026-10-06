#pragma once
#include "enemy_ai.hpp"
#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace yami::entity {
// A side effect is mandatory work for the engine host, not a substitute callback.
// Subject views refer to persistent entity/trigger names or static resource names.
struct Event {
    enum class Kind { SoundPlay, MenuActivate, MenuMessage, Pause, Video,
                      ReplacePlayerModel } kind;
    std::string_view subject;
    std::int32_t value = 0, duration = 0;
};
using Events = std::vector<Event>;
struct Runtime {
    Mat4 transform = identity_matrix(); // original entity+c8 model+10
    Vec3 muzzle; // entity+70,+74,+78, local coordinates
    std::int32_t barrels = 0; // +7c: 0 center,1 alternate,2 double
    bool horizontalAim = false; // +9c: Utu/Pervane do not turn aim horizontally
    float aimPitch = 0, aimYaw = 0; // +a0,+a8 radians
    float pitchLimit = 0.349060f, yawLimit = 0.174530f; // bit exact initialization below
    float playerPitch = 0; // global004ade1c, degrees
    std::uint32_t regenerationStart = 0; // +c4, starts at zero, not construction time
    std::int32_t flashDraws = 0; // +4: decremented on DRAW, not tick
    std::uint8_t soundVariant = 1; // +6c, constructor reseeds shared MSVC rand
    bool alternateModel = false; // player+198
};
Runtime initialize(std::string_view model, Vec3 position, enemy_ai::Random&,
                   std::uint32_t unixSeconds);
void synchronize_position(Runtime&, const gameplay::EntityState&) noexcept;
Vec3 right_axis(const Runtime&) noexcept;
Vec3 forward_axis(const Runtime&) noexcept;
// Vtable+18 is world-X pitch00423790; +1c rotates around model ROW-up00423a40.
void rotate_world_x(Mat4&, float degrees) noexcept;
void rotate_world_y(Mat4&, float degrees) noexcept;
void rotate_world_z(Mat4&, float degrees) noexcept;
void rotate_local_up(Mat4&, float degrees) noexcept;
void turn(Runtime&, float degrees) noexcept;
void look(Runtime&, float yawDegrees, float pitchDegrees) noexcept; // [-70,40] pitch
void move(gameplay::EntityState&, gameplay::ForceState&, const Runtime&, gameplay::Move) noexcept;
gameplay::ActionTransition action(gameplay::EntityState&, gameplay::EntityState& player,
                                  std::int32_t next, std::uint32_t wallClock, Events&);
// Animation ordinal is the action index in each exported Mesh.animation_names.
// Duration comes from the FIRST model mesh (00410350); no guessed name mapping.
std::uint32_t animation_duration(std::uint32_t frameCount);
void update_animation(gameplay::EntityState&, gameplay::EntityState& player,
                      std::uint32_t firstMeshFrameCount, std::int32_t level,
                      std::uint32_t wallClock, enemy_ai::Random&, Events&);
void update_render(gameplay::EntityState&, Runtime&, std::uint32_t frameNow) noexcept;
// Returns original diffuse light color and consumes exactly one visible draw.
std::array<float,4> draw_light(const gameplay::EntityState&, Runtime&) noexcept;
Mat4 upright_transform(const Runtime&, float modelCollisionHeight) noexcept; //0040ffc0
Vec3 enemy_aim(Runtime&, Vec3 targetPosition) noexcept; //00404f30
Vec3 player_muzzle(const Runtime&, Vec3 modelPosition) noexcept; //0040b6c0(8,20,30)
// Mandatory synchronous VM: callbacks may disable subsequent boxes or teleport
// the player; position and disabled bytes are reread for EACH trigger.
void triggers(const gameplay::EntityState& player,const std::vector<gameplay::Obb>&,bool use,
               void* host,void (*invokeEvent)(void*,std::string_view,std::string_view));
struct PickupAnimation {
    std::uint32_t elapsed = 0, previousClock;
    explicit PickupAnimation(std::uint32_t wallClock) : previousClock(wallClock) {}
};
void update_pickup(PickupAnimation&,std::uint32_t wallClock) noexcept; //004176f0
struct PickupRuntime {
    Vec3 originalPosition; // +68..70, captured only on first level-four positioning
    PickupAnimation animation;
    PickupRuntime(Vec3 position,std::uint32_t wallClock)
        : originalPosition(position),animation(wallClock) {}
};
gameplay::HealthState initialize_pickup(const gameplay::LevelHealth&,const Model&);
struct FlightPickupMotion { float phase = 0; Vec3 offset{4000,0,0}; };
// 00418550 updates only the original unculled render path;00418600 does not.
void update_flight_pickups(FlightPickupMotion&,std::span<const PickupRuntime>,
                           std::vector<gameplay::HealthState>&);
// Called once per rendered gameplay frame (0041643c), even when fixed ticks differ.
void respawn_big_health(gameplay::EntityState&, std::vector<gameplay::HealthState>&,
                        std::int32_t& score);
void pickup_effects(const gameplay::PhysicsEffects&, std::int32_t& healthMessageFrames, Events&);
// Arrow and Peyami are animated decorations in004ae0f0, NOT collectable healths.
std::string_view pickup_model(gameplay::LevelHealth::Kind) noexcept;
std::uint32_t decoration_animation(std::uint32_t wallClock) noexcept;
} // namespace yami::entity
