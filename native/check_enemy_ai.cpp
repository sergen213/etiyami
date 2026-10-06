#include "enemy_ai.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>

using namespace yami;
using namespace yami::enemy_ai;
namespace {
bool attack(const std::vector<Operation>& operations) {
    return std::any_of(operations.begin(),operations.end(),[](const auto& op) {
        return op.type == Operation::Type::Attack;
    });
}
void apply_actions(const std::vector<Operation>& operations, gameplay::EntityState& entity,
                   gameplay::EntityState& player, std::uint32_t now) {
    for (const auto& op : operations)
        if (op.type == Operation::Type::Action)
            gameplay::transition_action(entity,player,op.action,now);
}
}
int main() {
    Random random;
    assert(random.next() == 41);
    assert(random.next() == 18467);
    assert(random.next() == 6334);
    MotionHistory motion;
    motion.sample_tick(10,{1,0,0});
    assert(motion.latest().x == 10 && motion.average.x == 1);
    for (int i = 0; i < 4; ++i) motion.sample_tick(20,{1,0,0});
    assert(motion.latest().x == 10);
    motion.sample_tick(20,{1,0,0});
    assert(motion.latest().x == 20 && motion.average.x == 3);
    const std::array<Kind,5> kinds{Kind::Utu,Kind::Yayli,Kind::Bisiklet,Kind::Pervane,Kind::Soba};
    const std::array<int,5> firstAttack{10,24,0,34,7};
    const std::array<int,5> shots{1,5,10,5,1};
    const std::array<int,5> ticks{40,40,70,80,40};
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        std::vector<gameplay::EntityState> entities(2);
        auto& enemy = entities[0];
        auto& player = entities[1];
        enemy.bounds.extents = {1,1,1};
        player.bounds.extents = {1,1,1};
        player.role = 0;
        player.position = {0,0,-128};
        auto state = initialize(kinds[k],enemy.position);
        state.enabled = true;
        Input input;
        input.now = 1;
        input.forwardAxis = {0,0,1};
        input.rightAxis = {1,0,0};
        std::vector<Operation> operations;
        Random stream;
        tick(state,enemy,player,input,{},entities,0,1,stream,operations);
        assert(state.mode == 50 && state.unseenTime == 0 && !attack(operations));
        int count = 0;
        for (int frame = 0; frame <= ticks[k]; ++frame) {
            input.now += 33;
            tick(state,enemy,player,input,{},entities,0,1,stream,operations);
            apply_actions(operations,enemy,player,input.now);
            if (frame < firstAttack[k]) assert(!attack(operations));
            if (frame == firstAttack[k]) assert(attack(operations));
            count += int(attack(operations));
            assert(enemy.attackGuard == (frame < ticks[k]));
        }
        assert(count == shots[k]);
    }
    // Unsigned elapsed clock and the original strictly-greater search timeouts.
    std::vector<gameplay::EntityState> entities(2);
    auto& enemy = entities[0];
    auto& player = entities[1];
    player.hidden = true;
    player.position = {0,0,-128};
    auto state = initialize(Kind::Utu,enemy.position);
    state.enabled = true;
    state.mode = 50;
    state.unseenTime = 14967;
    state.lastTime = 1;
    Input input;
    input.now = 34;
    input.forwardAxis = {0,0,1};
    input.rightAxis = {1,0,0};
    Random stream;
    std::vector<Operation> operations;
    tick(state,enemy,player,input,{},entities,0,1,stream,operations);
    assert(state.mode == 50 && state.unseenTime == 15000);
    input.now += 33;
    tick(state,enemy,player,input,{},entities,0,1,stream,operations);
    assert(state.mode == 30 && state.queuedEvents == 0);
    state.unseenTime = 45000;
    input.now += 33;
    tick(state,enemy,player,input,{},entities,0,1,stream,operations);
    assert(state.mode == 10);
    // 32-bit wrapping elapsed time is intentional, not a signed-clock reset.
    state.lastTime = 0xfffffff0U;
    state.unseenTime = 0;
    input.now = 0x11;
    tick(state,enemy,player,input,{},entities,0,1,stream,operations);
    assert(state.unseenTime == 33);
    std::cout << "Recovered five enemy AI transitions, attack windows, and clock boundaries checked\n";
}
