#include "combat.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace yami::combat {
namespace {
constexpr std::array<Weapon,8> table{{
    {"melee","yaman_melee",80,0.35f,2,0.4f,0.85f,1},
    {"data/models/weapon/kursun_yaman/model.dat","lazer",17,0.2f,4,0.4f,0.25f,0xffffffffU},
    {"data/models/weapon/kursun_yaman_lazer/model.dat","foton_topu",30,0.4f,6,0.25f,0.17f,0xffffffffU},
    {"data/models/weapon/kursun_utu/model.dat","utu_ates",30,0.05f,5,1,0.2f,0xffffffffU},
    {"data/models/weapon/kursun_yayli/model.dat","cd_firlat",45,0.1f,10,1,0.1f,0xffffffffU},
    {"data/models/weapon/kursun_pervane/model.dat","pervane_lazer",30,0.75f,2,1,0.3f,0xffffffffU},
    {"data/models/weapon/kursun_bisiklet/model.dat","bisiklet_kursun",40,0.15f,8,1,0.3f,0xffffffffU},
    {"data/models/weapon/kursun_soba/model.dat","sobali_ates",25,0.5f,2,1,0.3f,0xffffffffU}
}};
constexpr std::array<std::array<std::string_view,2>,8> variants{{
    {"yaman_melee1","yaman_melee2"},{"lazer1","lazer2"},
    {"foton_topu1","foton_topu2"},{"utu_ates1","utu_ates2"},
    {"cd_firlat1","cd_firlat2"},{"pervane_lazer1","pervane_lazer2"},
    {"bisiklet_kursun1","bisiklet_kursun2"},{"sobali_ates1","sobali_ates2"}
}};
constexpr std::array<std::string_view,6> hitSounds{{"yaman_vurulma1","yaman_vurulma2","yaman_vurulma3","yaman_vurulma4","yaman_vurulma5","yaman_vurulma6"}};
Vec3 minus(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 negative(Vec3 v) { return {-v.x,-v.y,-v.z}; }
float squared(Vec3 a,Vec3 b) {
    const auto d=minus(a,b);
    return static_cast<float>(double(d.z)*d.z+double(d.x)*d.x+double(d.y)*d.y);
}
void check(Context& c) {
    if (c.entities.size()!=c.runtime.size() || c.player>=c.entities.size())
        throw std::runtime_error("combat entity/runtime identity mismatch");
}
void spawn(World& world,Context& c,std::size_t owner,Vec3 position,Vec3 direction) {
    gameplay::normalize(direction);
    Vec3 horizontal{direction.x,0,direction.z}; gameplay::normalize(horizontal);
    const float degrees=std::bit_cast<float>(0x42652ee0U);
    float angle=static_cast<float>(std::acos(double(horizontal.x))*degrees);
    if (horizontal.z<0) angle=360-angle;
    const float yaw=180-angle;
    world.projectiles.push_back({owner,position,position,position,direction,
                                 weapon(c.entities.at(owner).weapon).lifetimeTicks,yaw,yaw});
    if (c.entities[owner].weapon!=0) {
        const auto hit=gameplay::raycast(position,direction,c.blocking,c.entities,owner,false);
        if (hit.kind==gameplay::CollisionHit::Kind::Entity) c.entities[hit.index].reactToHit=true;
    }
}
Vec3 player_aim(Context& c,const Mat4& camera,Vec3 muzzle,bool automatic) {
    const auto& actor=c.entities[c.player];
    auto forward=negative(entity::forward_axis(c.runtime[c.player]));
    if (!automatic) {
        const Vec3 origin{camera.values[12],camera.values[13],camera.values[14]};
        const Vec3 ray{-camera.values[8],-camera.values[9],-camera.values[10]};
        const auto hit=gameplay::raycast(origin,ray,c.blocking,c.entities,c.player,false);
        if (hit.kind==gameplay::CollisionHit::Kind::None) return forward;
        auto direction=minus(hit.point,muzzle); gameplay::normalize(direction);
        if (direction.y>0.8f) { forward.y=0; return forward; }
        return direction;
    }
    float best=0.43f; std::optional<std::size_t> target; Vec3 direction;
    for (std::size_t i=0;i<c.entities.size();++i) {
        const auto& candidate=c.entities[i];
        if (candidate.health<=0 || candidate.hidden) continue;
        auto delta=minus(candidate.position,muzzle);
        // 0040bce0 deliberately replaces vertical delta with forward.y for the
        // angle test; actual selected firing direction retains the target's Y.
        Vec3 angleDirection{delta.x,forward.y,delta.z}; gameplay::normalize(angleDirection);
        const float angle=static_cast<float>(std::acos(double(dot(forward,angleDirection))));
        if (!(angle<best)) continue;
        const auto sight=gameplay::raycast(actor.position,minus(candidate.position,actor.position),
                                           c.blocking,c.entities,c.player,false);
        if (sight.kind==gameplay::CollisionHit::Kind::Entity && sight.index==i) {
            target=i; direction=delta; best=angle;
        }
    }
    if (target) { gameplay::normalize(direction); return direction; }
    return forward;
}
void menu_after_transition(gameplay::ActionTransition transition,entity::Events& events) {
    if (transition!=gameplay::ActionTransition::PlayerDeathMenu) return;
    events.push_back({entity::Event::Kind::Pause,{},1});
    for (int page:{5,6,7}) events.push_back({entity::Event::Kind::MenuActivate,{},page});
    events.push_back({entity::Event::Kind::MenuMessage,{},512,100});
}
}
const std::array<Weapon,8>& weapons() noexcept { return table; }
const Weapon& weapon(std::int32_t index) {
    if (index<0 || index>=static_cast<std::int32_t>(table.size()))
        throw std::runtime_error("invalid original weapon index");
    return table[static_cast<std::size_t>(index)];
}
void regenerate(gameplay::EntityState& actor,entity::Runtime& runtime,std::uint32_t now) {
    const auto& w=weapon(actor.weapon);
    if (actor.energy<1) {
        actor.energy=std::min(1.0f,static_cast<float>(double(now-runtime.regenerationStart)/1000*w.regeneration+actor.energy));
        runtime.regenerationStart=now;
    }
}
void select_weapon(gameplay::EntityState& actor,entity::Runtime& runtime,std::int32_t next,
                   std::int32_t level,entity::Events& events) {
    weapon(next);
    if (actor.weapon==next) return;
    if (actor.role==0 && ((level==1 && actor.weapon==0 && next==1) ||
                         (level==3 && actor.weapon==1 && next==2))) {
        runtime.alternateModel=true;
        events.push_back({entity::Event::Kind::ReplacePlayerModel,
                         next==1 ? "yaman_silahli" : "yaman_buyuk_silahli"});
    }
    actor.weapon=next; actor.energy=1;
}
void attack_enemy(World& world,Context& c,std::size_t owner) {
    check(c); auto& actor=c.entities.at(owner); auto& r=c.runtime.at(owner);
    if (actor.role==0 && actor.action==12 && actor.actionCompleted==0) return;
    const auto index=static_cast<std::size_t>(actor.weapon); const auto& w=weapon(actor.weapon);
    if (r.soundVariant<1 || r.soundVariant>2) throw std::runtime_error("invalid constructor attack sound variant");
    c.events.push_back({entity::Event::Kind::SoundPlay,variants[index][r.soundVariant-1],20});
    auto fire=[&] { const auto direction=entity::enemy_aim(r,c.entities[c.player].position);
                   spawn(world,c,owner,transform_point(r.transform,r.muzzle),direction); };
    if (r.barrels==0) fire();
    else if (r.barrels==1) { fire(); r.muzzle.x=-r.muzzle.x; }
    else if (r.barrels==2) { fire(); r.muzzle.x=-r.muzzle.x; fire(); }
    actor.energy=std::max(0.0f,actor.energy-w.energyCost);
}
bool attack_player(World& world,Context& c,std::int32_t requested,const Mat4& camera,bool automatic) {
    check(c); auto& actor=c.entities[c.player]; const auto& w=weapon(actor.weapon);
    if (actor.action>14 && actor.action<22 && actor.weapon==0) return false;
    if (c.frameNow-world.lastPlayerAttack<=1000U/w.attacksPerSecond || actor.energy<=w.energyCost) return false;
    auto sound=w.sound;
    if (actor.weapon==0) { c.random.state=c.unixSeconds; sound=variants[0][c.random.next()%2]; }
    c.events.push_back({entity::Event::Kind::SoundPlay,sound,50});
    if (actor.weapon==0) {
        switch (requested) {
        case 15:case 19:world.meleeDelays.push_back(14);break;
        case 16:case 17:case 21:world.meleeDelays.push_back(22);break;
        case 18:case 20:
            world.meleeDelays.push_back(10); world.meleeDelays.push_back(19); world.meleeDelays.push_back(41);break;
        default:break; // original consumes energy and sound even without a delayed strike
        }
    } else {
        const auto muzzle=entity::player_muzzle(c.runtime[c.player],actor.position);
        spawn(world,c,c.player,muzzle,player_aim(c,camera,muzzle,automatic));
    }
    actor.energy=std::max(0.0f,actor.energy-w.energyCost); world.lastPlayerAttack=c.frameNow;
    return true;
}
void apply_player_input(World& world,Context& c,gameplay::ForceState& force,
                         const gameplay::PlayerInput& input,const Mat4& camera,bool automatic) {
    check(c); auto& player=c.entities[c.player];
    const auto decision=gameplay::select_player_input(input,player);
    for (std::size_t i=0;i<decision.moveCount;++i)
        entity::move(player,force,c.runtime[c.player],decision.moves[i]);
    if (decision.attack) attack_player(world,c,decision.action,camera,automatic);
    entity::action(player,player,decision.action,c.wallClock,c.events);
    if (input.special) special_attack(c);
}
void tick_melee(World& world,Context& c) {
    check(c);
    for (std::size_t i=0;i<world.meleeDelays.size();) {
        if (world.meleeDelays[i]!=0) { --world.meleeDelays[i]; ++i; continue; }
        if (c.entities[c.player].weapon==0) {
            auto direction=negative(entity::forward_axis(c.runtime[c.player])); direction.y=0;
            spawn(world,c,c.player,c.entities[c.player].position,direction);
        }
        world.meleeDelays.erase(world.meleeDelays.begin()+static_cast<std::ptrdiff_t>(i));
    }
}
gameplay::DamageResult damage(World& world,Context& c,std::size_t target,float amount) {
    check(c); auto& actor=c.entities.at(target);
    if (actor.role!=0 && !actor.dead && !actor.dying && actor.health-amount<=0 && !c.invokeOnDead)
        throw std::runtime_error("lethal combat requires synchronous on_dead VM executor");
    const auto scoreBefore=c.score; const bool aiBefore=actor.aiEnabled;
    auto result=gameplay::damage(actor,c.entities[c.player],amount,c.score,c.levelEnabled,c.wallClock);
    if (!result.applied) return result;
    c.runtime[target].flashDraws=10;
    menu_after_transition(result.action,c.events);
    if (actor.role==0) {
        const auto sound=hitSounds[c.random.next()%6];
        if (c.random.next()%2==0) c.events.push_back({entity::Event::Kind::SoundPlay,sound,100});
    } else if (c.entities[c.player].weapon==0) {
        c.random.state=c.unixSeconds; const auto variant=c.random.next()%2;
        if (world.firstMeleeHit) { c.events.push_back({entity::Event::Kind::SoundPlay,"yaman_ara2",100}); world.firstMeleeHit=false; }
        c.events.push_back({entity::Event::Kind::SoundPlay,variant==0 ? "yaman_melee_vur1" : "yaman_melee_vur2",100});
    } else {
        // The original still tries the unregistered unsuffixed melee-hit name.
        c.events.push_back({entity::Event::Kind::SoundPlay,"yaman_melee_vur",100});
    }
    if (result.died && actor.role==0 && c.level!=4)
        c.events.push_back({entity::Event::Kind::SoundPlay,"yaman_ol",100});
    if (result.invokeOnDead) {
        // Gameplay's state-only port advances these two fields. Restore their
        // original callback-visible values, execute the real VM, then advance.
        c.score=scoreBefore; actor.aiEnabled=aiBefore;
        c.invokeOnDead(c.deathHost,actor.name);
        c.score+=20; actor.aiEnabled=false;
        result.invokeOnDead=false; // consumed synchronously, never dispatch twice
    }
    return result;
}
void tick_projectiles(World& world,Context& c,std::uint32_t tick) {
    check(c); world.projectileTickStart=tick;
    // 00426126 sets iterator to successor, then0042612b advances it again: the
    // projectile immediately after an expired projectile skips this tick.
    for (std::size_t i=0;i<world.projectiles.size();++i) {
        auto& p=world.projectiles[i];
        if (p.lifetime==0) { world.projectiles.erase(world.projectiles.begin()+static_cast<std::ptrdiff_t>(i)); continue; }
        const auto& w=weapon(c.entities.at(p.owner).weapon); // live OWNER weapon, not captured at fire
        p.previous=p.committed;
        p.committed={p.committed.x+w.step*p.direction.x,p.committed.y+w.step*p.direction.y,p.committed.z+w.step*p.direction.z};
        --p.lifetime;
        const auto hit=gameplay::raycast(p.previous,p.direction,c.blocking,c.entities,p.owner,false);
        if (hit.kind==gameplay::CollisionHit::Kind::None ||
            !(squared(hit.point,p.previous)<squared(p.previous,p.committed)*4)) continue;
        if (hit.kind==gameplay::CollisionHit::Kind::Static) p.lifetime=0;
        else if (!c.entities[hit.index].dead && !c.entities[hit.index].hidden && !c.entities[c.player].specialAttack) {
            damage(world,c,hit.index,w.damage); p.lifetime=0;
        }
    }
}
void interpolate_projectiles(World& world,std::uint32_t now) noexcept {
    for (auto& p:world.projectiles)
        p.rendered=gameplay::interpolate(p.previous,p.committed,world.projectileTickStart,now);
}
Mat4 projectile_transform(const Projectile& projectile) noexcept {
    auto matrix=identity_matrix(); entity::rotate_world_y(matrix,projectile.yaw);
    matrix.values[12]=projectile.rendered.x; matrix.values[13]=projectile.rendered.y; matrix.values[14]=projectile.rendered.z;
    return matrix;
}
void special_attack(Context& c) {
    check(c); auto& player=c.entities[c.player];
    if (player.boostTicks==0) { entity::action(player,player,10,c.wallClock,c.events); return; }
    entity::action(player,player,22,c.wallClock,c.events); player.specialAttack=true;
    if (c.level!=3 || c.entities.empty()) return;
    // The first actor is not searched/replaced by the closest one.
    auto& first=c.entities.front();
    if (first.hidden || first.dead || std::fabs(first.position.x-player.position.x)>=100 ||
        std::fabs(first.position.z-player.position.z)>=100) return;
    if (!c.invokeOnDead) throw std::runtime_error("special kill requires synchronous on_dead VM executor");
    first.health=0; first.dead=true;
    entity::action(first,player,6,c.wallClock,c.events); first.aiEnabled=false;
    c.invokeOnDead(c.deathHost,first.name); c.score+=20;
    c.events.push_back({entity::Event::Kind::Video,"robot_dusme",25});
}
} // namespace yami::combat
