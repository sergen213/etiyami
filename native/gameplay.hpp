#pragma once

#include "assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yami::gameplay {

// Original units and handedness are retained: Y is vertical; no metre conversion.
// The model transform is owned by the renderer; these are its gameplay snapshots.
using Vec3 = ::yami::Vec3;
struct Obb {
    Vec3 center;
    std::array<Vec3, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec3 extents; // half extents, not full width (00409330, 0040a9f0)
    std::string name;
    bool disabled = false; // original byte +0x48
};
struct Aabb { Vec3 center, extents; };

// 00409330: all 15 separating axes, contact inclusive, no epsilon inflation.
bool intersects(const Obb&, const Aabb&) noexcept;
// 0040a9f0: unbounded forward ray, starts inside => distance 0. Direction need
// not be normalized; distance is the parameter of origin + distance*direction.
bool intersect_ray(Vec3 origin, Vec3 direction, const Obb&, float& distance) noexcept;

struct EntityState {
    std::string name, model;
    Vec3 position, previousPosition, committedPosition; // model; +0x14; +0xe4
    Obb bounds; // +0x20; center follows model position for collision queries
    float health = 1; // +0x80
    float energy = 1; // +0x90; not an integer ammunition count
    std::int32_t role = 1; // +0x84: 0 player, 1 other entity
    std::int32_t weapon = 0; // +0x8c
    std::int32_t action = 0; // +0xb4
    std::uint32_t animationFrame = 0; // +0xb8
    std::uint32_t actionStart = 0, actionCompleted = 0; // +0xbc, +0xc0
    std::uint32_t positionStart = 0; // +0x10
    bool dead = false, dying = false; // +8, +9: distinct original states
    bool hidden = false, aiEnabled = true, movementRequested = false;
    bool specialAttack = false; // player +0x185
    bool movementModeGuard = false, attackGuard = false, reactToHit = false; // +0x98,+0x99,+0xb0
    std::int32_t boostTicks = 0, bigHealthTicks = 0; // player +0x19c,+0x18c
    std::optional<std::size_t> retainedBigHealth; // level-three +0x25c
};
struct CollisionHit {
    enum class Kind { None, Static, Entity } kind = Kind::None;
    std::size_t index = 0;
    float distance = 0;
    Vec3 point;
};
// Preserves original scan/tie order. Caller owns both vectors and entity identity
// is the stable vector index; excludedEntity is optional (0040b3c0/0040ad80).
CollisionHit raycast(Vec3 origin, Vec3 direction, const std::vector<Obb>&,
                     const std::vector<EntityState>&,
                     std::optional<std::size_t> excludedEntity = {}, bool livingOnly = false) noexcept;
CollisionHit overlap(const Aabb&, const std::vector<Obb>&,
                     const std::vector<EntityState>&,
                     std::optional<std::size_t> excludedEntity = {}) noexcept;
void set_position(EntityState&, Vec3, bool commit, std::uint32_t frameNow) noexcept;
Vec3 interpolate(Vec3 previous, Vec3 current, std::uint32_t start,
                 std::uint32_t now) noexcept; // 00414630, game's invariant duration=33

// 004052a0 and 00405330; PlayerDeathMenu is a real required side effect, not a
// completed action. The caller must show original menu IDs 5,6,7 and message512.
enum class ActionTransition { Unchanged, Changed, PlayerDeathMenu };
bool action_locked(const EntityState&) noexcept;
ActionTransition transition_action(EntityState&, EntityState& player,
                                   std::int32_t next, std::uint32_t now) noexcept;
bool movement_allowed(const EntityState&) noexcept; // 00404600..00404850

struct PlayerInput {
    // Two complete key groups are deliberately separate: the original diagonal
    // test does not merge e.g. one arrow with one letter into a diagonal.
    bool forward = false, back = false, left = false, right = false;
    bool alternateForward = false, alternateBack = false;
    bool alternateLeft = false, alternateRight = false;
    bool fire = false, secondaryFire = false, special = false;
};
enum class Move { Forward, Back, Left, Right, Stop };
struct InputDecision {
    std::array<Move, 2> moves{{Move::Stop, Move::Stop}};
    std::size_t moveCount = 1;
    std::int32_t action = 0;
    bool attack = false;
    std::optional<std::int32_t> specialAction;
};
// Exact movement/action priority of 00416b60. Actual force application is separate
// from input selection; do not use a guessed velocity to execute this result.
InputDecision select_player_input(const PlayerInput&, const EntityState&) noexcept;

// Original game's startup state and unsigned millisecond timing, 00417390,
// 00417280, 004301a0. The render/input stage runs once after all fixed ticks.
enum class Phase : std::uint32_t { Menu = 0, Intro = 1, Loading = 2, Active = 3 };
struct FrameClock {
    Phase phase = Phase::Intro;
    bool paused = true;
    std::int32_t level = 0, previousLevel = -1;
    std::uint32_t levelStart = 0, nextTick = 0, frameNow = 0;
    void level_loaded(std::uint32_t now) noexcept;
    void begin_frame(std::uint32_t now) noexcept;
    bool tick_due() const noexcept;
    void finish_tick() noexcept;
    void open_menu() noexcept;
};

// Save format: LE score word followed by whitespace text; no magic/version and
// no script-variable count. Variables are serialized in VM registry order.
struct SavedEntity {
    std::string name, model;
    Vec3 position;
    float health = 0;
    bool alive = true, visible = true;
};
struct SavedHealth { Vec3 position; bool big = false; };
struct SaveGame {
    std::int32_t score = 0, level = 0;
    std::vector<SavedEntity> entities;
    std::vector<SavedHealth> healths;
    std::vector<std::int32_t> scriptValues;
};
// Malformed input throws runtime_error; reject original unsafe >64 entity saves.
// read_save consumes exactly the known variable count; original writer's final
// duplicate score token is intentionally not consumed, matching 00427930.
SaveGame read_save(std::istream&, std::size_t scriptVariableCount);
void write_save(std::ostream&, const SaveGame&);

struct NamedVisibility { std::string_view name; bool visible; };
struct NamedVariable { std::string_view name; std::int32_t value; };
struct CheckpointState {
    std::optional<std::int32_t> weapon;
    std::vector<NamedVisibility> visibility;
    std::vector<NamedVariable> variables;
    std::optional<Vec3> playerPosition;
    std::string_view restartMusic;
};
// 00427930: these path-specific changes are NOT present in the save byte stream.
CheckpointState checkpoint_state(std::string_view exactOriginalPath);

struct DamageResult {
    bool applied = false, died = false, invokeOnDead = false;
    ActionTransition action = ActionTransition::Unchanged;
};
// State/score portion of 00405600. Caller must perform its recovered audio and
// script side effects; death hook name is entity.name + "_on_dead".
DamageResult damage(EntityState&, EntityState& player, float amount,
                    std::int32_t& score, bool& levelEnabled, std::uint32_t now) noexcept;

struct LevelPart { std::string name, filename; };
struct LevelModel { std::string name, filename; ::yami::Mat4 transform; };
struct LevelBot { std::string name, model; Vec3 position; };
struct LevelHealth {
    enum class Kind { Small, Big, Arrow, Peyami } kind = Kind::Small;
    std::string name;
    Vec3 position;
};
struct LevelMetadata {
    Vec3 boundsMin, boundsMax, sphereCenter;
    float sphereRadius = 0;
    std::vector<LevelPart> parts;
    std::vector<LevelModel> models;
    std::vector<LevelBot> bots;
    std::vector<LevelHealth> healths;
    std::vector<Obb> blocking, triggers;
};
// Original 00418ae0 metadata only, not a substitute for entity/AI construction.
// WINDOWS-1254 input, libxml2 non-network parser. Original scene order retained.
LevelMetadata read_level_metadata(const std::filesystem::path&);
std::int32_t entity_type(std::string_view model) noexcept; // 00427350 dispatch
Vec3 level_player_start(std::int32_t level); // 0041a7e0
float recovered_length(Vec3) noexcept; // 00414590 + initializer 00414be0
float normalize(Vec3&) noexcept; // 004147b0, quantized original sqrt lookup
struct SweepHit { bool hit = false; float fraction = 0; Vec3 normal; };
// 004097f0 is NOT a conventional earliest-overlap SAT interval: it rejects each
// separated axis independently, then chooses max(gap/projected displacement).
SweepHit sweep_box(const Obb&, const Aabb&, Vec3 displacement) noexcept;

struct PlayerCameraState {
    float defaultPitch = 0; // +0x1bc
    float pitch = 0; // +0x1b8
    float distance = 145; // +0x1c4
    float distanceBase = 145; // +0x1c0
    float defaultDistance = 145; // +0x17c
    float acceleration = 2500; // +0x1d0
    std::uint32_t fallingSince = 0; // +0x1c8
    bool falling = false; // +0x1d4
};
PlayerCameraState initialize_player_camera(std::int32_t level) noexcept; // 0040c180
// Complete camera obstruction/distance recovery from unexported 0040b860;
// not the entity's separate force/gravity controller at +0xf0/+0x12c.
::yami::Mat4 solve_player_camera(const ::yami::Mat4& playerTransform,
                                PlayerCameraState&, std::uint32_t now,
                                const std::vector<Obb>&,
                                const std::vector<EntityState>&,
                                std::optional<std::size_t> playerIndex = {});

struct HealthState {
    Obb bounds;
    float amount = 0.2f;
    std::int32_t boost = 500;
    bool big = false, hidden = false, removed = false;
    // Stable IDs are vector indices; an implicit link means index+1 until a
    // respawn moves a node to the tail. scanHead is used only on element zero.
    std::size_t scanNext = static_cast<std::size_t>(-1), scanHead = 0;
};
void append_health_scan_order(std::vector<HealthState>&,std::size_t index);
struct WorldSweep {
    enum class Kind { None, Static, Entity, Health } kind = Kind::None;
    std::size_t index = 0;
    float fraction = 0;
    Vec3 normal;
};
// 0040aea0 scans pickups, entities (except level four), then static boxes.
// Its final clearance clamp intentionally tests the LAST successful candidate,
// not the nearest hit, reproducing the original order-dependent fraction bug.
WorldSweep sweep_world(const Aabb&,Vec3 direction,float distance,
                       const std::vector<Obb>&,const std::vector<EntityState>&,
                       const std::vector<HealthState>&,std::int32_t level,
                       std::optional<std::size_t> owner = {},bool livingOnly = true) noexcept;
struct ForceState {
    float rate = 400, decay = 400*0.033333335f;
    float stepDecay = (400*0.033333335f)*0.033333335f, halfStep = 400*0.00055555563f;
    float speed = 0, requestedSpeed = 185;
    std::int32_t ticks = 0;
    Vec3 direction; // aliases entity +0x110 in original
};
void set_force_rate(ForceState&,float rate) noexcept; // 00422910
void request_movement(EntityState&,ForceState&,Move,Vec3 modelRightAxis) noexcept;
// Pickup audio/menu side effects are returned, not replaced with fake callbacks.
// Uses fixed-size reporting because original recursive slide depth is exactly five.
struct PhysicsEffects {
    std::array<std::size_t,5> collected{};
    std::size_t collectedCount = 0;
};
void move_swept(std::size_t owner,Vec3 direction,float distance,std::int32_t depth,
                Vec3 gravityDirection,std::uint32_t frameNow,std::int32_t level,
                const std::vector<Obb>&,std::vector<EntityState>&,
                std::vector<HealthState>&,std::int32_t& score,PhysicsEffects&) noexcept;
PhysicsEffects tick_body(std::size_t owner,ForceState&,Vec3 gravityDirection,
                         std::uint32_t tickNow,std::int32_t level,
                         const std::vector<Obb>&,std::vector<EntityState>&,
                         std::vector<HealthState>&,std::int32_t& score) noexcept;
void initialize_entity(EntityState&,ForceState&,std::string_view model,const ::yami::Model&,
                       bool player,std::int32_t level,std::uint32_t now);

struct FlightState {
    float verticalSpeed = 0, lateralSpeed = 0, forwardStep = 2; // +260,+264,+268
    float roll = 0, pitch = 0; // +26c,+270
    bool left = false, right = false, forward = false, back = false; // +278..27b
};
// Original controls ignore the float argument. Vertical movement's bounds guard
// incorrectly uses LATERAL speed; do not replace it with a vertical clamp.
void request_flight_movement(EntityState&,FlightState&,Move) noexcept;
struct FlightInput {
    PlayerInput keys;
    float smoothedMouseX = 0, smoothedMouseY = 0; // 0046c0cc,0046c0c8
    bool mouseFire = false, interact = false;
    bool pauseHeld = false, pauseHeldPreviously = false;
};
struct FlightInputEffects {
    bool attack = false, interact = false, openMenu = false;
    bool mouseActive = false; // caller preserves as original global 004ae040
};
FlightInputEffects apply_flight_input(EntityState&,FlightState&,const FlightInput&,
                                      bool previousMouseActive) noexcept;
struct FlightEffects {
    PhysicsEffects pickups;
    bool showDeathMenu = false, playEndingMovie = false;
};
// 00408e50/00409120: one global wave phase survives level reload. Passenger state
// receives the primary flyer's resulting health/committed pose, but render pose
// is written before this tick's movement, exactly as in the original.
FlightEffects tick_flight(std::size_t owner,std::optional<std::size_t> passenger,
                          FlightState&,float& globalWavePhase,std::uint32_t now,
                          const std::vector<Obb>&,std::vector<EntityState>&,
                          std::vector<HealthState>&,std::int32_t& score) noexcept;
::yami::Mat4 flight_camera(const ::yami::Mat4& playerTransform,
                          const PlayerCameraState&,const FlightState&) noexcept; // 00408a70
} // namespace yami::gameplay
