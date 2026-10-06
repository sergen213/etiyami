#include "game.hpp"
#include "combat.hpp"
#include "script_vm.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace yami {
namespace {
using namespace gameplay;
std::uint32_t seconds() { return static_cast<std::uint32_t>(std::time(nullptr)); }
Mat4 inverse_pose(const Mat4& pose) {
    auto result=identity_matrix();
    for (int column=0;column<3;++column) for (int row=0;row<3;++row)
        result.values[column*4+row]=pose.values[row*4+column];
    const Vec3 p{pose.values[12],pose.values[13],pose.values[14]};
    const auto t=transform_direction(result,p);
    result.values[12]=-t.x; result.values[13]=-t.y; result.values[14]=-t.z;
    return result;
}
std::string descriptor(std::string_view model) { return "data/models/"+std::string(model)+"/model.dat"; }
menu::Draw ui_quad(menu::Rect rect,std::string_view material) {
    menu::Draw d; d.rect=rect; d.material=material;
    d.positions={{{rect.x,rect.y+rect.height},{rect.x+rect.width,rect.y+rect.height},
                  {rect.x+rect.width,rect.y},{rect.x,rect.y}}};
    d.uv={{{0,0},{1,0},{1,1},{0,1}}}; return d;
}
}
struct Game::State {
    Game& game;
    FrameClock clock;
    LevelMetadata metadata;
    struct Source { std::string name; Mat4 transform; bool hidden=false;
                    std::unique_ptr<ModelInstance> instance; };
    std::vector<Source> sources,decorations;
    std::vector<EntityState> actors;
    std::vector<entity::Runtime> runtime;
    std::vector<ForceState> forces;
    std::vector<enemy_ai::State> ai;
    std::vector<std::unique_ptr<ModelInstance>> models;
    std::unique_ptr<ModelInstance> alternatePlayer;
    std::array<std::unique_ptr<ModelInstance>,8> projectileModels;
    std::vector<HealthState> pickups;
    std::vector<entity::PickupRuntime> pickupRuntime;
    std::vector<std::unique_ptr<ModelInstance>> pickupModels;
    enemy_ai::Random random;
    enemy_ai::MotionHistory history;
    std::vector<enemy_ai::Operation> operations;
    combat::World combat;
    entity::Events events;
    script::Runtime vm;
    struct Variable { std::string name; std::uint32_t word=0; };
    std::vector<Variable> variables;
    std::vector<std::uint32_t> ownedStrings;
    PlayerCameraState cameraState;
    Mat4 camera=identity_matrix(),pausePose=identity_matrix();
    FlightState flight,passengerFlight;
    entity::FlightPickupMotion pickupMotion;
    float wavePhase=0,sensitivity=0.5f,pausePitchStep=0.1f;
    std::array<Vec2,10> mouseHistory{};
    std::uint32_t mouseSample=0,mouseIdle=0;
    Vec2 smoothedMouse{};
    std::int32_t score=0,healthMessageFrames=0,lastScore=0;
    bool automaticAim=true,focused=true,quit=false,previousEscape=false,mouseActive=false;
    bool levelEnabled=true,overlay=false,objectivesVisible=false;
    int operationDepth=0;
    std::uint32_t levelGeneration=0;
    std::optional<std::pair<int,bool>> pendingLevel;
    std::string movie,lastCheckpoint;
    std::uint32_t movieStart=0;
    std::uint32_t wallNow=0,clockOffset=0;
    std::optional<std::uint32_t> menuPauseWall;
    std::optional<std::size_t> passenger;
    std::vector<menu::Objective> objectives;
    std::vector<menu::Command> commands;
    std::array<std::uint8_t,4> saveCounts{};
    explicit State(Game& owner):game(owner) { random.state=seconds(); events.reserve(32); operations.reserve(32); }
    std::unique_ptr<ModelInstance> instance(std::string_view path,bool animated) {
        return std::make_unique<ModelInstance>(game.renderer_,game.scene_.model(path),animated);
    }
    void clear_registry() {
        for (auto address:ownedStrings) vm.release_string(address);
        ownedStrings.clear(); variables.clear(); vm.clear_callables();
    }
    Variable* variable(std::string_view name) {
        for (auto& value:variables) if (value.name==name) return &value;
        return nullptr;
    }
    void set_variable(std::string name,std::uint32_t word) {
        if (auto* old=variable(name)) { old->word=word; return; }
        variables.push_back({std::move(name),word});
    }
    void seed_callable_words() {
        for (const auto name:vm.ordered_callable_names())
            if (!variable(name)) variables.push_back({std::string(name),0});
    }
    void invoke(std::string_view name) { ++operationDepth; vm.invoke(name); --operationDepth; settle(); }
    void invoke_event(std::string_view name,std::string_view suffix) {
        ++operationDepth; vm.invoke_event(name,suffix); --operationDepth; settle();
    }
    void settle() {
        if (operationDepth || !pendingLevel) return;
        const auto request=*pendingLevel; pendingLevel.reset(); build_level(request.first,request.second);
    }
    void request_level(int level,bool initialize) {
        if (level<1 || level>4) throw std::runtime_error("Original level index outside 1..4");
        clear_registry(); clock.phase=Phase::Loading; clock.paused=false;
        for (auto& box:metadata.triggers) box.disabled=true;
        pendingLevel={{level,initialize}}; settle();
    }
    void add_actor(std::string name,std::string model,Vec3 position,bool player) {
        auto gpu=instance(descriptor(model),true);
        EntityState actor; actor.name=std::move(name); ForceState force;
        initialize_entity(actor,force,model,gpu->resource().model,player,clock.level,clock.frameNow);
        set_position(actor,position,true,clock.frameNow);
        actors.push_back(std::move(actor)); forces.push_back(force);
        runtime.push_back(entity::initialize(model,position,random,seconds()));
        ai.push_back(enemy_ai::initialize(static_cast<enemy_ai::Kind>(entity_type(model)),position));
        ai.back().enabled=!player; actors.back().aiEnabled=!player;
        models.push_back(std::move(gpu));
    }
    void replace_model(std::size_t index,std::string_view model) {
        auto gpu=instance(descriptor(model),true);
        actors.at(index).model=model; models.at(index)=std::move(gpu);
        // 00406140 replaces the draw model only, not force/weapon/health or the
        // entity's original collision extents and muzzle controller.
        runtime[index].transform=identity_matrix();
        entity::synchronize_position(runtime[index],actors[index]);
    }
    void visibility(std::string_view name,bool visible) {
        for (std::size_t i=0;i<actors.size();++i) if (actors[i].name==name) {
            actors[i].hidden=!visible; actors[i].aiEnabled=visible;
            ai[i].enabled=visible; return;
        }
        for (auto& source:sources) if (source.name==name) { source.hidden=!visible; return; }
        for (auto& box:metadata.triggers) if (box.name==name) { box.disabled=!visible; return; }
        for (auto& box:metadata.blocking) if (box.name==name) { box.disabled=!visible; return; }
        for (auto& health:pickups) if (health.bounds.name==name) { health.hidden=!visible; return; }
        for (auto& source:decorations) if (source.name==name) { source.hidden=!visible; return; }
    }
    std::filesystem::path save_path(std::string_view original) const {
        const auto filename=std::filesystem::path(original).filename();
        const auto value=filename.string();
        if (value.size()!=10 || value.substr(0,4)!="save" || value.substr(6)!=".eti"
            || value[4]<'1' || value[4]>'4' || value[5]<'1' || value[5]>'4')
            throw std::runtime_error("Invalid original checkpoint filename: "+std::string(original));
        auto path=game.saveDirectory_/filename;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
            throw std::runtime_error("Native checkpoint may not be a symlink");
        return path;
    }
    void save(std::string_view name) {
        SaveGame saved; saved.level=clock.level; saved.score=score;
        for (const auto& actor:actors) saved.entities.push_back({actor.name,actor.model,
            actor.committedPosition,actor.health,!actor.dead,!actor.hidden});
        for (const auto& health:pickups) if (!health.removed)
            saved.healths.push_back({health.bounds.center,health.big});
        for (const auto& value:variables) saved.scriptValues.push_back(std::bit_cast<std::int32_t>(value.word));
        const auto destination=save_path(name);
        auto serial=clock.frameNow;
        auto temporary=destination;
        do { temporary=destination; temporary+=".tmp-"+std::to_string(serial++); }
        while (std::filesystem::exists(std::filesystem::symlink_status(temporary)));
        try {
            std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
            if (!file) throw std::runtime_error("Cannot write native checkpoint");
            write_save(file,saved); file.flush(); file.close();
            if (!file) throw std::runtime_error("Cannot finish native checkpoint");
            std::filesystem::rename(temporary,destination);
        } catch (...) {
            std::error_code ignored; std::filesystem::remove(temporary,ignored); throw;
        }
        lastCheckpoint=name; game.menu_.show_notification(4);
        saveCounts=menu::checkpoint_counts(game.saveDirectory_);
    }
    void checkpoint(std::string_view name) {
        // Original restore reads the host/function registry prefix without
        // invoking on_start; named checkpoint adjustments establish its variables.
        const std::string original(name);
        std::ifstream file(save_path(name),std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read native checkpoint: "+original);
        std::array<char,4> scoreWord{}; file.read(scoreWord.data(),4);
        int level=0; file>>level; file.clear(); file.seekg(0);
        if (!file) throw std::runtime_error("Cannot read native checkpoint level");
        request_level(level,false);
        auto saved=read_save(file,variables.size()); score=saved.score;
        for (std::size_t i=0;i<saved.scriptValues.size();++i) variables[i].word=std::bit_cast<std::uint32_t>(saved.scriptValues[i]);
        // The save reader reconstructs actors in stream order, not XML order.
        actors.clear(); runtime.clear(); forces.clear(); ai.clear(); models.clear(); passenger.reset();
        for (const auto& item:saved.entities) {
            const bool player=item.name=="player" || item.name=="player_ucak";
            if (!player && !item.alive) continue;
            add_actor(item.name,item.model,item.position,player);
            auto& actor=actors.back(); actor.health=item.health; actor.dead=false; actor.hidden=!item.visible;
            ai.back().enabled=item.visible && !player; actor.aiEnabled=ai.back().enabled;
            if (clock.level==4 && item.name=="player") passenger=actors.size()-1;
        }
        if (actors.empty() || actors.front().role!=0) throw std::runtime_error("Checkpoint has no primary player");
        if (clock.level==4) {
            entity::turn(runtime[0],-90);
            if (passenger) entity::turn(runtime[*passenger],-90);
        }
        pickups.clear(); pickupModels.clear(); pickupRuntime.clear();
        for (const auto& item:saved.healths) add_pickup({item.big?LevelHealth::Kind::Big:LevelHealth::Kind::Small,{},item.position});
        const auto extra=checkpoint_state(original);
        if (extra.weapon) { combat::select_weapon(actors.front(),runtime.front(),*extra.weapon,clock.level,events); drain_events(); }
        for (const auto& value:extra.visibility) visibility(value.name,value.visible);
        for (const auto& value:extra.variables) set_variable(std::string(value.name),std::bit_cast<std::uint32_t>(value.value));
        if (extra.playerPosition) { set_position(actors.front(),*extra.playerPosition,true,clock.frameNow); entity::synchronize_position(runtime.front(),actors.front()); }
        if (!extra.restartMusic.empty()) { game.audio_.rewind(extra.restartMusic); game.audio_.play(extra.restartMusic); }
        lastCheckpoint=original; clock.phase=Phase::Active; clock.paused=false;
        game.menu_.set_game_started(true); game.menu_.show(0); update_camera(); drain_events();
    }
    void add_pickup(const LevelHealth& source) {
        auto gpu=instance(entity::pickup_model(source.kind),true);
        pickups.push_back(entity::initialize_pickup(source,gpu->resource().model));
        pickupRuntime.emplace_back(source.position,clock.frameNow); pickupModels.push_back(std::move(gpu));
    }
    void build_level(int level,bool initialize) {
        ++levelGeneration;
        const auto now=wallNow-clockOffset;
        clock.frameNow=now;
        const auto oldLevel=clock.level;
        if (oldLevel>=1 && oldLevel<=4) game.audio_.stop("level"+std::to_string(oldLevel));
        actors.clear(); runtime.clear(); forces.clear(); ai.clear(); models.clear();
        sources.clear(); decorations.clear(); pickups.clear(); pickupRuntime.clear(); pickupModels.clear();
        alternatePlayer.reset(); passenger.reset(); combat.projectiles.clear(); combat.meleeDelays.clear();
        clock.level=level; clock.previousLevel=level; clock.level_loaded(now);
        cameraState=initialize_player_camera(level); flight={}; passengerFlight={}; levelEnabled=true; overlay=false;
        metadata=read_level_metadata(game.scene_.resolve("data/levels/level"+std::to_string(level)+"/level.xml"));
        const auto start=level_player_start(level);
        add_actor(level==4?"player_ucak":"player",level==1?"yaman":level==4?"ucak":"yaman_silahli",start,true);
        if (level==1 || level==3) alternatePlayer=instance(descriptor(level==1?"yaman_silahli":"yaman_buyuk_silahli"),true);
        if (level==4) {
            add_actor("player","yaman_ucakta",start,true); passenger=1;
            entity::turn(runtime[0],-90); entity::turn(runtime[1],-90);
        }
        const auto directory="data/levels/level"+std::to_string(level)+"/";
        for (const auto& part:metadata.parts) sources.push_back({part.name,identity_matrix(),false,instance(directory+part.filename+"/model.dat",false)});
        for (const auto& source:metadata.models) sources.push_back({source.name,source.transform,false,instance(descriptor(source.filename),false)});
        if (initialize) for (const auto& bot:metadata.bots) add_actor(bot.name,bot.model,bot.position,false);
        if (level==4) {
            auto pose=identity_matrix(); entity::rotate_local_up(pose,-90);
            decorations.push_back({{},pose,false,instance(descriptor("cloud"),true)});
        }
        for (const auto& health:metadata.healths) {
            if (health.kind==LevelHealth::Kind::Small || health.kind==LevelHealth::Kind::Big) add_pickup(health);
            else { auto pose=identity_matrix(); pose.values[12]=health.position.x; pose.values[13]=health.position.y; pose.values[14]=health.position.z;
                   decorations.push_back({health.name,pose,false,instance(entity::pickup_model(health.kind),true)}); }
        }
        for (std::size_t i=1;i<projectileModels.size();++i)
            projectileModels[i]=instance(combat::weapons()[i].model,true);
        // 004181f0 calls 00401000: shared motion history resets for every level.
        history={}; install_hosts();
        vm.load_directory(game.scene_.resolve("data/scripts/level"+std::to_string(level)+".pcs").parent_path(),"level"+std::to_string(level));
        seed_callable_words();
        objectives.clear();
        static constexpr std::array<std::string_view,3> level1{{"1.Nöbetçi eczaneyi bul.","2.Tüm robotları öldür.","3.Romatizma ilacını anneanneye götür."}};
        static constexpr std::array<std::string_view,3> level2{{"1.Tarih kitabı için sahafı bul.","2.Sahafın babasını bul.","3.Kitabı bulmak için sahafa geri dön."}};
        static constexpr std::array<std::string_view,4> level3{{"1.Uçan makinayı yaptırmak için İsmail Usta’yı bul.","2.Peyami’yi bul.","3.Robotlardan gerekli malzemeleri topla (pervane , bisiklet, alet çantası).","4.İsmail Usta'nın atölyesine dön."}};
        if (level==1) for (auto text:level1) objectives.push_back({text,false});
        if (level==2) for (auto text:level2) objectives.push_back({text,false});
        if (level==3) for (auto text:level3) objectives.push_back({text,false});
        if (level==4) objectives.push_back({"1.Eti Yami’leri toplayarak enerjini koru ve tarih sınavına yetiş.",false});
        game.menu_.set_game_started(true); game.menu_.show(0); update_camera();
        if (initialize) invoke("on_start");
    }
    void movie_begin(std::string name,int percentage) {
        if (!movie.empty()) game.audio_.stop(movie);
        movie=std::move(name); movieStart=wallNow;
        game.scene_.reset_video(movie); // Material resolution remains strict for real AVI data.
        if (!game.audio_.registered(movie)) {
            std::filesystem::path wav;
            try { wav=game.scene_.resolve("data/avi_sound/"+movie+".wav"); }
            catch (const std::filesystem::filesystem_error& error) {
                if (error.code()!=std::errc::no_such_file_or_directory) throw;
                wav=error.path1(); // Retain the original unavailable sample, including its name.
            }
            game.audio_.register_sound({movie,wav,false,true,false,100,2});
        }
        game.audio_.play(movie,{},percentage);
        clock.phase=Phase::Intro; clock.paused=true;
    }
    void movie_end() {
        if (movie.empty()) return;
        auto finished=std::exchange(movie,{}); game.audio_.stop(finished); game.audio_.rewind(finished);
        const bool startup=actors.empty(); clock.phase=startup?Phase::Menu:Phase::Active;
        clock.paused=startup; clock.nextTick=wallNow-clockOffset;
        if (startup) { game.menu_.show(1); game.audio_.play("menumusic"); }
        invoke_event(finished,"_on_end");
        static constexpr std::array<std::string_view,7> stopped{{"level1_son","otobuse_binme","ismail_usta_robotu_yapmaya_baslar","son","kutup_oyun_oncesi","sahaf_kitap_alma","sahaf_ikinci_konusma"}};
        if (std::find(stopped.begin(),stopped.end(),finished)==stopped.end())
            for (int level=1;level<=4;++level) game.audio_.set_volume("level"+std::to_string(level),10,false);
    }
    void show_main(int page,bool first,bool second) {
        clock.paused=true;
        if (!second) { menuPauseWall=wallNow; pause_audio(true); }
        if (!first && !actors.empty()) {
            actors[0].hidden=true; if (passenger) actors[*passenger].hidden=true;
            overlay=true; pausePose=runtime[0].transform; return;
        }
        game.menu_.show(page); clock.previousLevel=second?-1:clock.level;
        clock.level=0; clock.phase=second?Phase::Intro:Phase::Menu;
    }
    void resume(bool first,bool second) {
        clock.paused=false;
        if (!first && !actors.empty()) {
            actors[0].hidden=false; if (passenger) actors[*passenger].hidden=false; overlay=false;
        } else {
            if (!second && clock.previousLevel>0) { clock.level=clock.previousLevel; clock.phase=Phase::Active; }
            clock.previousLevel=0;
        }
        if (!second) {
            // 00430060 / 004300e0 remove menu wall time from the original game timer.
            if (menuPauseWall) clockOffset+=wallNow-*menuPauseWall;
            clock.frameNow=wallNow-clockOffset;
            pause_audio(false);
        }
        menuPauseWall.reset();
        clock.nextTick=clock.frameNow; game.menu_.show(0);
    }
    void pause_audio(bool paused) { for (int channel=0;channel<48;++channel) if (game.audio_.playing(channel)) game.audio_.pause(channel,paused); }
    void ending() {
        score=std::max(score,1000); lastScore=score;
        const std::string token=std::to_string(lastScore);
        std::ofstream handoff(save_path("save43.eti"),std::ios::binary|std::ios::trunc);
        if (!handoff || !(handoff<<token) || !handoff.flush()) throw std::runtime_error("Cannot write native ending score handoff");
        handoff.close();
        if (!game.callbacks_.ending) throw std::runtime_error("Native ending dialog callback required");
        game.callbacks_.ending(game.callbacks_.host,token,clock.frameNow);
        score=0; combat.firstMeleeHit=true; actors[0].dead=true;
        show_main(5,true,false); clock.previousLevel=-1; clock.phase=Phase::Menu;
        game.scene_.reset_video("game_intro");
    }
    void install_hosts() {
        vm.reset_stack(); // 00415db0 starts with 0040c720 before registering hosts.
        for (const auto& specification:script::game_host_specs()) {
            const std::string name=specification.name;
            vm.register_host(name,[this,name](script::Runtime& machine) {
                if (name=="load_level") { const int level=machine.pop_int(); request_level(level,true); }
                else if (name=="play_avi") { auto subject=machine.pop_string(); auto gain=machine.pop_word(); movie_begin(std::move(subject),static_cast<int>(gain)); }
                else if (name=="play_music" || name=="play_fx") { auto subject=machine.pop_string(); auto gain=machine.pop_word(); game.audio_.play(subject,{},static_cast<int>(gain)); }
                else if (name=="stop_music") game.audio_.stop(machine.pop_string());
                else if (name=="set_volume") { auto subject=machine.pop_string(); auto gain=machine.pop_word(); auto effects=machine.pop_word(); game.audio_.set_volume(subject,gain,effects!=0); }
                else if (name=="set_player_weapon") { const auto weapon=machine.pop_int(); combat::select_weapon(actors.at(0),runtime.at(0),weapon,clock.level,events); drain_events(); }
                else if (name=="save_game") save(machine.pop_string());
                else if (name=="entity_hide" || name=="entity_show") visibility(machine.pop_string(),name=="entity_show");
                else if (name=="entity_set_model") { auto subject=machine.pop_string(); auto model=machine.pop_string(); for (std::size_t i=0;i<actors.size();++i) if (actors[i].name==subject) replace_model(i,model); }
                else if (name=="game_end") show_main(5,true,false);
                else if (name=="play_library_game") show_main(12,true,false);
                else if (name=="set_player_pos") { const float x=machine.pop_float(),y=machine.pop_float(),z=machine.pop_float(); set_position(actors.at(0),{-x,y,z},true,clock.frameNow); entity::synchronize_position(runtime.at(0),actors.at(0)); invoke("oyleomazsaboyleolur"); }
                else if (name=="son") ending();
                else if (name=="is_svar") { const auto subject=machine.pop_string(); machine.push_int(variable(subject)!=nullptr || machine.contains(subject)); }
                else if (name=="get_svar_int" || name=="get_svar_string") {
                    auto subject=machine.pop_string(); const auto* value=variable(subject);
                    const auto word=value?value->word:0;
                    if (name=="get_svar_string" && std::find(ownedStrings.begin(),ownedStrings.end(),word)!=ownedStrings.end())
                        machine.retain_string(word);
                    machine.push_word(word);
                }
                else if (name=="set_svar_int") { auto subject=machine.pop_string(); const auto value=machine.pop_word(); set_variable(std::move(subject),value); }
                else if (name=="set_svar_str") { auto subject=machine.pop_string(); auto text=machine.pop_string();
                    if (auto* old=variable(subject)) {
                        const auto address=old->word; const auto found=std::find(ownedStrings.begin(),ownedStrings.end(),address);
                        if (found!=ownedStrings.end()) { machine.release_string(address); ownedStrings.erase(found); }
                    }
                    const auto word=machine.allocate_string(text); ownedStrings.push_back(word); set_variable(std::move(subject),word);
                } else throw std::runtime_error("Unimplemented original host: "+name);
            });
        }
        seed_callable_words();
    }
    void drain_events() {
        std::size_t index=0;
        while (index<events.size()) {
            const auto event=events[index++];
            switch (event.kind) {
            case entity::Event::Kind::SoundPlay: game.audio_.play(event.subject,{},event.value); break;
            case entity::Event::Kind::MenuActivate:
                if (event.value==2 || event.value==4) game.menu_.show_notification(static_cast<std::size_t>(event.value));
                else { const int page=game.menu_.page(); game.menu_.show(0); game.menu_.set_button_state(static_cast<std::size_t>(event.value),true); game.menu_.show(page); }
                break;
            case entity::Event::Kind::MenuMessage: game.menu_.set_cursor({float(event.value),float(event.duration)}); break;
            case entity::Event::Kind::Pause: show_main(1,false,false); break;
            case entity::Event::Kind::Video: movie_begin(std::string(event.subject),event.value); break;
            case entity::Event::Kind::ReplacePlayerModel:
                if (alternatePlayer && descriptor(event.subject)==descriptor(actors[0].model)) break;
                if (alternatePlayer) { models[0].swap(alternatePlayer); actors[0].model=event.subject; }
                else replace_model(0,event.subject);
                break;
            }
        }
        events.clear();
    }
    static void death_callback(void* host,std::string_view name) {
        auto& self=*static_cast<State*>(host); self.drain_events(); self.invoke_event(name,"_on_dead");
    }
    static void trigger_callback(void* host,std::string_view name,std::string_view suffix) {
        static_cast<State*>(host)->invoke_event(name,suffix);
    }
    combat::Context context() {
        return {actors,runtime,metadata.blocking,0,clock.level,score,levelEnabled,random,
                clock.frameNow,clock.frameNow,seconds(),events,this,&death_callback};
    }
    template<class Function> void operation(Function&& function) {
        ++operationDepth; function(); drain_events(); --operationDepth; settle();
    }
    void tick() {
        const auto generation=levelGeneration;
        operation([&] { auto ctx=context(); combat::tick_melee(combat,ctx); });
        if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
        if (levelEnabled) history.sample_tick(forces[0].speed,forces[0].direction);
        for (std::size_t i=0;levelEnabled && i<actors.size();++i) {
            if (i!=0 && (!passenger || i!=*passenger)) operation([&] {
                auto& actor=actors[i]; ai[i].enabled=actor.aiEnabled;
                const enemy_ai::Input input{clock.frameNow,entity::forward_axis(runtime[i]),entity::right_axis(runtime[i]),history.latest(),forces[i].direction,forces[i].speed,0,{}};
                operations.clear(); enemy_ai::tick(ai[i],actor,actors[0],input,metadata.blocking,actors,i,0,random,operations);
                for (const auto& command:operations) {
                    auto ctx=context();
                    switch (command.type) {
                    case enemy_ai::Operation::Type::Move: entity::move(actor,forces[i],runtime[i],command.move); break;
                    case enemy_ai::Operation::Type::Turn: entity::turn(runtime[i],command.degrees); break;
                    case enemy_ai::Operation::Type::Action: entity::action(actor,actors[0],command.action,clock.frameNow,events); break;
                    case enemy_ai::Operation::Type::Attack: combat::attack_enemy(combat,ctx,i); break;
                    }
                    drain_events(); if (pendingLevel) break;
                }
            });
            if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
        }
        for (std::size_t i=0;i<actors.size();++i) {
            if (clock.level==4 && (i==0 || (passenger && i==*passenger))) operation([&] {
                auto effect=tick_flight(i,i==0?passenger:std::nullopt,i==0?flight:passengerFlight,wavePhase,clock.frameNow,metadata.blocking,actors,pickups,score);
                entity::pickup_effects(effect.pickups,healthMessageFrames,events);
                if (effect.showDeathMenu) { show_main(1,false,false); const auto page=game.menu_.page(); game.menu_.show(0); for (int b:{5,6,7}) game.menu_.set_button_state(b,true); game.menu_.show(page); game.menu_.set_cursor({512,100}); }
                if (effect.playEndingMovie) movie_begin("son",90);
            });
            else operation([&] {
                const auto effects=tick_body(i,forces[i],{0,-1,0},clock.frameNow,clock.level,metadata.blocking,actors,pickups,score);
                entity::synchronize_position(runtime[i],actors[i]); entity::pickup_effects(effects,healthMessageFrames,events);
            });
            if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
        }
        operation([&] { auto ctx=context(); combat::tick_projectiles(combat,ctx,clock.nextTick); });
    }
    void update_camera() {
        if (actors.empty()) return;
        camera=clock.level==4?flight_camera(runtime[0].transform,cameraState,flight)
              :solve_player_camera(runtime[0].transform,cameraState,clock.frameNow,metadata.blocking,actors,0);
        game.audio_.listener(actors[0].position);
    }
    void mouse(Vec2 motion) {
        mouseHistory[mouseSample%10]=motion;
        mouseIdle=(motion.x==0 && motion.y==0)?mouseIdle+1:0;
        smoothedMouse={};
        if (mouseIdle<10) {
            // 00411770 stores each weight as binary32, then sums oldest first
            // in x87 double precision; unsigned ring subtraction also wraps.
            std::array<float,9> weights; weights[0]=sensitivity;
            for (std::size_t age=1;age<weights.size();++age)
                weights[age]=static_cast<float>(double(weights[age-1])*double(0.2f));
            const auto oldest=mouseHistory[(mouseSample-9U)%10];
            double x=double(weights[8])*double(0.2f)*oldest.x;
            double y=double(weights[8])*double(0.2f)*oldest.y;
            for (int age=8;age>=0;--age) {
                const auto sample=mouseHistory[(mouseSample-static_cast<std::uint32_t>(age))%10];
                x+=double(weights[age])*sample.x; y+=double(weights[age])*sample.y;
            }
            smoothedMouse={static_cast<float>(x),static_cast<float>(y)};
        }
        ++mouseSample;
    }
    void input(const GameInput& raw) {
        const auto generation=levelGeneration;
        PlayerInput keys=raw.player; keys.fire=keys.fire || raw.mouse_left;
        keys.special=keys.special || (raw.mouse_right && clock.level!=4);
        keys.secondaryFire=keys.secondaryFire || (raw.mouse_right && clock.level==4);
        if (clock.level==4) {
            const FlightInput flightInput{keys,smoothedMouse.x,smoothedMouse.y,raw.mouse_left,raw.interact,raw.escape,previousEscape};
            const auto effect=apply_flight_input(actors[0],flight,flightInput,mouseActive); mouseActive=effect.mouseActive;
            if (effect.attack) operation([&] { auto ctx=context(); combat::attack_player(combat,ctx,0,camera,automaticAim); });
            if (effect.interact) operation([&] { entity::triggers(actors[0],metadata.triggers,true,this,&trigger_callback); });
            if (effect.openMenu) show_main(1,true,false);
        } else {
            operation([&] { auto ctx=context(); combat::apply_player_input(combat,ctx,forces[0],keys,camera,automaticAim); });
            if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
            entity::look(runtime[0],-smoothedMouse.x,automaticAim?0:-smoothedMouse.y);
            if (raw.interact) operation([&] { entity::triggers(actors[0],metadata.triggers,true,this,&trigger_callback); });
            if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
            objectivesVisible=raw.help;
            game.menu_.set_button_state(3,raw.help);
            if (raw.escape && !previousEscape) show_main(1,true,false);
        }
    }
    void update_render() {
        const auto generation=levelGeneration;
        for (std::size_t i=0;i<actors.size();++i) {
            operation([&] {
            entity::update_render(actors[i],runtime[i],clock.frameNow);
            const auto& resource=models[i]->resource();
            if (!resource.parts.empty() && !resource.parts[0].mesh->animations.empty()) {
                const auto ordinal=static_cast<std::size_t>(actors[i].action);
                if (ordinal>=resource.parts[0].mesh->animations.size()) throw std::runtime_error("Entity action exceeds original animation ordinals");
                entity::update_animation(actors[i],actors[0],resource.parts[0].mesh->animations[ordinal].frame_count,clock.level,clock.frameNow,random,events);
            }
            combat::regenerate(actors[i],runtime[i],clock.frameNow);
            });
            if (clock.phase!=Phase::Active || levelGeneration!=generation) return;
        }
        for (auto& pickup:pickupRuntime) entity::update_pickup(pickup.animation,clock.frameNow);
        combat::interpolate_projectiles(combat,clock.frameNow); update_camera();
    }
    void persist_settings() {
        menu::save_settings(game.saveDirectory_/"game.ini",game.menu_.settings());
    }
    void menu_commands() {
        // Commands own only views into persistent menu resources. Copy the short
        // command list before mutations can append settings commands.
        commands.assign(game.menu_.commands().begin(),game.menu_.commands().end()); game.menu_.clear_commands();
        for (const auto& command:commands) {
            switch (command.kind) {
            case menu::CommandKind::InitializeLevel:
                if (command.first) request_level(command.level,command.second);
                else if (!actors.empty()) { set_position(actors[0],level_player_start(clock.level),true,clock.frameNow); entity::synchronize_position(runtime[0],actors[0]); }
                break;
            case menu::CommandKind::ResetLevel: clear_registry(); actors.clear(); runtime.clear(); forces.clear(); ai.clear(); models.clear(); pickups.clear(); pickupModels.clear(); pickupRuntime.clear(); sources.clear(); decorations.clear(); combat.projectiles.clear(); combat.meleeDelays.clear(); break;
            case menu::CommandKind::ResumeGame: resume(command.first,command.second); break;
            case menu::CommandKind::ShowMainMenu: show_main(command.level,command.first,command.second); break;
            case menu::CommandKind::LoadCheckpoint: checkpoint(command.name); break;
            case menu::CommandKind::RestartCheckpoint: {
                if (lastCheckpoint.empty()) {
                    // 00428940 searches newest checkpoint across all four levels.
                    static constexpr std::array<std::string_view,8> newest{{
                        "data/save/save43.eti","data/save/save41.eti","data/save/save32.eti",
                        "data/save/save31.eti","data/save/save22.eti","data/save/save21.eti",
                        "data/save/save12.eti","data/save/save11.eti"}};
                    for (const auto name:newest) if (std::ifstream(save_path(name),std::ios::binary)) {
                        lastCheckpoint=name; break;
                    }
                    if (lastCheckpoint.empty()) throw std::runtime_error("No native checkpoint available to restart");
                }
                checkpoint(lastCheckpoint);
                break;
            }
            case menu::CommandKind::RestoreCamera:
                if (!runtime.empty() && clock.level!=4) { runtime[0].transform=pausePose; runtime[0].playerPitch=0; update_camera(); }
                break;
            case menu::CommandKind::StopMusic: game.audio_.stop(command.name); break;
            case menu::CommandKind::PlayMusic: game.audio_.play(command.name); break;
            case menu::CommandKind::SetMusicVolume: game.audio_.set_volume(command.name,100,false); break;
            case menu::CommandKind::ResetVideo: game.scene_.reset_video(command.name); break;
            case menu::CommandKind::ApplySettings:
                sensitivity=command.settings.mouseSensitivity;
                game.audio_.master_volume(command.settings.musicGain,command.settings.effectGain);
                game.renderer_.set_brightness(command.settings.brightnessRamp);
                game.renderer_.set_graphics(command.settings.values.graphics);
                break;
            case menu::CommandKind::SetAimingMode:
                automaticAim=command.first;
                if (automaticAim && !runtime.empty()) { entity::rotate_world_x(runtime[0].transform,-runtime[0].playerPitch); runtime[0].playerPitch=0; }
                break;
            case menu::CommandKind::RunScript: invoke(command.name); break;
            case menu::CommandKind::PersistSettings: persist_settings(); break;
            case menu::CommandKind::Quit: quit=true; break;
            }
        }
    }
    void draw(std::uint32_t now) {
        wallNow=now;
        if (clock.phase==Phase::Intro) {
            game.renderer_.camera(identity_matrix(),interface_projection(),true);
            if (!movie.empty() && !game.scene_.draw_video(movie,now-movieStart)) movie_end();
            return;
        }
        now-=clockOffset;
        if (clock.phase==Phase::Active && !actors.empty()) {
            entity::respawn_big_health(actors[0],pickups,score);
            if (clock.level==4) entity::update_flight_pickups(pickupMotion,pickupRuntime,pickups);
            game.renderer_.camera(inverse_pose(camera),frustum_projection(0.75f,float(game.renderer_.pixel_width())/game.renderer_.pixel_height(),1,30000));
            for (std::size_t i=0;i<sources.size();++i) if (!sources[i].hidden) {
                DrawState draw; draw.model=sources[i].transform; draw.lighting=i!=0; draw.fog=i!=0 && clock.level!=4;
                game.scene_.draw(*sources[i].instance,draw,0,0,now);
            }
            for (std::size_t i=0;i<actors.size();++i) if (!actors[i].hidden) {
                DrawState draw; draw.model=entity::upright_transform(runtime[i],models[i]->resource().model.collision_box.y);
                draw.lighting=true; draw.fog=clock.level!=4; draw.diffuse_light=entity::draw_light(actors[i],runtime[i]);
                game.scene_.draw(*models[i],draw,static_cast<std::size_t>(actors[i].action),actors[i].animationFrame,now);
            }
            for (std::size_t i=0;i<pickups.size();++i) if (!pickups[i].hidden && !pickups[i].removed) {
                DrawState draw; draw.fog=clock.level!=4; entity::Runtime pose;
                const auto p=pickups[i].bounds.center; pose.transform.values[12]=p.x; pose.transform.values[13]=p.y; pose.transform.values[14]=p.z;
                draw.model=entity::upright_transform(pose,pickupModels[i]->resource().model.collision_box.y);
                game.scene_.draw(*pickupModels[i],draw,0,pickupRuntime[i].animation.elapsed,now);
            }
            for (auto& source:decorations) if (!source.hidden) {
                if (clock.level==4 && &source==&decorations.front()) {
                    const auto right=entity::right_axis(runtime[0]);
                    const Vec3 front{-right.z,0,right.x};
                    source.transform.values[12]=actors[0].position.x+front.x*200;
                    source.transform.values[13]=actors[0].position.y+180;
                    source.transform.values[14]=actors[0].position.z+front.z*200;
                }
                DrawState draw; entity::Runtime pose; pose.transform=source.transform; draw.fog=clock.level!=4;
                draw.model=entity::upright_transform(pose,source.instance->resource().model.collision_box.y);
                game.scene_.draw(*source.instance,draw,0,entity::decoration_animation(now),now);
            }
            for (const auto& projectile:combat.projectiles) {
                const auto weapon=static_cast<std::size_t>(actors.at(projectile.owner).weapon);
                if (projectileModels[weapon]) { DrawState draw; draw.model=combat::projectile_transform(projectile); draw.fog=clock.level!=4;
                    game.scene_.draw(*projectileModels[weapon],draw,0,now,now); }
            }
            game.renderer_.finish_world();
            const bool wideHud=game.menu_.page()==0;
            const float logicalWidth=wideHud
                ? std::max(1024.0f,768.0f*game.renderer_.pixel_width()/game.renderer_.pixel_height())
                : 1024.0f;
            if (wideHud) game.renderer_.hud_camera();
            else game.renderer_.camera(identity_matrix(),interface_projection(),true);
            if (overlay) {
                if (clock.level!=4) {
                    const float next=runtime[0].playerPitch+pausePitchStep;
                    if (next>40 || next<-70) pausePitchStep=-pausePitchStep;
                    runtime[0].playerPitch+=pausePitchStep;
                    entity::rotate_world_x(runtime[0].transform,pausePitchStep*0.125f);
                    entity::rotate_world_y(runtime[0].transform,0.5f); entity::rotate_world_z(runtime[0].transform,pausePitchStep);
                }
            }
            const menu::Hud hud{score,actors[0].bigHealthTicks,objectivesVisible,objectives};
            game.scene_.draw_ui(game.menu_.draw(game.menu_context(),hud,logicalWidth),now);
            if (!automaticAim && actors[0].weapon!=0) {
                const auto aim=ui_quad({(logicalWidth/2-30)/1024,354.0f/768,60.0f/1024,60.0f/768},"nisan");
                game.scene_.draw_ui(std::span<const menu::Draw>(&aim,1),now);
            }
        } else {
            game.renderer_.camera(identity_matrix(),interface_projection(),true);
            game.scene_.draw_ui(game.menu_.draw(game.menu_context()),now);
        }
    }
};
Game::Game(Renderer& renderer,SceneCache& scene,menu::Menu& menu,AudioSystem& audio,
           std::filesystem::path saveDirectory,GameCallbacks callbacks)
    :renderer_(renderer),scene_(scene),menu_(menu),audio_(audio),saveDirectory_(std::move(saveDirectory)),callbacks_(callbacks),state_(std::make_unique<State>(*this)) {}
Game::~Game()=default;
void Game::initialize(bool skipIntro) {
    std::filesystem::create_directories(saveDirectory_); audio_.register_original_sounds(scene_.root());
    state_->wallNow=state_->clock.frameNow=static_cast<std::uint32_t>(SDL_GetTicks()); menu_.show(1);
    state_->saveCounts=menu::checkpoint_counts(saveDirectory_);
    state_->install_hosts(); state_->menu_commands();
    if (skipIntro) { state_->clock.phase=Phase::Menu; state_->clock.paused=true; audio_.play("menumusic"); }
    else state_->movie_begin("game_intro",90);
}
void Game::persist_settings() { state_->persist_settings(); }
void Game::load_level(std::int32_t level,bool initializeFromXml) { state_->request_level(level,initializeFromXml); }
void Game::frame(const GameInput& input,std::uint32_t now) {
    auto& s=*state_; s.wallNow=now; s.clock.begin_frame(now-s.clockOffset);
    if (!s.focused) { s.previousEscape=false; return; }
    s.mouse(input.look_motion);
    if (s.clock.phase==Phase::Intro) { if (input.escape && !s.previousEscape) s.movie_end(); }
    else if (s.clock.phase==Phase::Menu || s.overlay || (!s.actors.empty() && s.actors[0].dead)) {
        s.saveCounts=menu::checkpoint_counts(saveDirectory_);
        menu_.update({input.menu_motion,input.mouse_left},menu_context()); s.menu_commands();
    }
    if (s.clock.phase==Phase::Active && !s.clock.paused && !s.actors.empty()) {
        while (s.clock.tick_due()) {
            const auto generation=s.levelGeneration;
            s.tick();
            if (s.levelGeneration!=generation) { s.previousEscape=input.escape; return; }
            s.clock.finish_tick();
            if (s.clock.phase!=Phase::Active || s.clock.paused) break;
        }
        if (s.clock.phase==Phase::Active && !s.clock.paused) {
            const auto generation=s.levelGeneration;
            s.input(input);
            if (s.clock.phase==Phase::Active && s.levelGeneration==generation) s.update_render();
        }
    }
    s.previousEscape=input.escape;
}
void Game::render(std::uint32_t now) { state_->draw(now); }
void Game::skip_video() { state_->movie_end(); }
void Game::set_focused(bool focused) { state_->focused=focused; if (!focused) { state_->mouseHistory={}; state_->smoothedMouse={}; state_->previousEscape=false; } }
bool Game::quit_requested() const noexcept { return state_->quit; }
const gameplay::FrameClock& Game::clock() const noexcept { return state_->clock; }
std::span<const gameplay::EntityState> Game::entities() const noexcept { return state_->actors; }
Vec3 Game::player_position() const noexcept { return state_->actors.empty()?Vec3{}:state_->actors[0].committedPosition; }
menu::Context Game::menu_context() const {
    const auto& s=*state_; const auto* player=s.actors.empty()?nullptr:&s.actors[0];
    return {s.clock.level,s.clock.previousLevel,player!=nullptr,player && player->dead,
            player?player->health:1,player?player->energy:0,s.clock.phase==Phase::Loading?0.0f:1.0f,
            s.saveCounts};
}
const std::filesystem::path& Game::save_directory() const noexcept { return saveDirectory_; }
} // namespace yami
