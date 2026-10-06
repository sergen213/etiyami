#include "entity.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace yami::entity {
namespace {
float bits(std::uint32_t value) { return std::bit_cast<float>(value); }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 negative(Vec3 v) { return {-v.x,-v.y,-v.z}; }
void rotation(Mat4& matrix,float degrees,Vec3 axis) {
    // 00423880 uses real sqrt, unlike the game's quantized vector normalization.
    const double length=std::sqrt(double(axis.x)*axis.x+double(axis.y)*axis.y+double(axis.z)*axis.z);
    if (length<0.0001) return;
    const double radians=double(degrees)*bits(0x3c8efa35U);
    const float s=static_cast<float>(std::sin(radians)),c=static_cast<float>(std::cos(radians));
    const float x=static_cast<float>(axis.x/length),y=static_cast<float>(axis.y/length),z=static_cast<float>(axis.z/length),t=1-c;
    auto r=identity_matrix();
    r.values[0]=x*x*t+c; r.values[1]=x*y*t+s*z; r.values[2]=x*z*t-s*y;
    r.values[4]=x*y*t-s*z; r.values[5]=y*y*t+c; r.values[6]=y*z*t+s*x;
    r.values[8]=x*z*t+s*y; r.values[9]=y*z*t-s*x; r.values[10]=z*z*t+c;
    matrix=multiply(matrix,r);
}
void death_menu(Events& events) {
    events.push_back({Event::Kind::Pause,{},1});
    for (int page:{5,6,7}) events.push_back({Event::Kind::MenuActivate,{},page});
    events.push_back({Event::Kind::MenuMessage,{},512,100});
}
}
Runtime initialize(std::string_view model,Vec3 position,enemy_ai::Random& random,std::uint32_t seconds) {
    Runtime r;
    random.state=seconds; r.soundVariant=static_cast<std::uint8_t>(random.next()%2+1);
    r.pitchLimit=bits(0x3eb2b7feU); r.yawLimit=bits(0x3e32b7feU);
    if (model=="robot_utu") { r.muzzle={0,30,0}; r.horizontalAim=true; }
    else if (model=="robot_yayli") r.muzzle={0,10,0};
    else if (model=="robot_bisiklet") { r.muzzle={20,45,10}; r.barrels=1; }
    else if (model=="robot_pervane") { r.muzzle={10,-5,0}; r.barrels=1; r.horizontalAim=true; }
    else if (model=="robot_soba") { r.muzzle={30,50,20}; r.barrels=2; }
    r.transform.values[12]=position.x; r.transform.values[13]=position.y; r.transform.values[14]=position.z;
    return r;
}
void synchronize_position(Runtime& r,const gameplay::EntityState& e) noexcept {
    r.transform.values[12]=e.position.x; r.transform.values[13]=e.position.y; r.transform.values[14]=e.position.z;
}
Vec3 right_axis(const Runtime& r) noexcept { const auto& m=r.transform.values; return {m[0],m[1],m[2]}; }
Vec3 forward_axis(const Runtime& r) noexcept { const auto& m=r.transform.values; return {m[8],m[9],m[10]}; }
void rotate_world_x(Mat4& m,float degrees) noexcept { rotation(m,degrees,{1,0,0}); }
void rotate_world_y(Mat4& m,float degrees) noexcept { rotation(m,degrees,{0,1,0}); }
void rotate_world_z(Mat4& m,float degrees) noexcept { rotation(m,degrees,{0,0,1}); }
void rotate_local_up(Mat4& m,float degrees) noexcept { rotation(m,degrees,{m.values[1],m.values[5],m.values[9]}); }
void turn(Runtime& r,float degrees) noexcept { rotate_local_up(r.transform,degrees); }
void look(Runtime& r,float yaw,float pitch) noexcept {
    if (yaw!=0) turn(r,yaw);
    const float next=r.playerPitch+pitch;
    if (pitch!=0 && next<40 && next>=-70) { r.playerPitch=next; rotate_world_x(r.transform,pitch); }
}
void move(gameplay::EntityState& e,gameplay::ForceState& f,const Runtime& r,gameplay::Move direction) noexcept {
    gameplay::request_movement(e,f,direction,right_axis(r));
}
gameplay::ActionTransition action(gameplay::EntityState& e,gameplay::EntityState& player,
                                  std::int32_t next,std::uint32_t now,Events& events) {
    const auto result=gameplay::transition_action(e,player,next,now);
    if (result==gameplay::ActionTransition::PlayerDeathMenu) death_menu(events);
    return result;
}
std::uint32_t animation_duration(std::uint32_t frames) {
    if (!frames || frames>0x7fffffffU) throw std::runtime_error("invalid original entity animation frame count");
    const double milliseconds=double(frames)*double(bits(0x3d088889U))*1000.0;
    if (milliseconds>4294967295.0) throw std::runtime_error("entity animation duration overflow");
    return static_cast<std::uint32_t>(milliseconds); //00405464 FILD/FMUL/FMUL/FIST trunc
}
void update_animation(gameplay::EntityState& e,gameplay::EntityState& player,
                      std::uint32_t frames,std::int32_t level,std::uint32_t now,
                      enemy_ai::Random& random,Events& events) {
    const auto duration=animation_duration(frames);
    if (e.dead && e.actionCompleted>0) return;
    e.animationFrame+=now-e.actionStart; e.actionStart=now;
    // The original wraps at duration-one frame, increments ONCE even for a long
    // stall, and uses full duration for the remainder. Dead actors hold that frame.
    const float end=static_cast<float>(static_cast<float>(duration)-bits(0x42055555U));
    if (static_cast<double>(e.animationFrame)>end) {
        ++e.actionCompleted; e.animationFrame%=duration;
        if (e.dead) e.animationFrame=static_cast<std::uint32_t>(end);
        if (e.role==0 && level!=4 && e.action==0 && e.actionCompleted%5==0)
            action(e,player,static_cast<std::int32_t>(random.next()%3+1),now,events);
    }
}
void update_render(gameplay::EntityState& e,Runtime& r,std::uint32_t now) noexcept {
    const auto delta=sub(e.previousPosition,e.committedPosition);
    if (std::fabs(delta.x)>0.0001f || std::fabs(delta.y)>0.0001f || std::fabs(delta.z)>0.0001f)
        gameplay::set_position(e,gameplay::interpolate(e.previousPosition,e.committedPosition,e.positionStart,now),false,now);
    synchronize_position(r,e);
}
std::array<float,4> draw_light(const gameplay::EntityState& e,Runtime& r) noexcept {
    if (r.flashDraws>0) { --r.flashDraws; return {1,e.health,0,0}; }
    return {1,1,1,0};
}
Mat4 upright_transform(const Runtime& r,float height) noexcept {
    auto matrix=r.transform; auto& m=matrix.values;
    Vec3 up{0,1,0},forward{m[2],m[6],m[10]};
    auto right=cross(up,forward); forward=cross(right,up);
    gameplay::normalize(right); gameplay::normalize(up); gameplay::normalize(forward);
    m[0]=right.x; m[4]=right.y; m[8]=right.z;
    m[1]=up.x; m[5]=up.y; m[9]=up.z;
    m[2]=forward.x; m[6]=forward.y; m[10]=forward.z;
    m[13]-=height*0.5f;
    return matrix;
}
Vec3 enemy_aim(Runtime& r,Vec3 target) noexcept {
    auto forward=negative(forward_axis(r));
    const auto muzzle=transform_point(r.transform,r.muzzle);
    auto difference=sub(target,muzzle); gameplay::normalize(difference);
    r.aimPitch=std::clamp(static_cast<float>(std::atan(double(difference.y)-forward.y)),-r.pitchLimit,r.pitchLimit);
    if (!r.horizontalAim) {
        forward.y=0; difference=sub(target,muzzle); difference.y=0;
        gameplay::normalize(forward); gameplay::normalize(difference);
        float yaw=static_cast<float>(std::acos(double(dot(forward,difference))));
        if (difference.z*forward.x-difference.x*forward.z<0) yaw=-yaw;
        r.aimYaw=std::clamp(yaw,-r.yawLimit,r.yawLimit);
        const float c=static_cast<float>(std::cos(r.aimYaw)),s=static_cast<float>(std::sin(r.aimYaw));
        forward={forward.x*c-forward.z*s,static_cast<float>(std::tan(r.aimPitch)),forward.z*c+forward.x*s};
    } else { forward=negative(forward_axis(r)); forward.y=static_cast<float>(std::tan(r.aimPitch)); }
    gameplay::normalize(forward); return forward;
}
Vec3 player_muzzle(const Runtime& r,Vec3 position) noexcept {
    const auto right=right_axis(r),front=cross(right,{0,1,0});
    return {position.x+right.x*8-front.x*30,position.y+right.y*8+20-front.y*30,
            position.z+right.z*8-front.z*30};
}
void triggers(const gameplay::EntityState& player,const std::vector<gameplay::Obb>& boxes,bool use,
               void* host,void (*invokeEvent)(void*,std::string_view,std::string_view)) {
    if (!invokeEvent) throw std::runtime_error("triggers require synchronous VM event executor");
    for (const auto& box:boxes) {
        if (box.disabled) continue;
        const auto d=sub(player.position,box.center);
        if (std::fabs(dot(d,box.axes[0]))<box.extents.x &&
            std::fabs(dot(d,box.axes[1]))<box.extents.y &&
            std::fabs(dot(d,box.axes[2]))<box.extents.z)
            invokeEvent(host,box.name,use ? "_on_use" : "_on_enter");
    }
}
void update_pickup(PickupAnimation& animation,std::uint32_t now) noexcept {
    animation.elapsed+=now-animation.previousClock; animation.previousClock=now;
}
gameplay::HealthState initialize_pickup(const gameplay::LevelHealth& source,const Model& model) {
    if (source.kind!=gameplay::LevelHealth::Kind::Small && source.kind!=gameplay::LevelHealth::Kind::Big)
        throw std::runtime_error("decorative model is not a health pickup");
    gameplay::HealthState health;
    health.bounds.center=source.position; health.bounds.extents=model_half_extents(model);
    health.bounds.name=source.name;
    health.big=source.kind==gameplay::LevelHealth::Kind::Big;
    health.amount=health.big ? 0.3f : 0.2f; health.boost=health.big ? 1000 : 500;
    return health;
}
void update_flight_pickups(FlightPickupMotion& motion,std::span<const PickupRuntime> runtime,
                           std::vector<gameplay::HealthState>& pickups) {
    if (runtime.size()!=pickups.size()) throw std::runtime_error("pickup runtime count mismatch");
    motion.phase+=0.1f; motion.offset.x-=20;
    motion.offset.y=motion.offset.z=static_cast<float>(std::sin(double(motion.phase))*bits(0xbddd67c9U));
    for (std::size_t i=0;i<pickups.size();++i) {
        auto& health=pickups[i]; if (health.removed || health.hidden) continue;
        const auto p=runtime[i].originalPosition;
        health.bounds.center={p.x+motion.offset.x,p.y+motion.offset.y,p.z+motion.offset.z};
    }
}
void respawn_big_health(gameplay::EntityState& player,std::vector<gameplay::HealthState>& pickups,std::int32_t& score) {
    if (player.bigHealthTicks!=0) { --player.bigHealthTicks; return; }
    if (player.retainedBigHealth && *player.retainedBigHealth>=pickups.size())
        throw std::runtime_error("retained health identity out of range");
    if (player.retainedBigHealth) {
        auto& health=pickups[*player.retainedBigHealth];
        if (health.hidden) {
            const auto restored=*player.retainedBigHealth;
            health.hidden=false; health.removed=false; player.retainedBigHealth.reset();
            gameplay::append_health_scan_order(pickups,restored);
            if (score-45>0) score-=45;
        }
    }
}
void pickup_effects(const gameplay::PhysicsEffects& effects,std::int32_t& frames,Events& events) {
    for (std::size_t i=0;i<effects.collectedCount;++i) {
        events.push_back({Event::Kind::SoundPlay,"yami",100});
        frames=45; events.push_back({Event::Kind::MenuActivate,{},2});
    }
}
std::string_view pickup_model(gameplay::LevelHealth::Kind kind) noexcept {
    switch (kind) {
    case gameplay::LevelHealth::Kind::Small:return "data/models/saglik/model.dat";
    case gameplay::LevelHealth::Kind::Big:return "data/models/saglik_buyuk/model.dat";
    case gameplay::LevelHealth::Kind::Arrow:return "data/models/ok/model.dat";
    case gameplay::LevelHealth::Kind::Peyami:return "data/models/peyami/model.dat";
    }
    return {};
}
std::uint32_t decoration_animation(std::uint32_t now) noexcept { return now; }
} // namespace yami::entity
