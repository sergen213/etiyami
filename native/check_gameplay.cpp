#include "gameplay.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a,float b,float epsilon=0.0001f) { return std::fabs(a-b)<=epsilon; }
}
int main(int argc,char** argv) {
    using namespace yami::gameplay;
    try {
        Obb box; box.center={5,0,0}; box.extents={1,1,1};
        check(intersects(box,{{3,0,0},{1,1,1}}),"SAT contact must be inclusive");
        check(!intersects(box,{{2.9f,0,0},{1,1,1}}),"SAT separated boxes overlap");
        float parameter=-1;
        check(intersect_ray({0,0,0},{2,0,0},box,parameter) && parameter==2,
              "Ray direction must remain unnormalized");
        check(intersect_ray({5,0,0},{0,0,0},box,parameter) && parameter==0,
              "Inside ray must return zero even for stationary direction");
        check(!intersect_ray({0,0,0},{-1,0,0},box,parameter),"Backward ray hit");
        const auto hit=sweep_box(box,{{0,0,0},{1,1,1}},{10,0,0});
        check(hit.hit && near(hit.fraction,0.3f) && hit.normal.x==1,"Recovered swept SAT");
        const auto overlapping=sweep_box(box,{{5,0,0},{1,1,1}},{0,0,0});
        check(overlapping.hit && near(overlapping.fraction,-1.01f),"Original stationary overlap fraction");
        check(!sweep_box(box,{{0,0,0},{1,1,1}},{-10,0,0}).hit,"Sweeping away from a separated box");
        std::vector<Obb> boxes{box};
        auto sweep=sweep_world({{0,0,0},{1,1,1}},{1,0,0},10,boxes,{}, {},1);
        check(sweep.kind==WorldSweep::Kind::Static && near(sweep.fraction,0.2f),"Original 0.1 fraction clearance");
        boxes[0].center={2.5f,0,0};
        Obb last=box; last.center={10,0,0}; boxes.push_back(last);
        sweep=sweep_world({{0,0,0},{1,1,1}},{1,0,0},10,boxes,{}, {},1);
        check(near(sweep.fraction,-0.05f),"Last successful candidate clamp must not be rewritten as nearest clamp");
        Vec3 normalized{3,4,0};
        const float length=normalize(normalized);
        check(near(length,5,0.001f) && near(normalized.x,0.6f,0.001f),"Original quantized sqrt table");
        Vec3 zero{}; check(normalize(zero)==0 && zero.x==0,"Zero normalization");
        EntityState player; player.role=0; player.model="yaman";
        PlayerInput input; input.forward=true;
        check(select_player_input(input,player).action==4,"W/Up forward action");
        input.right=true; input.fire=true;
        check(select_player_input(input,player).action==17,"Firing forward-right action");
        input={}; input.back=true; input.left=true;
        check(select_player_input(input,player).action==7,"Original back-left action quirk");
        for (const auto rightAxis : std::array<Vec3,4>{{{1,0,0},{0,0,1},{-1,0,0},{0,0,-1}}}) {
            for (const bool alternate : {false,true}) {
                for (const bool left : {false,true}) {
                    EntityState walker; walker.role=0; walker.model="yaman";
                    PlayerInput strafe;
                    if (alternate) { strafe.alternateLeft=left; strafe.alternateRight=!left; }
                    else { strafe.left=left; strafe.right=!left; }
                    const auto decision=select_player_input(strafe,walker);
                    ForceState motion;
                    request_movement(walker,motion,decision.moves[0],rightAxis);
                    const auto lateral=motion.direction.x*rightAxis.x+motion.direction.z*rightAxis.z;
                    check(left ? lateral<-.9f : lateral>.9f,
                          "A/Left must move screen-left and D/Right screen-right at every heading");
                }
            }
        }
        player.action=18; check(action_locked(player) && !movement_allowed(player),"Unarmed action lock");
        player.model="yaman_silahli"; player.weapon=1;
        check(!action_locked(player) && movement_allowed(player),"Armed action lock differs");
        player.action=13; player.actionCompleted=1;
        check(transition_action(player,player,0,123)==ActionTransition::PlayerDeathMenu && player.dead,
              "Completed player death must open death menu");
        FrameClock clock;
        clock.paused=false; clock.level_loaded(100); clock.begin_frame(166);
        unsigned ticks=0; while (clock.tick_due()) { clock.finish_tick(); ++ticks; }
        check(ticks==2 && clock.nextTick==166,"Fixed 33ms strict catch-up condition");
        SaveGame save; save.level=2; save.score=-13;
        save.entities.push_back({"player","yaman_silahli",{1.25f,2.5f,-3},0.75f,true,false});
        save.healths.push_back({{4,5,6},true}); save.scriptValues={7,-8};
        std::stringstream stream(std::ios::in|std::ios::out|std::ios::binary);
        write_save(stream,save); stream.seekg(0);
        const auto restored=read_save(stream,2);
        check(restored.score==-13 && restored.level==2 && restored.entities.size()==1
              && restored.entities[0].position.x==1.25f && !restored.entities[0].visible
              && restored.healths[0].big && restored.scriptValues[1]==-8,"Original binary/text save schema");
        check(checkpoint_state("data/save/save32.eti").playerPosition.has_value(),"Checkpoint 32 relocation");
        PlayerCameraState camera;
        Obb cameraBlock; cameraBlock.center={0,80,145}; cameraBlock.extents={2,2,2};
        solve_player_camera(yami::identity_matrix(),camera,100,{cameraBlock},{});
        check(camera.distance==130 && camera.distanceBase==130 && !camera.falling,"Camera shortening in three-unit steps");
        solve_player_camera(yami::identity_matrix(),camera,200,{},{});
        solve_player_camera(yami::identity_matrix(),camera,1000,{},{});
        check(camera.distance==145,"Camera delayed quadratic distance recovery");
        const auto flightDefaults=initialize_player_camera(4);
        check(flightDefaults.defaultPitch==-10 && flightDefaults.distance==250,
              "Level-four camera constructor differs from walking camera");
        const auto flightView=flight_camera(yami::identity_matrix(),PlayerCameraState{},FlightState{});
        check(flightView.values[13]==10 && flightView.values[14]==145,
              "Flying camera has ten-unit height, not walking camera eighty");
        EntityState body; body.role=0; body.model="yaman"; body.bounds.extents={1,1,1};
        ForceState force; set_force_rate(force,400);
        request_movement(body,force,Move::Forward,{1,0,0});
        std::vector<EntityState> entities{body}; std::vector<HealthState> healths;
        std::int32_t score=0;
        auto effects=tick_body(0,force,{0,-1,0},100,1,{},entities,healths,score);
        check(near(entities[0].position.z,-6,0.01f) && entities[0].position.y==-19.9f
              && !entities[0].movementRequested && effects.collectedCount==0,"Original horizontal/vertical fixed tick");
        body={}; body.role=0; body.bounds.extents={1,1,1}; body.health=0.5f;
        entities={body}; HealthState health; health.bounds.center={5,0,0}; health.bounds.extents={1,1,1};
        healths={health}; effects={};
        move_swept(0,{1,0,0},10,5,{0,-1,0},0,1,{},entities,healths,score,effects);
        check(healths[0].removed && near(entities[0].health,0.7f) && score==30
              && effects.collectedCount==1 && near(entities[0].committedPosition.x,10),"Pickup consumption/recursive remainder");
        EntityState airplane; airplane.role=0; airplane.model="ucak";
        airplane.position=airplane.previousPosition=airplane.committedPosition={-5450,147,-631};
        entities={airplane,EntityState{}}; healths.clear();
        FlightState flight; FlightInput flightInput;
        flightInput.keys.forward=true; flightInput.interact=true;
        auto flightInputEffects=apply_flight_input(entities[0],flight,flightInput,false);
        check(flight.verticalSpeed==-1.5f && flight.pitch==-1
              && !flightInputEffects.interact,"Flight fixed controls and nested interaction quirk");
        flightInput={}; flightInput.smoothedMouseX=1;
        flightInputEffects=apply_flight_input(entities[0],flight,flightInput,false);
        check(flightInputEffects.mouseActive && flight.lateralSpeed==0,"Flight mouse dead-zone active flag");
        float wavePhase=0;
        const auto flightEffects=tick_flight(0,1,flight,wavePhase,200,{},entities,healths,score);
        check(entities[0].position.x==-5450 && entities[0].committedPosition.x==-5448
              && entities[1].committedPosition.x==-5448 && near(entities[0].health,0.997f)
              && !flightEffects.showDeathMenu && !flightEffects.playEndingMovie,
              "Flight render-before-physics and passenger copy ordering");
        FlightState guardBug; guardBug.lateralSpeed=-499;
        request_flight_movement(entities[0],guardBug,Move::Forward);
        check(guardBug.verticalSpeed==0,"Flight vertical guard deliberately uses lateral speed");
        health.bounds.center={5,0,0}; health.bounds.extents={1,1,1}; health.removed=false;
        healths={health,health};
        check(sweep_world({{0,0,0},{1,1,1}},{1,0,0},10,{}, {},healths,3).index==0,
              "Initial pickup ties preserve exported order");
        append_health_scan_order(healths,0);
        check(sweep_world({{0,0,0},{1,1,1}},{1,0,0},10,{}, {},healths,3).index==1,
              "Respawned retained pickup must scan last without changing stable ID");
        if (argc==2) {
            const std::filesystem::path root=argv[1];
            for (int level=1;level<=4;++level) {
                const auto metadata=read_level_metadata(root/"data"/"levels"/("level"+std::to_string(level))/"level.xml");
                if (level==1)
                    check(metadata.parts.size()==399 && metadata.bots.size()==17
                          && metadata.healths.size()==12
                          && metadata.healths[8].kind==LevelHealth::Kind::Arrow
                          && metadata.bots[0].name=="bot_robot_7"
                          && near(metadata.bots[0].position.x,-149.6017f),
                          "Original level-one metadata survives ignored malformed camera tail");
                if (level==2)
                    check(metadata.parts.size()==404 && metadata.bots.size()==29
                          && metadata.healths.size()==17
                          && metadata.bots[18].name=="bot_robot_19"
                          && metadata.bots[24].name=="bot_robot_19",
                          "Original level-two metadata preserves duplicate entity names");
            }
        }
        std::cout << "Recovered gameplay checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
