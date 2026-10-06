#include "combat.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
struct DeathState {
    std::vector<yami::gameplay::EntityState>& entities;
    int& score;
    bool observed = false;
};
void on_dead(void* opaque,std::string_view name) {
    auto& state=*static_cast<DeathState*>(opaque);
    const auto& enemy=state.entities[1];
    assert(name=="target" && enemy.dead && enemy.dying && enemy.health==0 && enemy.action==6);
    assert(state.score==10 && enemy.aiEnabled); // original BEFORE +20 and AI disable
    state.score+=7; // a real host/script score mutation must survive the return
    state.observed=true;
}
}
int main() {
    using namespace yami;
    std::vector<gameplay::EntityState> actors(2);
    actors[0].name="player"; actors[0].model="yaman_silahli"; actors[0].role=0;
    actors[0].weapon=1; actors[0].energy=1;
    actors[0].bounds.extents={5,5,5};
    actors[1].name="target"; actors[1].model="robot_utu"; actors[1].weapon=3;
    actors[1].position=actors[1].committedPosition=actors[1].previousPosition={0,20,-55};
    actors[1].bounds.extents={5,5,5};
    enemy_ai::Random random;
    std::vector<entity::Runtime> runtime;
    runtime.push_back(entity::initialize(actors[0].model,actors[0].position,random,100));
    runtime.push_back(entity::initialize(actors[1].model,actors[1].position,random,100));
    std::vector<gameplay::Obb> boxes;
    entity::Events events;
    int score=10; bool enabled=true;
    combat::Context context{actors,runtime,boxes,0,1,score,enabled,random,250,250,100,events};
    combat::World world;
    auto camera=identity_matrix(); camera.values[13]=20;
    assert(!combat::attack_player(world,context,15,camera,false)); // strict >250ms
    context.frameNow=251;
    actors[0].energy=combat::weapon(1).energyCost;
    assert(!combat::attack_player(world,context,15,camera,false)); // strict >cost
    actors[0].energy=1;
    assert(combat::attack_player(world,context,15,camera,false));
    assert(world.projectiles.size()==1 && actors[0].energy==0.75f);
    assert(actors[1].reactToHit); // constructor's unbounded sight query
    combat::tick_projectiles(world,context,264);
    assert(std::fabs(actors[1].health-0.8f)<0.000001f && world.projectiles[0].lifetime==0);
    assert(runtime[1].flashDraws==10 && score==10);
    DeathState state{actors,score};
    context.deathHost=&state; context.invokeOnDead=on_dead;
    const auto killed=combat::damage(world,context,1,1);
    assert(killed.died && !killed.invokeOnDead && state.observed);
    assert(score==37 && !actors[1].aiEnabled); // callback bonus7 plus post-hook20
    actors[1].dead=actors[1].dying=false; actors[1].health=1;
    context.invokeOnDead=nullptr;
    bool rejected=false;
    try { combat::damage(world,context,1,1); } catch (const std::runtime_error&) { rejected=true; }
    assert(rejected && actors[1].health==1 && !actors[1].dead); // fail BEFORE lethal mutation
    world.projectiles.clear(); world.meleeDelays.clear();
    actors[0].weapon=0; actors[0].action=0; actors[0].energy=1;
    world.lastPlayerAttack=0; context.frameNow=501;
    assert(combat::attack_player(world,context,15,camera,false));
    assert(world.meleeDelays.size()==1 && world.meleeDelays[0]==14 && world.projectiles.empty());
    for (int tick=0;tick<14;++tick) combat::tick_melee(world,context);
    assert(world.projectiles.empty() && world.meleeDelays[0]==0);
    combat::tick_melee(world,context);
    assert(world.projectiles.size()==1 && world.meleeDelays.empty() && world.projectiles[0].lifetime==1);
    // Projectile motion uses the owner's current weapon on EVERY tick, not its
    // launch weapon. This also checks exact expiration successor-skipping.
    world.projectiles={{0,{},{1000,1000,1000},{},{0,0,-1},0},
                       {0,{},{1000,1000,1000},{},{0,0,-1},3}};
    actors[0].weapon=2;
    combat::tick_projectiles(world,context,297);
    assert(world.projectiles.size()==1 && world.projectiles[0].lifetime==3 && world.projectiles[0].committed.z==1000);
    combat::tick_projectiles(world,context,330);
    assert(world.projectiles[0].committed.z==970 && world.projectiles[0].lifetime==2);
    actors[0].weapon=7;
    combat::tick_projectiles(world,context,363);
    assert(world.projectiles[0].committed.z==945);
    actors[0].weapon=1; actors[0].energy=1;
    combat::regenerate(actors[0],runtime[0],100);
    assert(runtime[0].regenerationStart==0); // full energy DOES NOT refresh clock
    actors[0].energy=0.75f;
    combat::regenerate(actors[0],runtime[0],110);
    assert(std::fabs(actors[0].energy-0.794f)<0.000001f && runtime[0].regenerationStart==110);
    std::cout<<"combat cooldown, energy, actual ray hit, synchronous death state, melee delay, live weapon and expiry verified\n";
}
