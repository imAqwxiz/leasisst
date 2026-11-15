#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
    struct Preset { const char* name; int accuracy; int lookahead; };

    constexpr Preset PRESETS[] = {
        {"Top 1000", 35, 6},
        {"Top 500",  50, 8},
        {"Top 100",  70, 10},
        {"Top 10",   90, 14},
        {"Top 1",   100, 24},
    };

    // Physics constants in 60 Hz reference frames. Tune these first if a mode times badly.
    constexpr float G_CUBE       = 0.958f;
    constexpr float V_JUMP       = 11.18f;
    constexpr float V_YELLOW     = 11.18f;
    constexpr float V_PINK       = 7.2f;
    constexpr float V_RED        = 14.5f;
    constexpr float G_BALL       = 0.575f;
    constexpr float GAP_FALLBACK = 120.f;   // ball/spider travel if no block is found
    constexpr float BASE_SPEED   = 311.58f; // 1x speed, units/sec
    constexpr float SIM_DT       = 1.f / 240.f; // seconds per simulated step
    constexpr float SIM_DTF      = 0.25f;       // reference frames per simulated step

    enum class Kind { None, Cube, CubeAir, Ball, Spider, Wave };
    enum class OrbKind { Yellow, Pink, Red, Other };

    struct Box { float l, r, b, t; };
    struct Orb { Box box; OrbKind kind; };
    struct Entry { GameObject* obj; GameObjectType type; };

    struct Sim {
        Kind kind = Kind::None;
        Box player{};
        float speed = BASE_SPEED;
        float dir = 1.f;          // +1 normal gravity, -1 flipped
        float vy = 0.f;           // world velocity (units per ref frame)
        bool hasSurface = false;  // cube landing level known
        float landOff = 0.f;      // landing level relative to current center
        float gap = GAP_FALLBACK; // distance to opposite surface (ball/spider)
        int horizon = 0;
        bool curHold = false;
        bool newHold = false;
        float slope = 1.f;
        std::vector<Box> haz, solid;
        std::vector<Orb> orbs;
    };

    Box fromRect(CCRect const& r) { return {r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY()}; }
    bool overlap(Box const& a, Box const& b) { return a.l < b.r && a.r > b.l && a.b < b.t && a.t > b.b; }
    Box shifted(Box const& p, float dx, float dy) { return {p.l + dx, p.r + dx, p.b + dy, p.t + dy}; }

    bool hitsHazard(Sim const& s, Box const& p) {
        for (auto const& h : s.haz) if (overlap(p, h)) return true;
        return false;
    }

    // Cube on the ground (click = jump) or airborne near an orb (click = orb effect)
    bool simCube(Sim const& c, int k, bool air) {
        float wy = 0.f;
        float wv = air ? c.vy : 0.f;
        bool grounded = !air;
        bool clicked = false;
        for (int i = 0; i < c.horizon; ++i) {
            float dx = c.speed * SIM_DT * float(i + 1);
            if (i == k && !clicked) {
                clicked = true;
                if (!air) {
                    if (grounded) { wv = V_JUMP * c.dir; grounded = false; }
                } else {
                    Box p = shifted(c.player, dx, wy);
                    int idx = -1;
                    for (size_t j = 0; j < c.orbs.size(); ++j) {
                        if (overlap(p, c.orbs[j].box)) { idx = int(j); break; }
                    }
                    if (idx < 0) return false; // click wouldn't touch any orb
                    auto kind = c.orbs[idx].kind;
                    if (kind == OrbKind::Yellow)    wv = V_YELLOW * c.dir;
                    else if (kind == OrbKind::Pink) wv = V_PINK * c.dir;
                    else if (kind == OrbKind::Red)  wv = V_RED * c.dir;
                    // blue/green: only the timing window is checked
                    grounded = false;
                }
            }
            if (!grounded) {
                wv -= G_CUBE * c.dir * SIM_DTF;
                wy += wv * SIM_DTF;
                if (c.hasSurface && ((c.dir > 0.f && wy <= c.landOff) || (c.dir < 0.f && wy >= c.landOff))) {
                    wy = c.landOff; wv = 0.f; grounded = true;
                }
            }
            if (hitsHazard(c, shifted(c.player, dx, wy))) return false;
        }
        return true;
    }

    bool simBall(Sim const& s, int k) {
        float wy = 0.f, wv = 0.f;
        bool flipped = false;
        float target = s.dir * s.gap;
        for (int i = 0; i < s.horizon; ++i) {
            if (i == k) flipped = true;
            if (flipped) {
                float newDir = -s.dir;
                wv -= G_BALL * newDir * SIM_DTF;
                wy += wv * SIM_DTF;
                if ((s.dir > 0.f && wy >= target) || (s.dir < 0.f && wy <= target)) {
                    wy = target; wv = 0.f;
                }
            }
            if (hitsHazard(s, shifted(s.player, s.speed * SIM_DT * float(i + 1), wy))) return false;
        }
        return true;
    }

    bool simSpider(Sim const& s, int k) {
        for (int i = 0; i < s.horizon; ++i) {
            float wy = (i >= k) ? s.dir * s.gap : 0.f;
            if (hitsHazard(s, shifted(s.player, s.speed * SIM_DT * float(i + 1), wy))) return false;
        }
        return true;
    }

    bool simWave(Sim const& s, int k) {
        float wy = 0.f;
        for (int i = 0; i < s.horizon; ++i) {
            bool hold = (i >= k) ? s.newHold : s.curHold;
            wy += (hold ? 1.f : -1.f) * s.dir * s.slope * s.speed * SIM_DT;
            Box p = shifted(s.player, s.speed * SIM_DT * float(i + 1), wy);
            if (hitsHazard(s, p)) return false;
            for (auto const& b : s.solid) {
                if (!overlap(p, b)) continue;
                float ox = std::min(p.r, b.r) - std::max(p.l, b.l);
                float oy = std::min(p.t, b.t) - std::max(p.b, b.b);
                if (ox < oy) return false; // hit the front of a block
                float pc = (p.t + p.b) * 0.5f;
                float bc = (b.t + b.b) * 0.5f;
                float push = pc > bc ? oy : -oy; // slide along the surface
                wy += push;
                p = shifted(p, 0.f, push);
            }
        }
        return true;
    }

    // Returns the delay in sim steps (1 step = 1/240 s), or -1 for "don't interfere"
    int chooseDelay(Sim const& s, int K, float acc) {
        std::vector<int> safe;
        for (int k = 0; k <= K; ++k) {
            bool ok = false;
            if (s.kind == Kind::Cube)         ok = simCube(s, k, false);
            else if (s.kind == Kind::CubeAir) ok = simCube(s, k, true);
            else if (s.kind == Kind::Ball)    ok = simBall(s, k);
            else if (s.kind == Kind::Spider)  ok = simSpider(s, k);
            else if (s.kind == Kind::Wave)    ok = simWave(s, k);
            if (ok) safe.push_back(k);
        }
        if (safe.empty()) return -1;
        if (s.kind != Kind::CubeAir && int(safe.size()) == K + 1) return -1; // nothing in play

        float lo = float(safe.front());
        float hi = float(safe.back());
        float target = hi - (1.f - acc / 100.f) * (hi - lo);
        int best = safe.front();
        float bestDist = 1e9f;
        for (int k : safe) {
            float d = std::fabs(float(k) - target);
            if (d <= bestDist) { bestDist = d; best = k; }
        }
        return best;
    }
}

$execute {
    listenForSettingChanges<std::string>("preset", [](std::string name) {
        for (auto& p : PRESETS) {
            if (name == p.name) {
                Mod::get()->setSettingValue<int64_t>("accuracy", p.accuracy);
                Mod::get()->setSettingValue<int64_t>("lookahead", p.lookahead);
                break;
            }
        }
    });
}

class $modify(AssistGJBGL, GJBaseGameLayer) {
    struct Fields {
        std::vector<Entry> objs; // hazards, solids and orbs sorted by x
        bool built = false;
        bool pending = false;
        bool pendingWave = false;
        int stepsLeft = 0;
        bool releaseQueued = false;
        bool releaseNext = false;
        bool bypass = false;
        bool desiredHold = false;
        bool deliveredHold = false;
        float speed = 0.f;
        float lastX = 0.f;
        bool hasSurface = false;
        float surfaceY = 0.f;
        float surfaceDir = 1.f;
    };

    void buildObjects() {
        auto f = m_fields.self();
        f->built = true;
        f->objs.clear();
        if (!m_objects) return;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            auto t = obj->m_objectType;
            if (t == GameObjectType::Hazard || t == GameObjectType::Solid ||
                t == GameObjectType::YellowJumpRing || t == GameObjectType::PinkJumpRing ||
                t == GameObjectType::RedJumpRing || t == GameObjectType::GravityRing ||
                t == GameObjectType::GreenRing) {
                f->objs.push_back({obj, t});
            }
        }
        std::sort(f->objs.begin(), f->objs.end(), [](Entry const& a, Entry const& b) {
            return a.obj->getPositionX() < b.obj->getPositionX();
        });
    }

    bool assistOn() {
        return PlayLayer::get() && Mod::get()->getSettingValue<bool>("assist-on");
    }

    Kind detectKind() {
        auto p = m_player1;
        if (!p || p->m_isDead) return Kind::None;
        if (p->m_isShip || p->m_isBird || p->m_isRobot || p->m_isSwing) return Kind::None;
        if (p->m_isDart) return Kind::Wave;
        if (p->m_isSpider) return p->m_isOnGround ? Kind::Spider : Kind::None;
        if (p->m_isBall) return p->m_isOnGround ? Kind::Ball : Kind::None;
        return p->m_isOnGround ? Kind::Cube : Kind::CubeAir;
    }

    // false = nothing relevant nearby, don't interfere
    bool buildSim(Sim& s, Kind kind, int K) {
        auto f = m_fields.self();
        if (!f->built) this->buildObjects();
        auto p = m_player1;

        s.kind = kind;
        s.player = fromRect(p->getObjectRect());
        s.speed = f->speed > 1.f ? f->speed : BASE_SPEED;
        s.dir = p->m_isUpsideDown ? -1.f : 1.f;
        s.vy = static_cast<float>(p->m_yVelocity) * s.dir;
        s.horizon = K + (kind == Kind::Wave ? 30 : 108);
        s.slope = p->getScaleX() < 0.8f ? 2.f : 1.f; // mini wave is steeper
        s.curHold = f->deliveredHold;
        s.newHold = f->desiredHold;

        if (kind == Kind::Cube) {
            s.hasSurface = true;
            s.landOff = 0.f;
        } else if (kind == Kind::CubeAir && f->hasSurface && f->surfaceDir == s.dir) {
            float off = f->surfaceY - p->getPositionY();
            if ((s.dir > 0.f && off <= 0.f) || (s.dir < 0.f && off >= 0.f)) {
                s.hasSurface = true;
                s.landOff = off;
            }
        }

        float reach = s.speed * SIM_DT * float(s.horizon) + 60.f;
        bool needSolids = (kind == Kind::Wave || kind == Kind::Ball || kind == Kind::Spider);

        auto& v = f->objs;
        float x0 = s.player.l - 40.f;
        auto it = std::lower_bound(v.begin(), v.end(), x0, [](Entry const& e, float x) {
            return e.obj->getPositionX() < x;
        });
        for (; it != v.end(); ++it) {
            if (it->obj->getPositionX() > s.player.r + reach) break;
            Box b = fromRect(it->obj->getObjectRect());
            if (b.r < s.player.l - 5.f) continue;
            auto t = it->type;
            if (t == GameObjectType::Hazard) {
                if (s.haz.size() < 80) s.haz.push_back(b);
            } else if (t == GameObjectType::Solid) {
                if (needSolids && s.solid.size() < 300) s.solid.push_back(b);
            } else if (kind == Kind::CubeAir) {
                // shrink orbs a little so the predicted overlap is safely inside the real one
                Box ob{b.l + 3.f, b.r - 3.f, b.b + 3.f, b.t - 3.f};
                OrbKind ok = OrbKind::Other;
                if (t == GameObjectType::YellowJumpRing) ok = OrbKind::Yellow;
                else if (t == GameObjectType::PinkJumpRing) ok = OrbKind::Pink;
                else if (t == GameObjectType::RedJumpRing) ok = OrbKind::Red;
                s.orbs.push_back({ob, ok});
            }
        }

        // ball/spider: distance to the opposite surface from blocks above/below
        if (kind == Kind::Ball || kind == Kind::Spider) {
            float best = 1e9f;
            for (auto const& b : s.solid) {
                if (b.r <= s.player.l || b.l >= s.player.r) continue;
                float d = s.dir > 0.f ? b.b - s.player.t : s.player.b - b.t;
                if (d >= -1.f && d < best) best = d;
            }
            s.gap = best < 1e8f ? best : GAP_FALLBACK;
        }

        if (kind == Kind::CubeAir) return !s.orbs.empty();
        if (kind == Kind::Wave) return !(s.haz.empty() && s.solid.empty());
        return !s.haz.empty();
    }

    int planDelay(Kind kind) {
        float acc = static_cast<float>(Mod::get()->getSettingValue<int64_t>("accuracy"));
        int look = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead"));
        int K = look * 4; // 4 sim steps per 60 Hz frame
        if (kind == Kind::Wave) K = std::min(K, 40);

        Sim s;
        if (!this->buildSim(s, kind, K)) return 0;
        int d = chooseDelay(s, K, acc);
        return d < 0 ? 0 : d;
    }

    void deliver(bool down) {
        auto f = m_fields.self();
        f->deliveredHold = down;
        f->bypass = true;
        GJBaseGameLayer::handleButton(down, 1, true);
        f->bypass = false;
    }

    void firePending() {
        auto f = m_fields.self();
        f->pending = false;
        if (f->pendingWave) {
            f->pendingWave = false;
            if (f->desiredHold != f->deliveredHold) this->deliver(f->desiredHold);
            return;
        }
        this->deliver(true);
        if (f->releaseQueued) {
            f->releaseQueued = false;
            f->releaseNext = true;
        }
    }

    void handleWave(bool down) {
        auto f = m_fields.self();
        if (down == f->deliveredHold) {
            if (f->pending) { f->pending = false; f->pendingWave = false; }
            else this->deliver(down);
            return;
        }
        if (f->pending) return; // already waiting to deliver this direction
        int d = this->planDelay(Kind::Wave);
        if (d > 0) {
            f->pending = true;
            f->pendingWave = true;
            f->stepsLeft = d;
            return;
        }
        this->deliver(down);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.self();
        if (f->bypass || !isPlayer1 || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        f->desiredHold = down;
        Kind kind = this->assistOn() ? this->detectKind() : Kind::None;

        if (kind == Kind::Wave) {
            this->handleWave(down);
            return;
        }

        if (down) {
            if (f->pending) return;
            if (kind != Kind::None) {
                int d = this->planDelay(kind);
                if (d > 0) {
                    f->pending = true;
                    f->pendingWave = false;
                    f->stepsLeft = d;
                    f->releaseQueued = false;
                    return;
                }
            }
            this->deliver(true);
            return;
        }

        if (f->pending && !f->pendingWave) {
            f->releaseQueued = true;
            return;
        }
        this->deliver(false);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.self();

        if (m_player1) {
            float x = m_player1->getPositionX();
            if (m_player1->m_isDead || x < f->lastX - 1.f) {
                f->pending = false;
                f->pendingWave = false;
                f->releaseQueued = false;
                f->releaseNext = false;
                f->deliveredHold = false;
                f->desiredHold = false;
            }

            if (f->releaseNext) {
                f->releaseNext = false;
                this->deliver(false);
            }

            if (f->pending) {
                if (f->stepsLeft > 0) --f->stepsLeft;
                else this->firePending();
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (m_player1 && dt > 0.f) {
            float x = m_player1->getPositionX();
            float dx = x - f->lastX;
            if (dx > 0.f) f->speed = dx / dt;
            f->lastX = x;

            if (m_player1->m_isOnGround) {
                f->hasSurface = true;
                f->surfaceY = m_player1->getPositionY();
                f->surfaceDir = m_player1->m_isUpsideDown ? -1.f : 1.f;
            }
        }
    }
};
