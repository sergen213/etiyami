#include "enemy_ai.hpp"
#include <algorithm>
#include <cmath>

namespace yami::enemy_ai {
std::uint32_t Random::next() noexcept {
    state = state * 0x343fdU + 0x269ec3U;
    return (state >> 16) & 0x7fffU;
}
float Random::between(float low, float high) noexcept {
    // 00413570 uses the stored float reciprocal, with x87 intermediates.
    return static_cast<float>((double(high) - low) * next() * 0.000030518509447574615 + low);
}
void MotionHistory::sample_tick(float playerForceSpeed, Vec3 direction) noexcept {
    if (ticks % 5 == 0) {
        const Vec3 sample{playerForceSpeed*direction.x,playerForceSpeed*direction.y,playerForceSpeed*direction.z};
        const auto old = samples[nextSample];
        total = {static_cast<float>((double(sample.x)-old.x)+total.x),
                 static_cast<float>((double(sample.y)-old.y)+total.y),
                 static_cast<float>((double(sample.z)-old.z)+total.z)};
        samples[nextSample] = sample;
        average = {total.x*0.1f,total.y*0.1f,total.z*0.1f};
        lastSample = nextSample;
        nextSample = (nextSample+1)%10;
    }
    ++ticks;
}
State initialize(Kind kind, Vec3 position) {
    State s;
    s.kind = kind;
    s.lastPosition = position;
    s.detectStuck = kind != Kind::Soba;
    s.evadeAfterAttack = kind == Kind::Yayli || kind == Kind::Pervane;
    s.evadeDamage = kind == Kind::Utu || kind == Kind::Yayli;
    s.reactHit = kind != Kind::Pervane && kind != Kind::Soba;
    s.alternateEvade = kind == Kind::Pervane;
    return s;
}
namespace {
float dot(Vec3 a, Vec3 b) {
    return static_cast<float>(double(a.z)*b.z + double(a.y)*b.y + double(a.x)*b.x);
}
double clamped_acos(float value) {
    // Explicit clamps406bc1..406bf5/4069e7..406a1b/406dd6..406e09
    // are lost entirely by the original decompiler's x87 parameter recovery.
    return std::acos(double(std::clamp(value,-1.0f,1.0f)));
}
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float square_distance(Vec3 a, Vec3 b) {
    const auto d = subtract(a,b);
    // 00414600 instruction order: z*z + x*x + y*y.
    return static_cast<float>(double(d.z)*d.z + double(d.x)*d.x + double(d.y)*d.y);
}
float angle(Vec3 a, Vec3 b) {
    a.y = b.y = 0;
    gameplay::normalize(a);
    gameplay::normalize(b);
    double result = clamped_acos(dot(a,b));
    if (double(b.z)*a.x - double(b.x)*a.z > 0) result = -result;
    return static_cast<float>(result * 57.295779); // double at00459708
}
float bearing(const Input& in, Vec3 origin, Vec3 target) {
    // 00406b40 negates model row2 before its horizontal signed acos.
    return angle({-in.forwardAxis.x,-in.forwardAxis.y,-in.forwardAxis.z},subtract(target,origin));
}
void clear(State& s) {
    s.events.clear(); s.firstEvent = 0; s.queuedEvents = 0;
}
void append(State& s, std::uint8_t type, float parameter = 0,
            std::uint32_t repeats = 1, bool immediate = false) {
    if (!repeats) return;
    if (!s.queuedEvents) { s.events.clear(); s.firstEvent = 0; }
    s.queuedEvents += repeats;
    if (!s.events.empty()) {
        auto& last = s.events.back();
        if (last.type == type && last.parameter == parameter && last.immediate == immediate) {
            last.repeats += repeats; return;
        }
    }
    s.events.push_back({type,parameter,repeats,immediate});
}
void move(State& s, bool back, std::uint32_t count) { append(s,back ? 4 : 3,0,count); }
// 00407270..00407d80 append the remainder even when it is exactly zero.
void turn_events(State& s, std::uint8_t type, float total, float step) {
    const float amount = std::abs(total);
    const auto count = static_cast<std::uint32_t>(double(amount)/step);
    const float sign = total < 0 ? -1.0f : 1.0f;
    const float remainder = static_cast<float>(double(amount) - double(count)*step);
    append(s,type,sign*step,count);
    append(s,type,sign*remainder);
}
void turn_only(State& s, float degrees, float step) {
    turn_events(s,degrees < 0 ? 2 : 1,std::abs(degrees),step);
}
float speed(const State& s, int rate) {
    const float base = s.kind == Kind::Pervane ? 0.625f : s.kind == Kind::Soba ? 0.25f : 1.0f;
    return std::ldexp(base,rate);
}
void curved(State& s, int direction, int side, int rate, float fraction, Random& rng) {
    if (side == 999) side = int(rng.next()%2);
    if (rate < 0 || rate > 4) return; // original speed switch default
    const float total = fraction*360.0f;
    // direction2 forward:5+angle/5-angle; direction3 back:6-angle/6+angle
    // direction0 left:9-angle/9+angle; direction1 right:10+angle/10-angle.
    const auto type = std::uint8_t(direction == 2 ? 5 : direction == 3 ? 6 : direction == 0 ? 9 : 10);
    const bool negative = (direction == 3 || direction == 0) ? side == 0 : side != 0;
    turn_events(s,type,negative ? -total : total,speed(s,rate));
}
void pair_curve(State& s, int side, int rate, Random& rng) {
    if (side == 999) side = int(rng.next()%2);
    curved(s,2,side,rate,0.9f,rng); curved(s,2,1-side,rate,0.9f,rng);
}
void asymmetric_curve(State& s, int side, int rate, Random& rng) {
    if (side == 999) side = int(rng.next()%2);
    curved(s,2,side,rate,0.25f,rng);
    curved(s,2,1-side,rate,1.0f,rng);
    curved(s,2,side,rate,0.75f,rng);
}
void zigzag(State& s, int side, int direction, int rate, bool both, Random& rng) {
    if (side == 999) side = int(rng.next()%2);
    if (direction == 999) direction = 2+int(rng.next()%2);
    const bool yayli = s.kind == Kind::Yayli;
    const std::uint32_t first = !both && yayli ? 15 : 5;
    const std::uint32_t middle = !both && yayli ? 20 : 10;
    const float fraction = !both && s.kind == Kind::Pervane && direction == 2 ? 0.1f : 0.2f;
    move(s,direction != 2,first);
    curved(s,direction,side,rate,fraction,rng);
    move(s,direction != 2,middle);
    curved(s,direction,1-side,rate,fraction,rng);
    if (both) {
        const int other = direction == 2 ? 3 : 2;
        curved(s,other,side,rate,0.2f,rng);
        move(s,other != 2,10);
        curved(s,other,1-side,rate,0.2f,rng);
    }
}
void weave(State& s, int side, int direction, Random& rng) {
    // The third float argument is absent from Ghidra signatures: objdump at
    // 43180d/4348bf/403939/425410/42b94d proves1.0,1.3,1.0,1.0,1.0.
    if (side == 999) side = int(rng.next()%2);
    if (direction == 999) direction = 2+int(rng.next()%2);
    const float duration = s.kind == Kind::Yayli ? 1.3f : 1.0f;
    const float step = s.kind == Kind::Yayli ? 8 : s.kind == Kind::Pervane ? 10 : s.kind == Kind::Soba ? 4 : 16;
    move(s,direction != 2,static_cast<std::uint32_t>(double(duration)*5));
    const auto type = std::uint8_t(direction == 2 ? 5 : 6);
    float sign = direction == 2 ? (side == 0 ? 1.0f : -1.0f) : (side == 0 ? -1.0f : 1.0f);
    turn_events(s,type,sign*54.000004f,step);
    move(s,direction != 2,static_cast<std::uint32_t>(double(duration)*20));
    turn_events(s,type,-sign*108.00001f,step);
    move(s,direction != 2,static_cast<std::uint32_t>(double(duration)*19.5));
    turn_events(s,type,sign*54.000004f,step);
}
void random_curve(State& s, int direction, Random& rng) {
    const bool yayli = s.kind == Kind::Yayli;
    const float fraction = rng.between(yayli ? 0.3f : 0.2f,yayli ? 0.5f : 0.7f);
    const int rate = int(rng.next()%2)+(yayli ? 1 : 2);
    const int side = int(rng.next()%2);
    curved(s,direction,side,rate,fraction,rng);
}
void wander(State& s, Random& rng) {
    const auto choice = rng.next()%3;
    const bool back = s.blocked && s.blockedBack;
    if (!choice) { move(s,back,20); return; }
    const float high = s.kind == Kind::Yayli ? 0.5f : s.kind == Kind::Soba ? 0.6f : 0.7f;
    const float fraction = rng.between(0.2f,high);
    const int rate = int(rng.next()%2)+2;
    curved(s,back ? 3 : 2,int(choice-1),rate,fraction,rng);
}
void escape(State& s, Random& rng) {
    append(s,12,0,1,true);
    const float step = s.kind == Kind::Pervane ? 5 : s.kind == Kind::Soba ? 2 : 8;
    if (s.escapeDirection == 2) {
        move(s,true,10);
        turn_events(s,6,rng.next()%2 == 0 ? -162.0f : 162.0f,step);
    } else if (s.escapeDirection == 3) {
        move(s,false,10);
        turn_events(s,5,rng.next()%2 == 0 ? 162.0f : -162.0f,step);
    } else if (s.escapeDirection == 0) {
        if (s.kind == Kind::Pervane) append(s,8,0,15);
        else turn_events(s,10,-45,step);
    } else if (s.escapeDirection == 1) {
        if (s.kind == Kind::Pervane) append(s,7,0,15);
        else turn_events(s,9,45,step);
    }
    append(s,13,0,1,true);
}
// 431a70/434b60/403a50/425660/42bbb0: reaction patterns.
void react(State& s, bool useCollision, Random& rng) {
    append(s,12,0,1,true);
    const bool blocked = useCollision && s.blocked;
    int choice;
    if (s.kind == Kind::Pervane) choice = s.alternateEvade ? int(rng.next()%3)+1 : 1;
    else {
        if ((s.kind == Kind::Utu || s.kind == Kind::Soba) && !s.alternateEvade) (void)rng.next();
        choice = s.alternateEvade ? int(rng.next()%2)+1 : 1;
    }
    const int direction = !blocked ? 999 : s.blockedForward ? 2 : 3;
    if (s.kind == Kind::Bisiklet) zigzag(s,999,direction,3,false,rng);
    else if (choice == 1) {
        if (s.kind != Kind::Pervane || !blocked || s.blockedForward || s.blockedBack)
            zigzag(s,999,direction,s.kind == Kind::Yayli ? 2 : 3,false,rng);
    } else if (s.kind == Kind::Pervane && choice == 2) {
        const int rate = int(rng.next()%2);
        (void)rng.next(); // observed unused random call425746
        if (!blocked) curved(s,rng.next()%2 == 0 ? 0 : 1,0,rate,0.1f,rng);
        else if (s.blockedLeft) curved(s,0,0,rate,0.1f,rng);
        else if (s.blockedRight) curved(s,1,0,rate,0.1f,rng);
    } else if (s.kind == Kind::Pervane && choice == 3) {
        bool left;
        if (!blocked) left = rng.next()%2 == 0;
        else if (s.blockedLeft) left = true;
        else if (s.blockedRight) left = false;
        else left = rng.next()%2 == 0;
        append(s,left ? 7 : 8,0,20);
    } else {
        const bool left = !blocked ? rng.next()%2 == 0 : s.blockedLeft;
        const float step = s.kind == Kind::Soba ? 0.25f : 1;
        turn_events(s,left ? 9 : 10,left ? -36.0f : 36.0f,step);
    }
    append(s,13,0,1,true);
}
// 431770/434820/4038a0/425390/42b8b0: distinct damage evasions.
void evade(State& s, Random& rng) {
    append(s,12,0,1,true);
    const bool yayli = s.kind == Kind::Yayli;
    if (s.kind == Kind::Bisiklet) {
        if (s.blocked && !s.blockedForward) {
            (void)rng.next(); zigzag(s,999,3,3,true,rng);
        } else if (s.blocked) {
            switch (rng.next()%4) {
            case 0: pair_curve(s,999,3,rng); break;
            case 1: asymmetric_curve(s,999,3,rng); break;
            case 2: weave(s,999,2,rng); break;
            case 3: random_curve(s,2,rng); random_curve(s,2,rng); break;
            }
        } else {
            switch (rng.next()%5) {
            case 0: zigzag(s,999,999,3,true,rng); break;
            case 1: pair_curve(s,999,3,rng); break;
            case 2: asymmetric_curve(s,999,3,rng); break;
            case 3: weave(s,999,999,rng); break;
            case 4: { const int dir = 2+int(rng.next()%2); random_curve(s,dir,rng); random_curve(s,dir,rng); break; }
            }
        }
    } else if (!s.blocked || s.blockedForward) {
        const auto choice = rng.next()%(!s.blocked && s.alternateEvade ? 5U : s.blocked && yayli ? 2U : 4U);
        const int rate = yayli ? 2 : 3;
        const int direction = s.blocked ? 2 : 999;
        if (s.blocked && yayli) {
            if (!choice) weave(s,999,2,rng);
            else { random_curve(s,2,rng); move(s,false,10); random_curve(s,2,rng); }
        } else {
            switch (choice) {
            case 0: pair_curve(s,999,rate,rng); break;
            case 1: asymmetric_curve(s,999,rate,rng); break;
            case 2: weave(s,999,direction,rng); break;
            case 3: {
                const int dir = s.blocked ? 2 : 2+int(rng.next()%2);
                random_curve(s,dir,rng);
                if (yayli) move(s,dir == 3,5);
                random_curve(s,dir,rng); break;
            }
            case 4: {
                const bool left = rng.next()%2 == 0;
                const float step = s.kind == Kind::Pervane ? 0.625f : s.kind == Kind::Soba ? 0.25f : 1;
                turn_events(s,left ? 9 : 10,left ? -36.0f : 36.0f,step); break;
            }
            }
        }
    } else if (s.kind != Kind::Pervane || s.blockedBack) {
        if (rng.next()%2 == 0) weave(s,999,3,rng);
        else {
            random_curve(s,3,rng);
            if (yayli) move(s,true,10);
            random_curve(s,3,rng);
        }
    } else if (s.alternateEvade) {
        const bool left = s.blockedLeft;
        turn_events(s,left ? 9 : 10,left ? -36.0f : 36.0f,0.625f);
    }
    append(s,13,0,1,true);
}
float prediction(float a, float magnitude) {
    constexpr float small = 0.027027027681469917f;
    constexpr float medium = 0.054054055362939835f;
    constexpr float large = 0.10810811072587967f;
    if (a >= 170 || a <= -170) return 0;
    if (a > 135 && a < 170) return -magnitude*0.0054054055362939835f*5;
    if (a > 110 && a < 135) return -magnitude*medium;
    if (a > 60 && a <= 110) return -magnitude*large;
    if (a > 30 && a <= 60) return -magnitude*medium;
    if (a > 10 && a <= 30) return -magnitude*small;
    if (a > -30 && a <= -10) return magnitude*small;
    if (a > -60 && a <= -30) return magnitude*large;
    if (a > -110 && a <= -60) return magnitude*large;
    if (a > -135 && a <= -110) return magnitude*medium;
    if (a > -170 && a <= -135) return magnitude*small;
    return 0;
}
void engage(State& s, const gameplay::EntityState& e, const gameplay::EntityState& player,
            const Input& in, Random& rng) {
    float degrees = bearing(in,e.position,player.position);
    if (s.kind == Kind::Utu || s.kind == Kind::Yayli || s.kind == Kind::Bisiklet) {
        const auto v = in.playerVelocity;
        const float magnitude = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
        degrees += prediction(angle(v,subtract(player.position,e.position)),magnitude);
    }
    const float window = s.kind == Kind::Yayli ? 3 : s.kind == Kind::Pervane ? 2 : 1;
    const float clamp = s.kind == Kind::Bisiklet ? 8 : s.kind == Kind::Pervane ? 5 : s.kind == Kind::Soba ? 4 : 16;
    const float step = s.kind == Kind::Pervane ? 5 : s.kind == Kind::Soba ? 2 : 8;
    if (degrees > window || degrees < -window) {
        degrees = std::copysign(std::min(std::abs(degrees),clamp),degrees);
        if (s.blocked && !s.blockedBack) turn_only(s,degrees,step);
        else {
            std::uint8_t type;
            float signedTurn;
            if (s.kind == Kind::Utu) {
                // Utu advances while clear, reverses when rear-obstructed.
                type = s.blocked ? 6 : 5;
                signedTurn = s.blocked ? degrees : -degrees;
            } else if (s.kind == Kind::Yayli && !s.blocked) {
                const bool choice = rng.next()%2 != 0;
                type = choice ? 6 : 5;
                signedTurn = choice ? degrees : -degrees;
            } else { type = 6; signedTurn = degrees; }
            turn_events(s,type,signedTurn,step);
        }
        return;
    }
    append(s,14,0,1,true);
    if (s.kind == Kind::Bisiklet) {
        for (int i = 0; i < 10; ++i) { append(s,11); move(s,true,6); }
    } else if (s.kind == Kind::Yayli) {
        append(s,0,0,24);
        for (int i = 0; i < 5; ++i) { append(s,11); append(s,0,0,i == 4 ? 3 : 2); }
    } else if (s.kind == Kind::Pervane) {
        float rotation;
        if (degrees > 0) rotation = s.distanceBand == 2 ? 1 : s.distanceBand == 1 ? 2 : 4;
        else rotation = s.distanceBand == 2 ? -1 : s.distanceBand == 1 ? -2 : -4;
        move(s,false,34);
        for (int i = 0; i < 5; ++i) {
            append(s,11); turn_only(s,rotation,5); turn_only(s,rotation,5); move(s,true,4);
        }
        append(s,0,0,11);
    } else {
        append(s,0,0,s.kind == Kind::Soba ? 7 : 10);
        append(s,11);
        append(s,0,0,s.kind == Kind::Soba ? 32 : 29);
    }
    append(s,15,0,1,true);
    if (s.evadeAfterAttack && s.distanceSquared < 500000) {
        if (s.kind == Kind::Yayli) { if (rng.next()%3 == 0) react(s,true,rng); }
        else if (s.kind == Kind::Bisiklet) {
            append(s,12,0,1,true); zigzag(s,999,999,3,false,rng); append(s,13,0,1,true);
        } else react(s,false,rng);
    }
}
void refresh(State& s, const gameplay::EntityState& e, const gameplay::EntityState& player, const Input& in) {
    const auto elapsed = s.lastTime == 0 ? 0U : in.now-s.lastTime;
    s.lastTime = in.now;
    s.unseenTime += elapsed;
    if (s.kind != Kind::Pervane) { s.elapsed20 += elapsed; s.elapsed24 += elapsed; }
    s.distanceSquared = square_distance(e.position,player.position);
    if (s.kind == Kind::Bisiklet || s.kind == Kind::Pervane)
        s.distanceBand = s.distanceSquared > 500000 ? 2 : s.distanceSquared > 100000 ? 1 : 0;
    s.blocked = in.forceScalar > 5.011000156402588f;
    if (s.blocked) {
        if (in.forceScalar > 0) {
            const double right = clamped_acos(dot(in.rightAxis,in.collisionNormal));
            if (right < 0.7853981852531433f) s.collisionDirection = 1;
            else if (right > 2.356194496154785f) s.collisionDirection = 0;
            else {
                const double forward = clamped_acos(dot(in.forwardAxis,in.collisionNormal));
                if (forward < 0.7853981852531433f) s.collisionDirection = 3;
                else if (forward > 2.356194496154785f) s.collisionDirection = 2;
            }
        }
        s.blockedForward = s.collisionDirection == 2;
        s.blockedBack = s.collisionDirection == 3;
        s.blockedLeft = s.collisionDirection == 0;
        s.blockedRight = s.collisionDirection == 1;
    }
    if (s.detectStuck) {
        const float moved = square_distance(e.position,s.lastPosition);
        s.lastPosition = e.position;
        if (moved < 5) ++s.stationaryTicks;
        else s.stationaryTicks = 0;
        s.stuck = s.moveTicks > 8 && s.stationaryTicks > 8 && moved < 5;
        const auto oldFree = s.freeTicks;
        if (!s.stuck) { ++s.freeTicks; s.repeatedStuck = false; }
        else {
            s.freeTicks = 0;
            if (oldFree > 19) { s.stuckTicks = 0; s.repeatedStuck = false; }
            else { ++s.stuckTicks; s.repeatedStuck = s.stuckTicks > 5; }
        }
    }
}
void consume(State& s, gameplay::EntityState& e, std::vector<Operation>& out) {
    std::uint8_t last = 0;
    while (s.queuedEvents) {
        auto& run = s.events[s.firstEvent];
        const auto type = run.type;
        const float parameter = run.parameter;
        const bool immediate = run.immediate;
        if (!--run.repeats) ++s.firstEvent;
        --s.queuedEvents;
        last = type;
        const auto emitMove = [&](gameplay::Move m) { out.push_back({Operation::Type::Move,m,0}); };
        const auto emitTurn = [&](float value) { out.push_back({Operation::Type::Turn,gameplay::Move::Stop,value}); };
        switch (type) {
        case 1: emitTurn(parameter); break;
        case 2: emitTurn(-parameter); break;
        case 3: emitMove(gameplay::Move::Forward); break;
        case 4: emitMove(gameplay::Move::Back); break;
        case 5: emitMove(gameplay::Move::Forward); emitTurn(parameter); break;
        case 6: emitMove(gameplay::Move::Back); emitTurn(parameter); break;
        case 7: emitMove(gameplay::Move::Left); break;
        case 8: emitMove(gameplay::Move::Right); break;
        case 9: emitMove(gameplay::Move::Left); emitTurn(parameter); break;
        case 10: emitMove(gameplay::Move::Right); emitTurn(parameter); break;
        case 11: out.push_back({Operation::Type::Attack,gameplay::Move::Stop,0}); break;
        case 12: e.movementModeGuard = true; break;
        case 13: e.movementModeGuard = false; break;
        case 14: e.attackGuard = true; out.push_back({Operation::Type::Action,gameplay::Move::Stop,0,4}); break;
        case 15: e.attackGuard = false; break;
        default: break; // original type0 is a real one-tick wait
        }
        if (type >= 3 && type <= 10 && !e.attackGuard)
            out.push_back({Operation::Type::Action,gameplay::Move::Stop,0,type == 4 || type == 6 ? 3 : 2});
        if (!immediate) break;
    }
    if (s.detectStuck) {
        if (last < 3 || last > 10) { s.moveTicks = 0; s.stationaryTicks = 0; }
        else {
            ++s.moveTicks;
            s.escapeDirection = last == 3 || last == 5 ? 2 : last == 4 || last == 6 ? 3 : last == 7 || last == 9 ? 0 : 1;
        }
    }
}
bool orient_target(State& s, const gameplay::EntityState& e, const Input& in) {
    const float a = bearing(in,e.position,in.targetPosition);
    if (std::abs(a) <= 15) return true;
    clear(s);
    const float limit = s.kind == Kind::Pervane ? 10 : s.kind == Kind::Soba ? 4 : 16;
    const float step = s.kind == Kind::Pervane ? 5 : s.kind == Kind::Soba ? 2 : 8;
    const float turn = std::abs(a) >= 16 ? limit : std::abs(a);
    if (a > 1) turn_events(s,s.blockedForward ? 5 : 6,turn,step);
    else if (a < -1) turn_events(s,s.blockedForward ? 5 : 6,-turn,step);
    return false;
}
void search(State& s, Random& rng) {
    const int side = int(rng.next()%2);
    const float step = s.kind == Kind::Pervane ? 5 : s.kind == Kind::Soba ? 2 : 8;
    turn_only(s,side == 0 ? 360.0f : -360.0f,step);
    move(s,false,50); curved(s,2,side,3,0.5f,rng);
    move(s,false,100); curved(s,2,side,3,0.5f,rng); move(s,false,50);
}
} // namespace
void tick(State& s, gameplay::EntityState& entity, gameplay::EntityState& player,
          const Input& in, const std::vector<gameplay::Obb>& blocking,
          const std::vector<gameplay::EntityState>& entities, std::size_t entityIndex,
          std::size_t playerIndex, Random& rng, std::vector<Operation>& operations) {
    operations.clear();
    if (!s.enabled) return;
    refresh(s,entity,player,in);
    const auto hit = gameplay::raycast(entity.position,subtract(player.position,entity.position),blocking,entities,entityIndex,true);
    const bool visible = hit.kind == gameplay::CollisionHit::Kind::Entity && hit.index == playerIndex;
    if (!entity.attackGuard) {
        if (s.detectStuck && s.stuck && !s.repeatedStuck) {
            clear(s); escape(s,rng); s.stationaryTicks = 0;
        } else if (entity.reactToHit && s.reactHit) {
            if (s.kind != Kind::Bisiklet) clear(s);
            if (s.kind != Kind::Yayli && s.kind != Kind::Bisiklet) react(s,true,rng);
            else if (rng.next()%10 < 3) {
                if (s.kind == Kind::Bisiklet) clear(s);
                react(s,true,rng);
            }
            entity.reactToHit = false;
            s.lastHealth = entity.health;
        } else if (!entity.movementModeGuard && s.queuedEvents < (s.kind == Kind::Pervane ? 30U : 50U)) {
            switch (s.mode) {
            case 10:
                if (visible) { s.unseenTime = 0; s.mode = 50; }
                break;
            case 30:
                if (visible) { s.unseenTime = 0; s.mode = 50; }
                else if (s.unseenTime > 45000) { clear(s); s.mode = 10; }
                else {
                    s.hasTarget = square_distance(entity.position,in.targetPosition) > in.targetRadiusSquared;
                    if (!s.hasTarget || orient_target(s,entity,in)) search(s,rng);
                }
                break;
            case 40:
                evade(s,rng); s.lastHealth = entity.health; s.mode = 50;
                break;
            case 50:
                if (!s.blocked && s.evadeDamage) {
                    if (entity.health < s.lastHealth) s.mode = 40;
                    s.lastHealth = entity.health;
                }
                if (visible) { s.unseenTime = 0; clear(s); engage(s,entity,player,in,rng); }
                else if (s.unseenTime > 15000) { clear(s); s.mode = 30; }
                else wander(s,rng);
                break;
            case 60:
                if (s.kind == Kind::Yayli) { move(s,true,50); append(s,0,0,20); }
                else if (s.kind == Kind::Pervane) { react(s,true,rng); append(s,0,0,50); }
                break;
            default: break;
            }
        }
    }
    consume(s,entity,operations);
}
} // namespace yami::enemy_ai
