#include "gameplay.hpp"
#include "entity.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <stdexcept>
#include <cstring>
#include <memory>
#include <sstream>
#include <fstream>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace yami::gameplay {
namespace {
float component(Vec3 v, std::size_t i) noexcept { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
Vec3 subtract(Vec3 a, Vec3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
float dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 along(Vec3 p, Vec3 d, float t) noexcept { return {p.x+d.x*t,p.y+d.y*t,p.z+d.z*t}; }
std::size_t next_health(const std::vector<HealthState>& healths,std::size_t index) noexcept {
    const auto next=healths[index].scanNext;
    return next==static_cast<std::size_t>(-1) ? index+1 : next;
}
std::size_t first_health(const std::vector<HealthState>& healths) noexcept {
    return healths.empty() ? 0 : healths[0].scanHead;
}
Obb entity_box(const EntityState& e) {
    Obb result;
    result.center = e.position;
    result.axes = e.bounds.axes;
    result.extents = e.bounds.extents;
    return result;
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> void read_value(std::istream& input, T& value) {
    require(static_cast<bool>(input >> value), "Truncated or invalid original save field");
}
void read_position(std::istream& input, Vec3& value) {
    read_value(input,value.x); read_value(input,value.y); read_value(input,value.z);
    require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z),
            "Non-finite original save position");
}
std::size_t read_count(std::istream& input, std::size_t maximum) {
    std::int32_t count = 0; read_value(input,count);
    require(count >= 0 && static_cast<std::uint32_t>(count) <= maximum,
            "Original save count exceeds recovered layout");
    return static_cast<std::size_t>(count);
}
void require_token(const std::string& value) {
    require(!value.empty() && value.find_first_of(" \t\r\n\v\f") == std::string::npos,
            "Original save names must be nonempty whitespace-free tokens");
}
} // namespace

bool intersects(const Obb& a, const Aabb& b) noexcept {
    // 00409330 uses world XYZ, the three OBB axes, then each XYZ x OBB axis.
    // No normalization or epsilon is applied to the exported, rounded XML axes.
    const Vec3 delta = subtract(a.center,b.center);
    float absolute[3][3];
    for (std::size_t j=0;j<3;++j)
        for (std::size_t i=0;i<3;++i) absolute[j][i]=std::fabs(component(a.axes[j],i));
    for (std::size_t i=0;i<3;++i) {
        const float radius=absolute[0][i]*a.extents.x + absolute[1][i]*a.extents.y
                         + absolute[2][i]*a.extents.z + component(b.extents,i);
        if (!(radius >= std::fabs(component(delta,i)))) return false;
    }
    for (std::size_t j=0;j<3;++j) {
        const float radius=absolute[j][0]*b.extents.x + absolute[j][1]*b.extents.y
                         + absolute[j][2]*b.extents.z + component(a.extents,j);
        if (!(radius >= std::fabs(dot(a.axes[j],delta)))) return false;
    }
    for (std::size_t i=0;i<3;++i) {
        const std::size_t p=(i+1)%3,q=(i+2)%3;
        for (std::size_t j=0;j<3;++j) {
            const std::size_t k=(j+1)%3,l=(j+2)%3;
            const float distance=std::fabs(component(a.axes[j],p)*component(delta,q)
                                         -component(a.axes[j],q)*component(delta,p));
            const float radius=absolute[l][i]*component(a.extents,k)
                             +absolute[k][i]*component(a.extents,l)
                             +absolute[j][p]*component(b.extents,q)
                             +absolute[j][q]*component(b.extents,p);
            if (!(radius >= distance)) return false;
        }
    }
    return true;
}

bool intersect_ray(Vec3 origin, Vec3 direction, const Obb& box, float& distance) noexcept {
    // 0040aa59: epsilon bits 0x3a83126f = 0.001f; equality takes reciprocal path.
    const Vec3 delta=subtract(origin,box.center);
    distance=0;
    float far=std::numeric_limits<float>::max();
    for (std::size_t i=0;i<3;++i) {
        const float projected=dot(delta,box.axes[i]), d=dot(direction,box.axes[i]);
        const float extent=component(box.extents,i);
        if (std::fabs(d) >= 0.001f) {
            const float reciprocal=1.0f/d;
            float near=(-extent-projected)*reciprocal, exit=(extent-projected)*reciprocal;
            if (exit<near) std::swap(near,exit);
            if (near>distance) distance=near;
            if (exit<far) far=exit;
            if (distance>far) return false;
        } else if (projected < -extent || projected > extent) return false;
    }
    return true;
}

CollisionHit raycast(Vec3 origin, Vec3 direction, const std::vector<Obb>& boxes,
                     const std::vector<EntityState>& entities,
                     std::optional<std::size_t> excluded, bool livingOnly) noexcept {
    CollisionHit hit;
    float nearest=1e19f; // 0040b3de; not FLT_MAX
    for (std::size_t i=0;i<boxes.size();++i) {
        float distance;
        if (!boxes[i].disabled && intersect_ray(origin,direction,boxes[i],distance)
            && distance<nearest) {
            nearest=distance; hit.kind=CollisionHit::Kind::Static; hit.index=i;
        }
    }
    for (std::size_t i=0;i<entities.size();++i) {
        const auto& entity=entities[i];
        if (excluded==i || entity.hidden || (livingOnly && entity.dead)) continue;
        float distance;
        if (intersect_ray(origin,direction,entity_box(entity),distance) && distance<nearest) {
            nearest=distance; hit.kind=CollisionHit::Kind::Entity; hit.index=i;
        }
    }
    if (hit.kind!=CollisionHit::Kind::None) {
        hit.distance=nearest; hit.point=along(origin,direction,nearest);
    }
    return hit;
}

CollisionHit overlap(const Aabb& probe, const std::vector<Obb>& boxes,
                     const std::vector<EntityState>& entities,
                     std::optional<std::size_t> excluded) noexcept {
    // 0040ad80 deliberately does NOT filter dead or hidden entities.
    for (std::size_t i=0;i<boxes.size();++i)
        if (!boxes[i].disabled && intersects(boxes[i],probe))
            return {CollisionHit::Kind::Static,i,0,{}};
    for (std::size_t i=0;i<entities.size();++i)
        if (excluded!=i && intersects(entity_box(entities[i]),probe))
            return {CollisionHit::Kind::Entity,i,0,{}};
    return {};
}

void set_position(EntityState& e, Vec3 p, bool commit, std::uint32_t frameNow) noexcept {
    if (e.position.x==p.x && e.position.y==p.y && e.position.z==p.z) return;
    e.position=p;
    if (commit) {
        e.previousPosition=p; e.committedPosition=p; e.positionStart=frameNow;
    }
}
Vec3 interpolate(Vec3 previous, Vec3 current, std::uint32_t start, std::uint32_t now) noexcept {
    const auto elapsed=std::min<std::uint32_t>(now-start,33);
    const float before=static_cast<float>(33-elapsed),after=static_cast<float>(elapsed);
    constexpr float reciprocal=1.0f/33.0f;
    return {(before*previous.x+after*current.x)*reciprocal,
            (before*previous.y+after*current.y)*reciprocal,
            (before*previous.z+after*current.z)*reciprocal};
}

bool action_locked(const EntityState& e) noexcept {
    const auto a=e.action;
    if (e.role==1) return a==6;
    if (e.role!=0) return false;
    if (a==10 || a==22 || a==13 || a==12 || a==15) return true;
    return e.model=="yaman" && a>=16 && a<=21;
}
ActionTransition transition_action(EntityState& e, EntityState& player,
                                   std::int32_t next, std::uint32_t now) noexcept {
    if (e.role==0 && e.action==13 && player.actionCompleted!=0) {
        player.dead=true;
        return ActionTransition::PlayerDeathMenu;
    }
    if (e.role==0 && next==0 && (e.action==1 || e.action==2 || e.action==3)
        && e.actionCompleted==0) return ActionTransition::Unchanged;
    const bool forced=(e.role==0 && (next==13 || next==14))
                     || (e.role==1 && (next==6 || next==7));
    if (!forced && (next==e.action || (action_locked(e) && e.actionCompleted==0)))
        return ActionTransition::Unchanged;
    if (e.action==22) player.specialAttack=false;
    e.action=next; e.animationFrame=0; e.actionStart=now; e.actionCompleted=0;
    return ActionTransition::Changed;
}
bool movement_allowed(const EntityState& e) noexcept {
    if (e.role!=0 || e.actionCompleted!=0) return true;
    if (e.action==12) return false;
    return e.weapon!=0 || (e.action!=15 && e.action!=18 && e.action!=19 && e.action!=20);
}
InputDecision select_player_input(const PlayerInput& i, const EntityState& e) noexcept {
    InputDecision out;
    const bool attack=(i.fire || i.secondaryFire) && !e.specialAttack
                      && (e.action!=12 || e.actionCompleted!=0);
    out.attack=attack;
    auto choose=[&](Move a,Move b,std::int32_t normal,std::int32_t firing) {
        out.moves={a,b}; out.moveCount=b==Move::Stop ? 1 : 2;
        out.action=attack ? firing : normal;
    };
    if ((i.forward && i.right) || (i.alternateForward && i.alternateRight))
        choose(Move::Forward,Move::Right,5,17);
    else if ((i.forward && i.left) || (i.alternateForward && i.alternateLeft))
        choose(Move::Forward,Move::Left,9,21);
    else if ((i.back && i.right) || (i.alternateBack && i.alternateRight))
        choose(Move::Back,Move::Right,7,19);
    else if ((i.back && i.left) || (i.alternateBack && i.alternateLeft))
        choose(Move::Back,Move::Left,7,19);
    else if ((i.forward && !i.back) || (i.alternateForward && !i.alternateBack))
        choose(Move::Forward,Move::Stop,4,16);
    else if ((i.back && !i.forward) || (i.alternateBack && !i.alternateForward))
        choose(Move::Back,Move::Stop,7,19);
    else if ((i.right && !i.left) || (i.alternateRight && !i.alternateLeft))
        choose(Move::Right,Move::Stop,6,18);
    else if ((i.left && !i.right) || (i.alternateLeft && !i.alternateRight))
        choose(Move::Left,Move::Stop,8,20);
    else choose(Move::Stop,Move::Stop,0,15);
    if (i.special) out.specialAction=e.weapon==0 ? 10 : 22;
    return out;
}

void FrameClock::level_loaded(std::uint32_t now) noexcept {
    phase=Phase::Active; levelStart=now; nextTick=now;
}
void FrameClock::begin_frame(std::uint32_t now) noexcept { if (!paused) frameNow=now; }
bool FrameClock::tick_due() const noexcept { return !paused && nextTick<frameNow; }
void FrameClock::finish_tick() noexcept { nextTick+=1000/30; }
void FrameClock::open_menu() noexcept {
    paused=true; phase=level==-1 ? Phase::Intro : Phase::Menu;
    previousLevel=level; level=0;
}

SaveGame read_save(std::istream& input,std::size_t variableCount) {
    SaveGame save;
    unsigned char score[4]{};
    input.read(reinterpret_cast<char*>(score),4);
    require(input.gcount()==4,"Truncated original binary score");
    const std::uint32_t bits=std::uint32_t(score[0]) | std::uint32_t(score[1])<<8
                           | std::uint32_t(score[2])<<16 | std::uint32_t(score[3])<<24;
    save.score=bits<=0x7fffffffU ? static_cast<std::int32_t>(bits)
                               : -1-static_cast<std::int32_t>(0xffffffffU-bits);
    read_value(input,save.level);
    require(save.level>=1 && save.level<=4,"Original save level outside 1..4");
    const auto entities=read_count(input,64);
    save.entities.resize(entities);
    for (auto& entity:save.entities) {
        read_value(input,entity.name); read_value(input,entity.model);
        require(entity.name.size()<32 && entity.model.size()<32,
                "Original save name exceeds recovered 32-byte buffers");
        read_position(input,entity.position); read_value(input,entity.health);
        require(std::isfinite(entity.health),"Non-finite original save health");
        std::int32_t alive,visible; read_value(input,alive); read_value(input,visible);
        entity.alive=alive!=0; entity.visible=visible==1;
    }
    const auto healths=read_count(input,65536);
    save.healths.resize(healths);
    for (auto& health:save.healths) {
        read_position(input,health.position);
        std::int32_t big; read_value(input,big); health.big=big!=0;
    }
    require(variableCount<=65536,"Script registry too large for original save");
    save.scriptValues.resize(variableCount);
    for (auto& value:save.scriptValues) read_value(input,value);
    return save;
}
void write_save(std::ostream& output,const SaveGame& save) {
    require(save.level>=1 && save.level<=4,"Original save level outside 1..4");
    require(save.entities.size()<=64,"Original reader supports at most 64 entities");
    const auto word=static_cast<std::uint32_t>(save.score);
    const char bytes[4]={static_cast<char>(word),static_cast<char>(word>>8),
                         static_cast<char>(word>>16),static_cast<char>(word>>24)};
    output.write(bytes,4);
    output << ' ' << save.level << ' ' << save.entities.size() << '\n';
    output << std::fixed << std::setprecision(2);
    for (const auto& entity:save.entities) {
        require_token(entity.name); require_token(entity.model);
        require(entity.name.size()<32 && entity.model.size()<32,
                "Original save name exceeds recovered 32-byte buffers");
        output << ' ' << entity.name << ' ' << entity.model << ' '
               << std::setw(5) << entity.position.x << ' ' << std::setw(5) << entity.position.y
               << ' ' << std::setw(5) << entity.position.z << "  "
               << std::setw(5) << entity.health << "  " << (entity.alive ? 1 : 0)
               << " " << (entity.visible ? 1 : 0) << '\n';
    }
    output << ' ' << save.healths.size();
    for (const auto& health:save.healths)
        output << ' ' << std::setw(5) << health.position.x << ' ' << std::setw(5) << health.position.y
               << ' ' << std::setw(5) << health.position.z << ' ' << (health.big ? 1 : 0);
    output << '\n';
    for (auto value:save.scriptValues) output << ' ' << value << '\n';
    output << save.score; // 004278f0: duplicate text score, ignored by original reader
    require(static_cast<bool>(output),"Writing original save failed");
}

CheckpointState checkpoint_state(std::string_view path) {
    CheckpointState out;
    if (path=="data/save/save11.eti" || path=="data/save/save12.eti") {
        const bool second=path=="data/save/save12.eti";
        out.weapon=0; out.restartMusic="level1";
        out.visibility={{"ok_eczane",true},{"ok_gecit_kapisi",!second},
                        {"ok_sifa_eczane",true},{"ok_annane",false},
                        {"kapali_kapi",!second},{"x_kapi_collision",!second}};
        out.variables={{"gecit_kapisi_ilk_defa_kullanildi",second},
                       {"robot_1_oldu",second},{"robot_2_oldu",second},{"robot_3_oldu",second},
                       {"robot_4_oldu",second},{"robot_5_oldu",second},{"robot_6_oldu",second},
                       {"robot_7_oldu",second},{"herhangi_bir_robot_oldu_mu",second},
                       {"yaman_ilaci_aldi",0},{"butun_robotlar_oldu",second},
                       {"gecit_kapisi_kullanilamaz",second}};
    } else if (path=="data/save/save21.eti" || path=="data/save/save22.eti") {
        const bool second=path=="data/save/save22.eti";
        out.weapon=1; out.restartMusic="level2";
        out.visibility={{"ok_sahaf",false},{"ok_sahaf_baba",second},{"ok_otobus",false}};
        out.variables={{"yaman_sahafla_konustu",second},{"yaman_sahafin_babasiyla_konustu",0},
                       {"bot_30_oldu",second},{"bot_29_oldu",second},
                       {"bot_28_oldu",second},{"bot_27_oldu",second}};
    } else if (path=="data/save/save31.eti" || path=="data/save/save32.eti") {
        const bool second=path=="data/save/save32.eti";
        out.weapon=second ? 2 : 1; out.restartMusic="level3";
        out.visibility={{"bisikletli_robot",second},{"alet_kutulu_robot",second},
                        {"alet_kutulu_robot_piyon_5",second},{"alet_kutulu_robot_piyon_6",second},
                        {"alet_kutulu_robot_piyon_7",second},{"alet_kutulu_robot_piyon_8",second},
                        {"alet_kutulu_robot_piyon_9",second},{"bisikletli_robot_piyon_2",second},
                        {"pervaneli_robot_piyon_son",second},{"bot_robot_37",second},
                        {"bot_robot_38",second},{"bot_robot_39",second},{"bot_robot_40",second},
                        {"bot_robot_41",second},{"bot_robot_42",second},{"ok_ismail_usta",!second},
                        {"ok_peyami",false},{"peyami",!second}};
        if (!second) out.visibility.push_back({"pervaneli_robot",false});
        out.variables={{"yaman_ismail_usta_ile_konustu",second},{"ilk_robotlar_oldumu",1},
                       {"pervaneli_robot_oldu",0},{"alet_kutulu_robot_oldu",0},
                       {"bisikletli_robot_oldu",0},{"kalles_robot_1_oldu",second},
                       {"kalles_robot_2_oldu",second},{"kalles_robot_3_oldu",second},
                       {"kalles_robot_4_oldu",second},{"kalles_robot_15_oldu",second},
                       {"ilk_2_robot_oldu_1",1},{"ilk_2_robot_oldu_2",1},
                       {"butun_kalles_robotlar_oldu",second}};
        if (second) out.playerPosition=Vec3{-1987,117,320};
    } else if (path=="data/save/save41.eti") out.restartMusic="level4";
    return out;
}

DamageResult damage(EntityState& entity,EntityState& player,float amount,
                    std::int32_t& score,bool& levelEnabled,std::uint32_t now) noexcept {
    DamageResult result;
    if (entity.dead || entity.dying) return result;
    result.applied=true;
    if (entity.role==0) result.action=transition_action(entity,player,12,now);
    entity.health-=amount;
    if (entity.role==0 && score>2) score-=2; // original strict >0 after subtraction
    if (entity.health<=0) {
        result.died=true; entity.dying=true; entity.health=0;
        if (entity.role==0) {
            score=0; levelEnabled=false;
            if (entity.action!=13) result.action=transition_action(entity,player,13,now);
        } else {
            entity.dead=true; result.action=transition_action(entity,player,6,now);
            result.invokeOnDead=true; entity.aiEnabled=false; score+=20;
        }
    }
    return result;
}

namespace {
std::uint32_t float_bits(float value) noexcept {
    std::uint32_t bits; std::memcpy(&bits,&value,sizeof bits); return bits;
}
float from_bits(std::uint32_t bits) noexcept {
    float value; std::memcpy(&value,&bits,sizeof value); return value;
}
const std::array<std::uint32_t,65536>& sqrt_table() noexcept {
    static const auto table=[] {
        std::array<std::uint32_t,65536> result{};
        for (std::uint32_t i=0;i<32768;++i) {
            result[i]=float_bits(std::sqrt(from_bits((i|0x400000U)<<8)))&0x7fffffU;
            result[i+32768]=float_bits(std::sqrt(from_bits((i|0x3f8000U)<<8)))&0x7fffffU;
        }
        return result;
    }();
    return table;
}
float quantized_sqrt(float squared) noexcept {
    const auto bits=float_bits(squared);
    if (bits==0) return 0;
    return from_bits(((((bits+0xc0800000U)>>1)+0x3f800000U)&0x7f800000U)
                    |sqrt_table()[(bits>>8)&0xffffU]);
}
Vec3 cross(Vec3 a,Vec3 b) noexcept {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
} // namespace
float recovered_length(Vec3 value) noexcept {
    return quantized_sqrt(static_cast<float>(static_cast<long double>(value.z)*value.z
                          +static_cast<long double>(value.y)*value.y
                          +static_cast<long double>(value.x)*value.x));
}
float normalize(Vec3& value) noexcept {
    const float length=recovered_length(value);
    if (length!=0) {
        const long double reciprocal=1.0L/length;
        value={static_cast<float>(value.x*reciprocal),static_cast<float>(value.y*reciprocal),
               static_cast<float>(value.z*reciprocal)};
    }
    return length;
}

SweepHit sweep_box(const Obb& box,const Aabb& probe,Vec3 displacement) noexcept {
    std::array<Vec3,15> axes;
    axes[0]={1,0,0}; axes[1]={0,1,0}; axes[2]={0,0,1};
    for (std::size_t j=0;j<3;++j) axes[3+j]=box.axes[j];
    for (std::size_t i=0;i<3;++i)
        for (std::size_t j=0;j<3;++j) axes[6+i*3+j]=cross(axes[i],box.axes[j]);
    const Vec3 delta=subtract(box.center,probe.center);
    float best=-1.01f; // bits 0xbf8147ae at 004597fc, not zero
    std::size_t selected=0;
    for (std::size_t i=0;i<15;++i) {
        const auto axis=axes[i];
        const float projected=dot(delta,axis),velocity=dot(displacement,axis);
        // OBB principal-axis tests assume orthonormal axes despite rounded XML.
        float radius;
        if (i<3) {
            radius=std::fabs(component(box.axes[0],i))*box.extents.x
                  +std::fabs(component(box.axes[1],i))*box.extents.y
                  +std::fabs(component(box.axes[2],i))*box.extents.z
                  +component(probe.extents,i);
        } else if (i<6) {
            radius=std::fabs(axis.x)*probe.extents.x+std::fabs(axis.y)*probe.extents.y
                  +std::fabs(axis.z)*probe.extents.z+component(box.extents,i-3);
        } else {
            const auto world=(i-6)/3,j=(i-6)%3,k=(j+1)%3,l=(j+2)%3;
            const auto p=(world+1)%3,q=(world+2)%3;
            radius=std::fabs(component(box.axes[l],world))*component(box.extents,k)
                  +std::fabs(component(box.axes[k],world))*component(box.extents,l)
                  +std::fabs(component(box.axes[j],p))*component(probe.extents,q)
                  +std::fabs(component(box.axes[j],q))*component(probe.extents,p);
        }
        float gap;
        if (projected>=radius) {
            if (projected>velocity+radius) return {};
            gap=projected-radius;
        } else if (projected<=-radius) {
            if (projected<velocity-radius) return {};
            gap=radius+projected;
        } else gap=-velocity;
        if (velocity!=0) {
            const float fraction=gap/velocity;
            if (best<fraction) { best=fraction; selected=i; }
        }
    }
    return {true,best,axes[selected]};
}

namespace {
xmlNode* child(xmlNode* node,const char* name) {
    for (auto* c=node ? node->children : nullptr;c;c=c->next)
        if (c->type==XML_ELEMENT_NODE && xmlStrEqual(c->name,BAD_CAST name)) return c;
    return nullptr;
}
std::string attribute(xmlNode* node,const char* name) {
    if (!node) return {};
    xmlChar* value=xmlGetProp(node,BAD_CAST name);
    if (!value) return {};
    std::string result(reinterpret_cast<char*>(value)); xmlFree(value); return result;
}
float number(xmlNode* node,const char* name) {
    const auto text=attribute(node,name);
    require(!text.empty(),"Missing original level numeric attribute");
    std::istringstream parser(text); parser.imbue(std::locale::classic());
    float value; require(static_cast<bool>(parser>>value),"Invalid original level number");
    parser>>std::ws;
    require(parser.eof() && std::isfinite(value),"Non-finite or trailing original level number");
    return value;
}
Vec3 vector(xmlNode* node) { return {number(node,"x"),number(node,"y"),number(node,"z")}; }
template<class F> void elements(xmlNode* parent,const char* name,F action) {
    for (auto* n=parent ? parent->children : nullptr;n;n=n->next)
        if (n->type==XML_ELEMENT_NODE && xmlStrEqual(n->name,BAD_CAST name)) action(n);
}
} // namespace
LevelMetadata read_level_metadata(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    require(size<=static_cast<std::uintmax_t>(std::numeric_limits<int>::max()),
            "Original level XML exceeds parser size");
    std::ifstream input(path,std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size),'\0');
    require(static_cast<bool>(input.read(bytes.data(),static_cast<std::streamsize>(size))),
            "Cannot read original level XML");
    // Original TinyXML accepts malformed adjacent camera attributes; 00418ae0
    // ignores this entire section. Omit ONLY that exact bounded section in memory,
    // keeping all preceding/following bytes and strict parsing for consumed data.
    constexpr std::string_view open="<camera_list>",close="</camera_list>";
    const auto begin=bytes.find(open),end=bytes.find(close);
    if (begin!=std::string::npos || end!=std::string::npos) {
        require(begin!=std::string::npos && end!=std::string::npos && begin<end
                && bytes.find(open,begin+open.size())==std::string::npos
                && bytes.find(close,end+close.size())==std::string::npos,
                "Ambiguous or unterminated ignored camera_list");
        bytes.erase(begin,end+close.size()-begin);
    }
    std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> document(
        xmlReadMemory(bytes.data(),static_cast<int>(bytes.size()),path.string().c_str(),
                      "WINDOWS-1254",XML_PARSE_NONET),xmlFreeDoc);
    require(document!=nullptr,"Cannot parse original level XML");
    auto* root=xmlDocGetRootElement(document.get());
    require(root && xmlStrEqual(root->name,BAD_CAST "level"),"Invalid original level root");
    require(document->intSubset==nullptr && document->extSubset==nullptr,"Level DTD not allowed");
    LevelMetadata level;
    auto* data=child(root,"data");
    level.boundsMin=vector(child(data,"aabbmin")); level.boundsMax=vector(child(data,"aabbmax"));
    level.sphereCenter=vector(child(data,"bsphere")); level.sphereRadius=number(child(data,"bsphere"),"r");
    elements(child(root,"part_list"),"part",[&](xmlNode* node) {
        level.parts.push_back({attribute(node,"name"),attribute(node,"filename")});
    });
    elements(child(root,"model_list"),"model",[&](xmlNode* node) {
        LevelModel model{attribute(node,"name"),attribute(node,"filename"),::yami::identity_matrix()};
        std::size_t column=0;
        elements(child(node,"orient"),"row",[&](xmlNode* row) {
            require(column<4,"Too many original model orientation rows");
            for (std::size_t i=0;i<4;++i)
                model.transform.values[column*4+i]=number(row,i==0 ? "m0" : i==1 ? "m1" : i==2 ? "m2" : "m3");
            ++column;
        });
        require(column==4,"Original model orientation requires four rows");
        level.models.push_back(std::move(model));
    });
    elements(child(root,"bots"),"bot",[&](xmlNode* node) {
        level.bots.push_back({attribute(node,"name"),attribute(node,"path"),vector(child(node,"pos"))});
    });
    elements(child(root,"healths"),"health",[&](xmlNode* node) {
        LevelHealth health;
        const auto kind=attribute(node,"big");
        health.kind=kind=="true" ? LevelHealth::Kind::Big : kind=="ok" ? LevelHealth::Kind::Arrow
                    : kind=="peyami" ? LevelHealth::Kind::Peyami : LevelHealth::Kind::Small;
        health.name=attribute(node,"name"); health.position=vector(child(node,"pos"));
        level.healths.push_back(std::move(health));
    });
    elements(child(root,"collision_obbs"),"col",[&](xmlNode* node) {
        Obb box;
        box.name=attribute(node,"name"); box.center=vector(child(node,"center"));
        box.extents=vector(child(node,"extents"));
        box.axes={vector(child(node,"axis_x")),vector(child(node,"axis_y")),vector(child(node,"axis_z"))};
        if (box.name.empty() || box.name.front()=='x') level.blocking.push_back(std::move(box));
        else level.triggers.push_back(std::move(box));
    });
    return level;
}
std::int32_t entity_type(std::string_view model) noexcept {
    if (model=="robot_utu") return 1;
    if (model=="robot_yayli") return 2;
    if (model=="robot_bisiklet") return 3;
    if (model=="robot_pervane") return 4;
    if (model=="robot_soba") return 5;
    return 1; // 00427350 fallback dispatch is iron AI, not type zero
}
Vec3 level_player_start(std::int32_t level) {
    switch (level) {
    case 1:return {-977.4297f,362.7758f,1434.7706f};
    case 2:return {-4433.027f,29.6718f,2811.5317f};
    case 3:return {-1572.2731f,289.4572f,1825.4435f};
    case 4:return {-5450,147,-631};
    default:throw std::runtime_error("Unknown original player start level");
    }
}

namespace {
::yami::Mat4 camera_transform(const ::yami::Mat4& player,float pitch,float distance) {
    auto result=player; result.values[13]+=80;
    ::yami::entity::rotate_world_x(result,pitch);
    result.values[12]+=result.values[8]*distance;
    result.values[13]+=result.values[9]*distance;
    result.values[14]+=result.values[10]*distance;
    return result;
}
Vec3 translation(const ::yami::Mat4& matrix) {
    return {matrix.values[12],matrix.values[13],matrix.values[14]};
}
} // namespace
PlayerCameraState initialize_player_camera(std::int32_t level) noexcept {
    PlayerCameraState state;
    if (level==4) {
        state.pitch=state.defaultPitch=-10;
        state.distance=state.distanceBase=state.defaultDistance=250;
    }
    return state;
}
::yami::Mat4 solve_player_camera(const ::yami::Mat4& player,PlayerCameraState& state,
                                std::uint32_t now,const std::vector<Obb>& boxes,
                                const std::vector<EntityState>& entities,
                                std::optional<std::size_t> playerIndex) {
    auto candidate=camera_transform(player,state.defaultPitch,state.distance);
    auto blocked=[&](const ::yami::Mat4& matrix) {
        return overlap({translation(matrix),{10,10,10}},boxes,entities,playerIndex).kind
               !=CollisionHit::Kind::None;
    };
    if (!blocked(candidate)) {
        if (!state.falling) { state.fallingSince=now; state.falling=true; }
        if (now-state.fallingSince>500 && std::fabs(state.distance-state.defaultDistance)>0.001f) {
            const float old=state.distance;
            const float elapsed=(static_cast<float>(now)-static_cast<float>(state.fallingSince)-500)/1000;
            state.distance=std::min(state.defaultDistance,
                                    state.distanceBase+elapsed*elapsed*state.acceleration*0.5f);
            candidate=camera_transform(player,state.defaultPitch,state.distance);
            if (blocked(candidate)) {
                state.distanceBase=old; state.distance=old;
                candidate=camera_transform(player,state.defaultPitch,state.distance);
                state.falling=false;
            }
        }
    } else {
        state.fallingSince=now;
        float distance=state.distance-3;
        candidate=camera_transform(player,state.defaultPitch,distance);
        while (distance>=0 && blocked(candidate)) {
            distance-=3; candidate=camera_transform(player,state.defaultPitch,distance);
        }
        if (distance<0) distance=0;
        state.distance=distance; state.distanceBase=distance;
        state.pitch=state.defaultPitch; state.falling=false;
        candidate=camera_transform(player,state.defaultPitch,state.distance);
    }
    return candidate;
}

void append_health_scan_order(std::vector<HealthState>& healths,std::size_t index) {
    require(index<healths.size(),"Invalid retained pickup index");
    auto& head=healths[0].scanHead;
    if (head==index) head=next_health(healths,index);
    else {
        std::size_t previous=head;
        while (previous<healths.size() && next_health(healths,previous)!=index)
            previous=next_health(healths,previous);
        require(previous<healths.size(),"Retained pickup missing from scan order");
        healths[previous].scanNext=next_health(healths,index);
    }
    if (head>=healths.size()) head=index;
    else {
        std::size_t tail=head;
        while (next_health(healths,tail)<healths.size()) tail=next_health(healths,tail);
        healths[tail].scanNext=index;
    }
    healths[index].scanNext=healths.size();
}
WorldSweep sweep_world(const Aabb& probe,Vec3 direction,float distance,
                       const std::vector<Obb>& boxes,const std::vector<EntityState>& entities,
                       const std::vector<HealthState>& healths,std::int32_t level,
                       std::optional<std::size_t> owner,bool livingOnly) noexcept {
    WorldSweep nearest;
    float best=1e19f,last=0;
    const Vec3 displacement{direction.x*distance,direction.y*distance,direction.z*distance};
    auto consider=[&](const Obb& box,WorldSweep::Kind kind,std::size_t index,bool nonnegative) {
        const auto hit=sweep_box(box,probe,displacement);
        if (!hit.hit) return;
        last=hit.fraction;
        if (last<best && (!nonnegative || last>=0)) {
            best=last; nearest={kind,index,last,hit.normal};
        }
    };
    for (std::size_t i=first_health(healths);i<healths.size();i=next_health(healths,i))
        if (!healths[i].removed) consider(healths[i].bounds,WorldSweep::Kind::Health,i,false);
    if (level!=4)
        for (std::size_t i=0;i<entities.size();++i)
            if (owner!=i && !entities[i].hidden && (!livingOnly || !entities[i].dead))
                consider(entity_box(entities[i]),WorldSweep::Kind::Entity,i,true);
    for (std::size_t i=0;i<boxes.size();++i)
        if (!boxes[i].disabled) consider(boxes[i],WorldSweep::Kind::Static,i,false);
    nearest.fraction=last<=0.1f ? 0 : best-0.1f;
    return nearest;
}
void set_force_rate(ForceState& force,float rate) noexcept {
    constexpr float dt=0.033333335f; // 004596c4
    const long double decay=static_cast<long double>(rate)*dt;
    force.rate=rate; force.ticks=0; force.decay=static_cast<float>(decay);
    force.stepDecay=static_cast<float>(decay*dt);
    force.halfStep=rate*from_bits(0x3a11a2b5U);
}
void request_movement(EntityState& entity,ForceState& force,Move move,Vec3 right) noexcept {
    if (move==Move::Stop) {
        force.direction={}; // 00406560; does not zero scalar momentum
        return;
    }
    if (entity.role==0) {
        if (entity.boostTicks>0) { --entity.boostTicks; force.requestedSpeed=370; }
        else force.requestedSpeed=185;
    }
    if (!movement_allowed(entity)) return;
    Vec3 direction;
    switch (move) {
    case Move::Forward:direction=cross(right,{0,-1,0});break;
    case Move::Back:direction=cross(right,{0,1,0});break;
    case Move::Right:direction=right;normalize(direction);break;
    case Move::Left:direction={-right.x,-right.y,-right.z};normalize(direction);break;
    case Move::Stop:return;
    }
    force.direction={force.direction.x+direction.x,force.direction.y+direction.y,
                     force.direction.z+direction.z};
    normalize(force.direction);
    entity.movementRequested=true;
}
namespace {
void collect_health(std::size_t owner,std::size_t index,std::int32_t level,
                    std::vector<EntityState>& entities,std::vector<HealthState>& healths,
                    std::int32_t& score,PhysicsEffects& effects) noexcept {
    auto& health=healths[index];
    if (health.hidden) return;
    auto& player=entities[owner];
    player.health=std::min(1.0f,player.health+health.amount);
    player.boostTicks+=health.boost;
    if (health.big) player.bigHealthTicks=health.boost;
    if (level==3 && health.big) { health.hidden=true; player.retainedBigHealth=index; }
    health.removed=true; // original unlinks even retained hidden level-three models
    score+=30;
    if (effects.collectedCount<effects.collected.size())
        effects.collected[effects.collectedCount++]=index;
}
} // namespace
void move_swept(std::size_t owner,Vec3 direction,float distance,std::int32_t depth,
                Vec3 gravity,std::uint32_t frameNow,std::int32_t level,
                const std::vector<Obb>& boxes,std::vector<EntityState>& entities,
                std::vector<HealthState>& healths,std::int32_t& score,PhysicsEffects& effects) noexcept {
    (void)frameNow;
    if (depth<1) return;
    auto& entity=entities[owner];
    Vec3 start=entity.committedPosition;
    const auto hit=sweep_world({start,entity.bounds.extents},direction,distance,
                               boxes,entities,healths,level,owner);
    if (hit.kind==WorldSweep::Kind::None) {
        entity.committedPosition=along(start,direction,distance); return;
    }
    if (entity.role==0 && hit.kind==WorldSweep::Kind::Health) {
        collect_health(owner,hit.index,level,entities,healths,score,effects);
        const float traveled=hit.fraction*distance;
        if (traveled>0) entity.committedPosition=along(start,direction,traveled);
        const float remaining=distance-traveled;
        if (remaining>0.01f)
            move_swept(owner,direction,remaining,depth-1,gravity,frameNow,level,
                        boxes,entities,healths,score,effects);
        return;
    }
    if (entity.role==0 && hit.kind==WorldSweep::Kind::Entity) return;
    const auto raised=along(start,gravity,-20);
    const auto retry=sweep_world({raised,entity.bounds.extents},direction,distance,
                                 boxes,entities,healths,level,owner);
    if (retry.kind==WorldSweep::Kind::None) {
        entity.committedPosition=along(raised,direction,distance); return;
    }
    const float traveled=hit.fraction*distance;
    if (traveled>0) entity.committedPosition=along(start,direction,traveled);
    const float remaining=distance-traveled;
    if (remaining<=0.01f) return;
    auto normal=hit.normal; normalize(normal);
    const auto projection=dot(normal,direction);
    Vec3 slide{direction.x-normal.x*projection,direction.y-normal.y*projection,
               direction.z-normal.z*projection};
    normalize(slide);
    move_swept(owner,slide,remaining,depth-1,gravity,frameNow,level,
                boxes,entities,healths,score,effects);
}
PhysicsEffects tick_body(std::size_t owner,ForceState& force,Vec3 gravity,
                         std::uint32_t now,std::int32_t level,const std::vector<Obb>& boxes,
                         std::vector<EntityState>& entities,std::vector<HealthState>& healths,
                         std::int32_t& score) noexcept {
    PhysicsEffects effects;
    auto& entity=entities[owner];
    if (entity.dead) { entity.movementRequested=false; return effects; }
    entity.positionStart=now;
    if (force.speed!=0 || entity.movementRequested) {
        float distance;
        if (entity.movementRequested) {
            force.speed=force.requestedSpeed;
            force.ticks=static_cast<std::int32_t>(
                static_cast<double>(force.requestedSpeed)/force.decay);
            distance=static_cast<float>(force.ticks)*force.stepDecay+force.halfStep;
        } else {
            distance=static_cast<float>(force.ticks)*force.stepDecay-force.halfStep;
            --force.ticks; force.speed-=force.decay;
            if (force.speed<=0 || force.ticks<=0) {
                force.speed=0; force.ticks=0; force.direction={};
            }
        }
        move_swept(owner,force.direction,distance,5,gravity,now,level,
                    boxes,entities,healths,score,effects);
    }
    const Vec3 start=entity.committedPosition;
    const auto vertical=sweep_world({start,entity.bounds.extents},gravity,20,
                                    boxes,entities,healths,level,owner);
    if (vertical.kind==WorldSweep::Kind::None)
        entity.committedPosition=along(start,gravity,19.9f);
    else if (vertical.fraction*20>0)
        entity.committedPosition=along(start,gravity,vertical.fraction*20);
    set_position(entity,entity.committedPosition,false,now);
    entity.previousPosition=entity.committedPosition;
    entity.movementRequested=false; // 00415b10 does this after virtual entity tick
    return effects;
}
void initialize_entity(EntityState& entity,ForceState& force,std::string_view name,
                       const ::yami::Model& model,bool player,std::int32_t level,
                       std::uint32_t now) {
    entity.model=std::string(name); entity.role=player ? 0 : 1;
    entity.energy=1; entity.action=0; entity.animationFrame=0;
    entity.actionStart=0; entity.actionCompleted=0;
    entity.dead=false; entity.dying=false; entity.movementRequested=false;
    entity.bounds.extents=::yami::model_half_extents(model);
    entity.bounds.axes={Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1}};
    entity.health=1; entity.weapon=0;
    if (name=="robot_utu") entity.weapon=3;
    else if (name=="robot_yayli") { entity.weapon=4; entity.health=2; }
    else if (name=="robot_bisiklet") { entity.weapon=6; entity.health=4; }
    else if (name=="robot_pervane") { entity.weapon=5; entity.health=5000; }
    else if (name=="robot_soba") { entity.weapon=7; entity.health=6; }
    else if (name=="yaman_silahli") entity.weapon=1;
    else if (name=="yaman_buyuk_silahli") entity.weapon=2;
    set_force_rate(force,player ? 400 : 300);
    force.requestedSpeed=player ? 185 : name=="robot_utu" ? 170
                           : name=="robot_bisiklet" ? 300 : name=="robot_pervane" ? 250
                           : name=="robot_soba" ? 40 : 180;
    entity.positionStart=now;
    if (player) set_position(entity,level_player_start(level),true,now);
}

void request_flight_movement(EntityState& entity,FlightState& state,Move move) noexcept {
    constexpr float drain=0.002f;
    auto turn=[&](float& angle,float target) { angle+=(target-angle)*0.125f; };
    switch (move) {
    case Move::Forward:
        state.forward=true;
        if (state.lateralSpeed-1.5f>=-500) {
            if (entity.health>drain) { state.verticalSpeed-=1.5f; entity.health-=drain; }
            turn(state.pitch,-8);
        }
        break;
    case Move::Back:
        state.back=true;
        if (state.lateralSpeed-1.5f<500) {
            if (entity.health>drain) { state.verticalSpeed+=1.5f; entity.health-=drain; }
            turn(state.pitch,8);
        }
        break;
    case Move::Left:
        state.left=true;
        if (state.lateralSpeed-1.5f>=-500) {
            if (entity.health>drain) { state.lateralSpeed-=1.5f; entity.health-=drain; }
            turn(state.roll,8);
        }
        break;
    case Move::Right:
        state.right=true;
        if (state.lateralSpeed+1.5f<500) {
            if (entity.health>drain) { state.lateralSpeed+=1.5f; entity.health-=drain; }
            turn(state.roll,-8);
        }
        break;
    case Move::Stop: state.left=state.right=state.forward=state.back=false; break;
    }
}
FlightInputEffects apply_flight_input(EntityState& entity,FlightState& state,
                                      const FlightInput& input,bool previousMouseActive) noexcept {
    FlightInputEffects effects;
    effects.attack=input.keys.fire || input.keys.secondaryFire || input.mouseFire;
    const auto decision=select_player_input(input.keys,entity);
    if (decision.moves[0]!=Move::Stop || !previousMouseActive)
        for (std::size_t i=0;i<decision.moveCount;++i)
            request_flight_movement(entity,state,decision.moves[i]);
    const float horizontal=-input.smoothedMouseX;
    if (horizontal!=0) {
        effects.mouseActive=true; // even movement inside the two-unit dead zone
        if (std::fabs(horizontal)>2)
            request_flight_movement(entity,state,horizontal>=0 ? Move::Left : Move::Right);
    }
    const float vertical=-input.smoothedMouseY;
    if (vertical!=0) {
        if (std::fabs(vertical)>2) {
            request_flight_movement(entity,state,vertical>=0 ? Move::Back : Move::Forward);
            effects.mouseActive=true;
        }
        effects.interact=input.interact; // original use-key check is nested here
    }
    effects.openMenu=input.pauseHeld && !input.pauseHeldPreviously;
    return effects;
}
FlightEffects tick_flight(std::size_t owner,std::optional<std::size_t> passenger,
                          FlightState& state,float& phase,std::uint32_t now,
                          const std::vector<Obb>& boxes,std::vector<EntityState>& entities,
                          std::vector<HealthState>& healths,std::int32_t& score) noexcept {
    FlightEffects effects;
    auto& plane=entities[owner];
    set_position(plane,plane.committedPosition,false,now);
    plane.previousPosition=plane.committedPosition;
    const long double angle=static_cast<long double>(phase)+0.1f;
    phase=static_cast<float>(angle);
    const float wave=static_cast<float>(std::sin(angle));
    plane.committedPosition.x+=state.forwardStep;
    plane.committedPosition.y+=state.verticalSpeed+wave;
    plane.committedPosition.z+=state.lateralSpeed;
    Vec3 direction{state.forwardStep,state.verticalSpeed,state.lateralSpeed};
    const float length=normalize(direction);
    const auto target=along(plane.committedPosition,direction,length);
    const Aabb probe{{target.x,target.y-30,target.z},plane.bounds.extents};
    // 0040b1d0 returns the FIRST endpoint overlap, never the nearest swept hit.
    // 00408e50 ignores all non-pickup collisions; they do not stop the airplane.
    for (std::size_t i=first_health(healths);i<healths.size();i=next_health(healths,i)) {
        if (!healths[i].removed && intersects(healths[i].bounds,probe)) {
            collect_health(owner,i,4,entities,healths,score,effects.pickups);
            break;
        }
    }
    (void)boxes; // remaining static/level-four collision return is unused originally
    if (plane.committedPosition.y>730 || plane.committedPosition.y<85)
        plane.committedPosition.y-=wave+state.verticalSpeed;
    if (plane.committedPosition.z>760 || plane.committedPosition.z<-2075)
        plane.committedPosition.z-=state.lateralSpeed;
    if (plane.health>=0.001f) {
        plane.health-=0.001f;
        transition_action(plane,plane,0,now);
    } else {
        state.verticalSpeed-=0.2f;
        transition_action(plane,plane,1,now);
        if (plane.actionCompleted!=0) {
            plane.dead=true; effects.showDeathMenu=true;
        }
    }
    if (!effects.showDeathMenu) {
        auto damp=[](float& speed) {
            if (speed>0.5f) speed-=0.5f;
            else if (speed<-0.5f) speed+=0.5f;
            else speed=0;
        };
        damp(state.verticalSpeed); damp(state.lateralSpeed);
        if (!state.left && !state.right) state.roll-=state.roll*0.125f;
        if (!state.forward && !state.back) state.pitch-=state.pitch*0.125f;
        if (plane.committedPosition.x>-1015) effects.playEndingMovie=true;
        else plane.energy=(4405-(-plane.committedPosition.x-1015))*from_bits(0x396c6e99U);
    }
    if (passenger) {
        entities[*passenger].health=plane.health;
        entities[*passenger].committedPosition=plane.committedPosition;
    }
    return effects;
}
::yami::Mat4 flight_camera(const ::yami::Mat4& player,const PlayerCameraState& camera,
                          const FlightState& flight) noexcept {
    auto result=player; result.values[13]+=10;
    ::yami::entity::rotate_world_x(result,flight.pitch+camera.defaultPitch);
    ::yami::entity::rotate_world_y(result,flight.roll);
    ::yami::entity::rotate_world_z(result,flight.roll);
    result.values[12]+=result.values[8]*camera.distance;
    result.values[13]+=result.values[9]*camera.distance;
    result.values[14]+=result.values[10]*camera.distance;
    return result;
}
} // namespace yami::gameplay
