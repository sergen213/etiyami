#include "entity.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace {
struct TriggerState {
    yami::gameplay::EntityState& player;
    std::vector<yami::gameplay::Obb>& boxes;
    int interactions = 0;
};
void trigger(void* context,std::string_view name,std::string_view suffix) {
    auto& state=*static_cast<TriggerState*>(context);
    assert(name=="first" && suffix=="_on_enter");
    ++state.interactions;
    // Simultaneous trigger callbacks can change the following trigger's enabled
    // byte. A precomputed event batch would incorrectly invoke the second box.
    state.boxes[1].disabled=true;
    state.player.position.x=100;
}
}
int main(int argc,char** argv) {
    using namespace yami;
    const auto root=std::filesystem::path(argc>1 ? argv[1] : "game");
    const auto modelFile=resolve_asset(root,"data/models/yaman/model.dat");
    const auto model=read_model(modelFile);
    assert(!model.parts.empty());
    const auto meshFile=mesh_path(modelFile,model.parts.front());
    const auto mesh=read_mesh(meshFile);
    assert(mesh.animation_names.size()>22); // all original player action ordinals
    for (const auto& name:mesh.animation_names) {
        const auto animation=read_animation(animation_path(meshFile,name),mesh);
        assert(entity::animation_duration(animation.frame_count)>0);
    }
    enemy_ai::Random random;
    auto runtime=entity::initialize("robot_soba",{12,80,9},random,100);
    assert(runtime.barrels==2 && runtime.muzzle.x==30 && runtime.muzzle.y==50 && runtime.muzzle.z==20);
    entity::rotate_world_x(runtime.transform,25);
    const auto upright=entity::upright_transform(runtime,40);
    assert(upright.values[13]==60 && upright.values[1]==0 && upright.values[5]==1 && upright.values[9]==0);
    gameplay::EntityState player; player.role=0;
    gameplay::EntityState enemy; enemy.role=1; enemy.action=6; enemy.dead=true;
    entity::Events events;
    const auto duration=entity::animation_duration(30);
    assert(duration==1000);
    entity::update_animation(enemy,player,30,1,967,random,events);
    assert(enemy.actionCompleted==1 && enemy.animationFrame==966);
    entity::update_animation(enemy,player,30,1,3000,random,events);
    assert(enemy.animationFrame==966 && enemy.actionCompleted==1); // frozen death pose
    runtime.flashDraws=10; enemy.health=0.4f;
    for (int i=0;i<10;++i) assert(entity::draw_light(enemy,runtime)[1]==0.4f);
    assert(entity::draw_light(enemy,runtime)[1]==1 && runtime.flashDraws==0);
    std::vector<gameplay::Obb> boxes(2);
    boxes[0].name="first"; boxes[1].name="second";
    boxes[0].extents=boxes[1].extents={1,1,1};
    TriggerState state{player,boxes};
    player.position={1,0,0};
    entity::triggers(player,boxes,false,&state,trigger);
    assert(state.interactions==0); // containment excludes exactly touching edge
    player.position={};
    entity::triggers(player,boxes,false,&state,trigger);
    assert(state.interactions==1); // synchronous mutation suppresses second trigger
    gameplay::LevelHealth source; source.kind=gameplay::LevelHealth::Kind::Big;
    source.position={1,2,3}; source.name="health";
    const auto healthModel=read_model(resolve_asset(root,"data/models/saglik_buyuk/model.dat"));
    auto health=entity::initialize_pickup(source,healthModel);
    assert(health.big && health.amount==0.3f && health.boost==1000);
    entity::PickupAnimation animation(500);
    entity::update_pickup(animation,533);
    assert(animation.elapsed==33 && animation.previousClock==533);
    std::vector<gameplay::HealthState> healths(2); healths[0]=health;
    healths[0].hidden=true; healths[0].removed=true;
    player.retainedBigHealth=0; player.bigHealthTicks=1;
    int score=50;
    entity::respawn_big_health(player,healths,score);
    assert(healths[0].hidden && player.bigHealthTicks==0);
    entity::respawn_big_health(player,healths,score);
    assert(!healths[0].hidden && !healths[0].removed && !player.retainedBigHealth && score==5);
    assert(healths[0].scanHead==1 && healths[1].scanNext==0); // original tail reinsertion
    std::cout<<"entity animation, death pose, flash draws, trigger mutation, pickup respawn verified\n";
}
