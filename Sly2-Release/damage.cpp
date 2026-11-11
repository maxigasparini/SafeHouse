// The actors' damage controller (orig ACTOR +0xDF0, module 0x1A0B18..0x1A245C): damage.h.

#include "aipursuit.h"
#include "objmethod.h"
#include "booty.h"
#include "damage.h"
#include "sound.h"
#include "activate.h"
#include "gang.h"
#include "guard.h"
#include "interact.h"
#include "shd.h"

#include "actmesh.h"
#include "actor.h"
#include "alo.h"
#include "asega.h"
#include "brx.h"
#include "clock.h"
#include "cls.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "smrun.h"
#include "savedata.h"
#include "sly.h"
#include "so.h"
#include "ainoise.h"
#include "bsp.h"
#include "emitter.h"
#include "lox.h"
#include "physworld.h"
#include "xfphys.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "task.h"
#include "vecmat.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "update.h"
#include "cm.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <set>
#include <string>
#include <vector>

void AccelAloPos(ALO* p, const glm::vec3& v);  // solve.cpp: orig 0x1FAC30
glm::vec3 PosActorProbe0(ACTOR* pa);  // jump.cpp (the actor's +0x218 point)
bool FJumpPathEntry(OXA* poxaOwn, SOE* psoe);  // jumptgt.cpp (0x20F948)
const glm::mat3* g_pmatDamageSender = nullptr;  // (damage.h's harness hook: the sender's world matrix)

namespace
{
bool FSafeDebug()
{
    static const bool enabled = std::getenv("SLY2N_SAFEDEBUG") != nullptr;
    return enabled;
}

void TraceSafePlace(const char* event, ACTOR* pa, const SAFEGROUND::ENTRY& e)
{
    if (!FSafeDebug()) return;
    const glm::vec3 pos = PosSafeGround(pa, e);
    std::fprintf(stderr, "SAFEPLACE event=%s t=%.6f hero=%u count=%d owner=%u kind=%d floor=%d parameter=%.6f "
                         "point=(%.6f,%.6f,%.6f)\n", event, g_clock.t, pa->palo->oid, pa->safe.c,
                 e.ploOwner ? e.ploOwner->oid : 0, static_cast<int>(e.jtl), e.psoe ? e.psoe->tag : -1,
                 e.pos.x, pos.x, pos.y, pos.z);
}

void WarnOnce(const std::string& what)
{
    static std::set<std::string> s_set;
    if (s_set.insert(what).second) std::fprintf(stderr, "damage: %s not ported\n", what.c_str());
}

float F32(uint32_t u)
{
    float g;
    std::memcpy(&g, &u, 4);
    return g;
}

DAMAGE& Dmg(ACTOR* pa) { return pa->dmg; }

// the reaction n's clip (+0xA0 + 4 n: the actor's parts by their offset)
ASEG* PasegReaction(ACTOR* pa, int n)
{
    const uint32_t ib = 0xDF0 + 0xA0 + 4 * static_cast<uint32_t>(n);
    for (const auto& [ibPart, plo] : pa->palo->apart)
        if (ibPart == ib) return dynamic_cast<ASEG*>(plo);
    return nullptr;
}

// the sender's world matrix (vt slot 31 0x1F3FA0: its object record +0x40)
glm::mat3 MatSender(LO* plo)
{
    if (g_pmatDamageSender) return *g_pmatDamageSender;
    ALO* palo = plo && FIsLoDerivedFrom(plo, 0x30) ? static_cast<ALO*>(plo) : nullptr;
    if (!palo) return glm::mat3(1.0f);
    if (XF* pxf = PxfObj(palo)) return glm::mat3(pxf->matWorld);
    return glm::mat3(MatWorldOfAlo(palo));
}

// orig: 0x00155228 (the counter's change scaled by the game's suck: 0x154C90 on 0x2DE578's history between its
// limits, D_002BDC00 +0x20 / +0x24)
int NScaleDamage(int n)
{
    const float g = GSuck();  // 0x154C90 (task.cpp); D_002BDC00 +0x20 / +0x24: 1 / 0.5
    return static_cast<int>(((1.0f - g) * 1.0f + g * 0.5f) * static_cast<float>(n));
}

// orig: 0x00144418 (vt slot 86 of the actors: the hit takes at least the capacity and is not a touch (0 / 1),
// 0x80000 or healing (0x100000): fatal)
bool FDamageFatal(ACTOR* pa, const CTMSG* pctm)
{
    if (-pctm->nState < Dmg(pa).cCapacity) return false;
    switch (pctm->grfFilter)
    {
    case 0: case 1: case 0x80000: case 0x100000: return false;
    default: return true;
    }
}

// orig: 0x001A2230 (the controller's immunity to a kind: the actor's authored +0x298 (EOPID 231 / 1350) to every
// kind; the kinds 0x2000 / 0x4000 by reaction (0x2A6830) and, without one, by the +0x4C0 kind (0x2A6870) / the
// jump's state; the others while reacting or flashing, or 0x2E1E00 & 1 or 0x20F0B0(g_game): TODO(world), natively not)
bool FDamageImmune(ACTOR* pa, int kind)
{
    if (pa->palo && pa->palo->U32Field(0, 0x298)) return true;
    if (kind == 0x2000 || kind == 0x4000)
    {
        switch (pa->nSubDF4)
        {
        case -1: break;
        case 3: case 5: case 10: case 11: return true;
        default: return false;
        }
        switch (pa->n4C8)
        {
        case -1:
        {
            const int j = pa->jump.state;
            return 8 <= j && j < 0x12 && kind == 0x4000;
        }
        case 0: case 3: case 4: case 7: case 8: return true;
        case 1: case 2: case 5: case 6: case 9: return false;
        default: return true;
        }
    }
    if (pa->nSubDF4 != -1) return true;
    return Dmg(pa).nFlash == 1;
}

// orig: 0x001A21A0 (Sly's vt slot 76: in the states 3 / 8 / 21 / 22 the kind 0x4000; state 7 by +0x4C0 (0x194640);
// then 0x140ED8)
bool FSlyImmune(ACTOR* pa, int kind)
{
    if (SLY* p = dynamic_cast<SLY*>(pa))
    {
        switch (p->state)
        {
        case 3: case 8: case 21: case 22:
            if (kind == 0x4000) return true;
            break;
        case 7:
            // orig: 0x00194640: a prize pose, or the two protected phases
            // of a carried-object interaction, rejects every damage kind.
            if (p->n4C8 == 12 ||
                (p->n4C8 == 8 && (p->nSub4CC == 9 || p->nSub4CC == 10))) return true;
            break;
        default: break;
        }
    }
    return FActorImmune(pa, kind);
}

// vt slot 76 by class (Sly 0x1A21A0, the actors' 0x140ED8)
bool FImmuneVt(ACTOR* pa, int kind)
{
    using PFN = int (*)(LO*, int);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeVt(pa->palo, 76))) return pfn(pa->palo, kind) != 0;
    return FActorImmune(pa, kind);
}

// the deferred actions (the PMFs at 0x2A6768..0x2A6790)
}  // namespace

// ---- the safe ground (+0x9E0) ----

// orig: 0x0019FEB0 (the floor entry and point stood on: moved to the front, a new one pushes the oldest out)
void RecordSafeGround(ACTOR* pa, SOE* psoe, const glm::vec3& posLocal)
{
    SAFEGROUND& sg = pa->safe;
    const bool changed = sg.c == 0 || sg.ae[0].jtl != JTL_Max || sg.ae[0].psoe != psoe;
    int i = 1;
    for (; i < sg.c; ++i)
    {
        SAFEGROUND::ENTRY& e = sg.ae[i];
        if (psoe == e.psoe && glm::length(posLocal - e.pos) < 50.0f)
        {
            if (i != 1)
            {
                const SAFEGROUND::ENTRY t = e;
                std::memmove(&sg.ae[2], &sg.ae[1], sizeof(SAFEGROUND::ENTRY) * static_cast<size_t>(i - 1));
                sg.ae[1] = t;
            }
            break;
        }
    }
    if (i >= sg.c)
    {
        if (sg.c < 32) ++sg.c;
        std::memmove(&sg.ae[1], &sg.ae[0], sizeof(SAFEGROUND::ENTRY) * static_cast<size_t>(sg.c - 1));
    }
    SAFEGROUND::ENTRY& e = sg.ae[0];
    e = SAFEGROUND::ENTRY{};
    if (psoe)
    {
        e.ploOwner = psoe->palo;
        e.psoe = psoe;
        psoe->f51 = true;  // (its +0x10 bit 51)
    }
    e.pos = posLocal;
    e.t = g_clock.t;
    if (changed) TraceSafePlace("floor", pa, e);
}

// orig: 0x0019FD30 (same two most recent places policy as 0x19FEB0; deduplicate descriptions within 50 units)
void RecordSafeTarget(ACTOR* pa, LO* plo, JTL jtl, float g)
{
    if (!plo || jtl == JTL_Max) return;
    SAFEGROUND& sg = pa->safe;
    const bool changed = sg.c == 0 || sg.ae[0].ploOwner != plo || sg.ae[0].jtl != jtl;
    int i = 1;
    for (; i < sg.c; ++i)
    {
        const SAFEGROUND::ENTRY& e = sg.ae[i];
        if (e.ploOwner == plo && e.jtl == jtl && std::fabs(g - e.pos.x) < 50.0f)
        {
            if (i != 1)
            {
                const SAFEGROUND::ENTRY saved = e;
                std::memmove(&sg.ae[2], &sg.ae[1], sizeof(SAFEGROUND::ENTRY) * static_cast<size_t>(i - 1));
                sg.ae[1] = saved;
            }
            break;
        }
    }
    if (i >= sg.c)
    {
        if (sg.c < 32) ++sg.c;
        std::memmove(&sg.ae[1], &sg.ae[0], sizeof(SAFEGROUND::ENTRY) * static_cast<size_t>(sg.c - 1));
    }
    SAFEGROUND::ENTRY& e = sg.ae[0];
    e = {};
    e.ploOwner = plo;
    e.jtl = jtl;
    e.pos.x = g;
    e.t = g_clock.t;
    plo->grf |= 1ull << 59;
    if (changed) TraceSafePlace("target", pa, e);
}

// orig: 0x001942F0 (the safe description of rails, spires and side-stepping surfaces; otherwise the floor)
void UpdateSafeInteract(ACTOR* pa)
{
    const INTERACT& in = pa->inter;
    LO* plo = in.ploTarget;
    float g = 0.0f;
    switch (pa->n4C8)
    {
    case 3:
        if (!plo || plo->U32Field(0, 0x19C, 0)) return;
        g = in.s20;
        break;
    case 4:
        if (!plo || plo->U32Field(0, 0xEC, 0)) return;
        break;
    case 0: case 13:
        if (!plo) return;
        if (FIsLoDerivedFrom(plo, 0xC))
        {
            if (plo->U32Field(0, 0x19C, 0)) return;
            g = in.s20;
        }
        else if (FIsLoDerivedFrom(plo, 0x26))
        {
            if (plo->U32Field(0, 0xEC, 0)) return;
        }
        else
        {
            const bool wall = FIsLoDerivedFrom(plo, 0x22);
            if (wall && plo->U32Field(0, 0xEC, 0)) return;
            const uint32_t u = plo->U32Field(0, wall ? 0x10C : 0x184, 0);
            float radius;
            std::memcpy(&radius, &u, sizeof(radius));
            const glm::vec3 d = PosLoWorld(pa->palo) - PosLoWorld(plo);
            g = std::atan2(d.y, d.x) * radius;
        }
        break;
    default:
    {
        XP* xp = pa->pxpFloor;
        if (!xp || xp->normal.z <= 0.7f || pa->ploWater) return;
        ALO* floor = xp->axpd[1].palo;
        if (floor && (Sort(floor).grf148 & 0x20000000ull)) return;
        RecordSafeGround(pa, xp->axpd[1].psoe, xp->axpd[1].posLocal);
        return;
    }
    }
    RecordSafeTarget(pa, plo, in.jtl, g);
}

// orig: 0x0019EA60
void UpdateSafePipe(ACTOR* pa)
{
    LO* pipe = pa->ploF5C;
    if (pipe && !pipe->U32Field(0, 0x19C, 0)) RecordSafeTarget(pa, pipe, JTL_Pipe, pa->sCrvF50);
}

// orig: 0x001A0048 / 0x001A00D0. Only histories are pruned: the active return-jump entry remains a value copy.
void RemoveSafeGroundReferences(LO* plo, SOE* psoe)
{
    ACTOR* heroes[] = {PslyGlobal(), PbentleyGlobal(), PmurrayGlobal()};
    for (ACTOR* pa : heroes)
    {
        if (!pa) continue;
        SAFEGROUND& sg = pa->safe;
        int dst = 0;
        for (int src = 0; src < sg.c; ++src)
        {
            const SAFEGROUND::ENTRY& e = sg.ae[src];
            if ((plo && e.ploOwner == plo) || (psoe && e.psoe == psoe)) continue;
            if (dst != src) sg.ae[dst] = e;
            ++dst;
        }
        if (FSafeDebug() && sg.c != dst)
            std::fprintf(stderr, "SAFEPLACE event=invalidate t=%.6f hero=%u owner=%u floor=%d before=%d after=%d\n",
                         g_clock.t, pa->palo->oid, plo ? plo->oid : psoe && psoe->palo ? psoe->palo->oid : 0,
                         psoe ? psoe->tag : -1, sg.c, dst);
        sg.c = dst;
    }
    if (plo) plo->grf &= ~(1ull << 59);
    if (psoe) psoe->f51 = false;
}

// orig: 0x0019FC30 (the Sly states that retain a floor or an interaction description)
void UpdateSafeGround(ACTOR* pa, int state)
{
    switch (state)
    {
    case 0: case 1: case 5: case 6: case 9: case 10: case 11: case 12:
    case 15: case 16: case 17: case 20: case 23: case 24:
    {
        XP* pxp = pa->pxpFloor;
        if (!pxp || !(0.7f < pxp->normal.z) || pa->ploWater) break;
        ALO* paloFloor = pxp->axpd[1].palo;
        if (paloFloor && (Sort(paloFloor).grf148 & 0x20000000ull)) break;
        RecordSafeGround(pa, pxp->axpd[1].psoe, pxp->axpd[1].posLocal);
        break;
    }
    case 3: case 22:
        if (SLY* sly = dynamic_cast<SLY*>(pa); sly && sly->ploOther && !sly->ploOther->U32Field(0, 0x10C, 0))
            RecordSafeTarget(pa, sly->ploOther, JTL_Point, 0.0f);
        break;
    case 7: UpdateSafeInteract(pa); break;
    case 8: UpdateSafePipe(pa); break;
    default: break;
    }
}

namespace
{
// orig: 0x0019F3F8 (g 0: the entry's point in the world, less the actor's +0x240: where its record goes)
glm::vec3 PosSafeEntry(ACTOR* pa, const SAFEGROUND::ENTRY& e, float dt = 0.0f)
{
    XF* pxf = nullptr;
    glm::vec3 local;
    if (e.jtl != JTL_Max)
    {
        const glm::vec3 world = PosJtDesc(e.ploOwner, e.jtl, pa, e.pos.x, nullptr);
        if (FIsLoDerivedFrom(e.ploOwner, 0x30)) pxf = PxfObj(static_cast<ALO*>(e.ploOwner));
        else if (FIsLoDerivedFrom(e.ploOwner, 3)) pxf = static_cast<LOX*>(e.ploOwner)->pxf;
        local = PosConvertXfs(nullptr, pxf, world);
    }
    else
    {
        pxf = e.psoe ? e.psoe->pxf : nullptr;
        local = e.pos;
    }
    glm::vec3 pos;
    if (pxf && dt > 0.0f)
    {
        glm::mat4 predicted(1.0f);
        PredictXfChain(pxf, nullptr, dt, &predicted, nullptr);  // 0x2309E8 / 0x230A98
        pos = glm::vec3(predicted * glm::vec4(local, 1.0f));
    }
    else pos = PosConvertXfs(pxf, nullptr, local);
    if (e.jtl == JTL_Max) pos -= glm::vec3(pa->vecBase);
    return pos;
}

// orig: 0x0019F7D0 (the entry's cost: 2000 without a flat floor under it or with no room above, 1500 at an edge (one of
// eight points 40 around without floor), and near another player up to 400 - its distance)
float GCostSafeEntry(ACTOR* pa, const SAFEGROUND::ENTRY& e, const std::vector<ALO*>& apaloPlayers)
{
    float g = 0.0f;
    glm::vec3 pos = PosSafeEntry(pa, e);
    if (e.psoe)
    {
        // The entries within 500 (0x20FC60), using the original trajectory filter 0x20F948.
        std::vector<SOE*> apsoe;
        OXA* poxaOwn = Poxa(pa->palo);
        for (OXA* poxa : g_physw.apoxaActive)
        {
            if (poxa == poxaOwn) continue;
            if (glm::length(poxa->posCenter - pos) > 500.0f + poxa->sRadius) continue;
            for (SOE* psoe : poxa->apsoe)
            {
                if (!FJumpPathEntry(poxaOwn, psoe)) continue;
                if (apsoe.size() < 0x100) apsoe.push_back(psoe);
            }
        }
        LSG lsg;
        auto hit = [&](const glm::vec3& a, const glm::vec3& b) { return !apsoe.empty() && PsoeHitTestLineObjects(0, a, b, apsoe, &lsg); };
        if (!hit(pos + glm::vec3(0, 0, 125.0f), pos - glm::vec3(0, 0, 175.0f)) || lsg.t0 == 0.0f || lsg.plane0.z < 0.7f)
            g = std::max(g, 2000.0f);
        else pos = lsg.pos0;
        if (hit(pos + glm::vec3(0, 0, 180.0f), pos + glm::vec3(0, 0, 5.0f))) g = std::max(g, 2000.0f);
        static const glm::vec2 s_aofs[8] = {{40.0f, 0.0f}, {28.3f, 28.3f}, {0.0f, 40.0f}, {-28.3f, 28.3f}, {-40.0f, 0.0f},
                                            {-28.3f, -28.3f}, {0.0f, -40.0f}, {28.3f, -28.3f}};  // (0x2BF950)
        for (const glm::vec2& o : s_aofs)
        {
            const glm::vec3 v = pos + glm::vec3(o, 0.0f);
            if (!hit(v + glm::vec3(0, 0, 50.0f), v - glm::vec3(0, 0, 30.0f)))
            {
                g = std::max(g, 1500.0f);
                break;
            }
        }
    }
    for (ALO* palo : apaloPlayers)
    {
        if (palo == pa->palo) continue;
        ACTOR* pq = PactorFromLo(palo);
        const glm::vec3 posQ = pq ? PosActorProbe0(pq) : PosAloCenter(palo);
        const float d = glm::length(posQ - pos);
        g = std::max(g, std::clamp(400.0f - d, 0.0f, 1000.0f));  // (0x2BF9D0 / 0x2BF9E0)
    }
    return g;
}

// orig: 0x0019FAF8 (the cheapest entry; none: the actor's own place)
SAFEGROUND::ENTRY EntryBestSafe(ACTOR* pa)
{
    std::vector<ALO*> apaloPlayers;  // (0x15E9C8(g_game, 0x505, 0x33, 0, 16): the players, at most 16)
    if (g_psw)
        for (CID cid = 0; cid < CID_Max; ++cid)
            if (FCidDerivedFrom(cid, 0x33))
                for (LO* plo : g_psw->mpcidaplo[cid])
                    if (plo && plo->fInWorld && apaloPlayers.size() < 16) apaloPlayers.push_back(static_cast<ALO*>(plo));
    SAFEGROUND::ENTRY best{};
    float gBest = FLT_MAX;
    for (int i = 0; i < pa->safe.c; ++i)
    {
        const float g = GCostSafeEntry(pa, pa->safe.ae[i], apaloPlayers);
        if (g < gBest)
        {
            gBest = g;
            best = pa->safe.ae[i];
            if (g == 0.0f) return best;
        }
    }
    if (gBest == FLT_MAX)
    {
        best = SAFEGROUND::ENTRY{};
        best.pos = PosAloCenter(pa->palo) + glm::vec3(pa->vecBase);
    }
    return best;
}

// orig: 0x00196928 (the time a jump with a vertical speed within [vzMin, vzMax] and a horizontal one within the limits
// needs to cover d under the gravity: the longest of the quadratics' last roots)
float TJumpTo(const glm::vec3& d, const glm::vec3& vGravity, float vzMax, float vzMin, float sMax, float sMin)
{
    const float a = vGravity.z * 0.5f;
    float t = 0.0f;
    float ax[2];
    int c = cmm::CSolveQuadratic(-a, -vzMax, d.z, ax);
    if (c > 0) t = std::max(ax[c - 1], t);
    c = cmm::CSolveQuadratic(a, -vzMin, d.z, ax);
    if (c > 0) t = std::max(ax[c - 1], t);
    const float dxy = std::sqrt(d.x * d.x + d.y * d.y);
    if (float g = (sMax * dxy - d.z) / -a; 0.0f < g) t = std::max(std::sqrt(g), t);
    if (float g = (sMin * dxy - d.z) / a; 0.0f < g) t = std::max(std::sqrt(g), t);
    return t;
}
}  // namespace

glm::vec3 PosSafeGround(ACTOR* pa, const SAFEGROUND::ENTRY& e, float dt) { return PosSafeEntry(pa, e, dt); }
float TJumpBallistic(const glm::vec3& d, const glm::vec3& vGravity, float vzMax, float vzMin, float sMax, float sMin)
{
    return TJumpTo(d, vGravity, vzMax, vzMin, sMax, sMin);
}

// orig: 0x0019F590 (with no entry: the best safe ground; the current player taken there at once when a jump would
// take longer than 1.25 s (the heading the camera's, the camera cut, the jump's kind 0xB), else it jumps back (the
// jump's kind 0xA toward +0x490))
// The jump toward e (0x19F590 with an entry: always the jump, e.g. SCPT 0x2A's scripted leaps; only the search of
// the best safe ground (no entry) may take the current player there at once).
void RecoverActorTo(ACTOR* pa, const SAFEGROUND::ENTRY& e)
{
    TraceSafePlace("jump", pa, e);
    pa->entryJump = e;  // (+0x490)
    SetJumpKind(&pa->jump, 0xA, 0, 0);
}
void RecoverActor(ACTOR* pa)
{
    const SAFEGROUND::ENTRY e = EntryBestSafe(pa);  // 0x19FAF8 (+0x9E0)
    TraceSafePlace("choose", pa, e);
    if (g_psw && pa->palo == PloPoCurrent(g_psw))
    {
        const glm::vec3 r = PosSafeEntry(pa, e);
        const SOST* ps = Psost(pa->palo);
        XF* pxfPos = PxfPos(pa->palo);
        const glm::vec3 posNow = pxfPos ? glm::vec3(pxfPos->matWorld[3]) : PosAloCenter(pa->palo);
        const float t = TJumpTo(r - posNow, ps ? ps->dvGravity : glm::vec3(0.0f, 0.0f, -1750.0f), 1000.0f, -1000.0f, 1.0f, -1.0f);
        if (F32(0x3FA00000) < t)
        {
            TraceSafePlace("teleport", pa, e);
            const int c = pa->safe.c;
            SetStepPos(pa, r - glm::vec3(pa->vecBase));  // vt slot 29 (Sly 0x143CF8: 0x2041C0, +0x9E4 = 0; kept)
            pa->safe.c = c;
            SetStepPhysHeadingToward(pa->palo, g_cm.mat[0]);  // 0x2046B0 (the CM's +0x60 row)
            SetStepRotation(pa, pa->matHeading);                // vt slot 30 (0x2041F8: +0x1B0)
            CutCm(&g_cm);                                        // 0x14D8C0
            SetJumpKind(&pa->jump, 0xB, 0, 0);
            if (e.jtl != JTL_Max && FSafeTargetReattach(e.ploOwner))
            {
                CheckLoRootNow(g_psw, e.ploOwner);  // 0x148300
                ChooseJumpTarget(&pa->jump, e.ploOwner);  // 0x19B110
            }
            StartFadeLo(g_ploFadeRecover);                      // 0x15E1D8(g_game +0x3E60)
            return;
        }
    }
    RecoverActorTo(pa, e);
}

namespace
{
// orig: 0x001A1F68 (counter left: get up (0), else 8)
void NextGetUp(ACTOR* pa) { SetDamageState(pa, *Dmg(pa).pcount > 0 ? 0 : 8); }
// orig: 0x001A16A8 (0x19F590(actor, 0): the +0x9E0 sub-object)
void NextRecover(ACTOR* pa) { RecoverActor(pa); }
// orig: 0x001A1FA8 (counter left: the sender's bit 33 recovers, else the jump's kind 11; else 8)
void NextStunned(ACTOR* pa)
{
    DAMAGE& d = Dmg(pa);
    if (*d.pcount > 0)
    {
        ALO* ps = d.req.ploSender && FIsLoDerivedFrom(d.req.ploSender, 0x30) ? static_cast<ALO*>(d.req.ploSender) : nullptr;
        if (ps && (Sort(ps).grf148 & 0x200000000ull)) NextRecover(pa);
        else SetJumpKind(&pa->jump, 11, 0, 0);
    }
    else SetDamageState(pa, 8);
}
// orig: 0x001A2080 (the PMF 0x2A6790: the same as 0x1A1FA8)
void NextAfterSpecial(ACTOR* pa) { NextStunned(pa); }
// orig: 0x001A2028 (the burn's end: the plain sound 0x6A started without a source (0x1FBB90 / 0x1FB2F8(0)), the
// reaction 6 (0x1A0B40), then the next action 0x2A6790 (0x1A15B0))
void NextSpecial(ACTOR* pa)
{
    if (CSnd* ps = PsndNewPlain(0x6A)) ps->SetSource(nullptr);
    SetDamageState(pa, 6);
    Dmg(pa).pfnNext = NextAfterSpecial;
}

// orig: 0x001A16C8 (the velocity of the hit by kind (vt slot 35), then the reaction 7)
void ApplyDamageVelocity(ACTOR* pa)
{
    DAMAGE& d = Dmg(pa);
    glm::vec3 v = d.req.vec;
    LO* ps = d.req.ploSender;
    switch (d.req.grfFilter)
    {
    case 0x200: case 0x8000:
        if (ps && FIsLoDerivedFrom(ps, 0x33))
        {
            v *= 1.25f;  // (D_002BFACC 0.25 + 1, then 0x11E9F8 at D_002BFAD4 1200, z at least D_002BFAD0 700)
            const float l = glm::length(v);
            if (1200.0f < l) v *= 1200.0f / l;
            v.z = std::max(v.z, 700.0f);
        }
        break;
    case 0x40:
        if (ps && FIsLoDerivedFrom(ps, 0x36) && FIsLoDerivedFrom(ps, 0x30))
        {
            // hit by Murray: knocked to his left or right by which side of the point 150 behind him (his x row) the
            // actor is on (its local y: the decomp's prot3w reads word 1), in his frame (0x1A6FC0): (1000, +-900, 950) turned
            // into the world (audit 2026-11 D3)
            const glm::mat3 m(MatWorldOfAlo(static_cast<ALO*>(ps)));
            const glm::vec3 posS = PosAloCenter(pa->palo), posT = PosAloCenter(static_cast<ALO*>(ps));
            const glm::vec3 delta = posS - (posT + m[0] * -150.0f);
            const glm::vec3 local = glm::transpose(m) * delta;
            v = m * (local.y > 0.0f ? glm::vec3(1000.0f, 900.0f, 950.0f) : glm::vec3(1000.0f, -900.0f, 950.0f));  // 0x2BFAE0 / 0x2BFAF0
        }
        break;
    case 0x100:
        if (ps && FIsLoDerivedFrom(ps, 0x4F))
        {
            glm::vec3 dir;
            if (FIsLoDerivedFrom(ps, 0x51)) dir = MatSender(ps)[0];  // (glm column 0 = the original's first row)
            else
            {
                const glm::vec3 a = FIsLoDerivedFrom(pa->palo, 0x30) ? PosAloCenter(pa->palo) : glm::vec3(0.0f);
                const glm::vec3 b = FIsLoDerivedFrom(ps, 0x30) ? PosAloCenter(static_cast<ALO*>(ps)) : glm::vec3(0.0f);
                dir = a - b;
            }
            dir.z = 0.0f;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            dir = l < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : dir * (1.0f / l);  // (0x2A2DA0)
            v = dir * 600.0f + glm::vec3(0.0f, 0.0f, 1.0f) * 800.0f;            // (0x2BFB00 / 0x2BFB04)
        }
        break;
    default: break;
    }
    SetAloVelocityTo(pa->palo, v);  // vt slot 35 (0x1F4488)
    SetDamageState(pa, 7);
}
}  // namespace

// orig: 0x00140ED8
bool FActorImmune(ACTOR* pa, int kind)
{
    if (pa->nF20 == 1) return true;
    if (pa->jump.state == 0x15) return true;
    // D_003D4A54: the safe house (cid 0x46) last initialized in this world (0x1D7238, its InitLo; 0 at the world
    // reset 0x20E068); 0x1C40C0: it is the current player (0x1C4840): while the player is in the safe house no actor
    // takes damage (the heists start there)
    if (g_psw)
        if (const std::vector<LO*>& a = g_psw->mpcidaplo[0x46]; !a.empty() && a.back() == PloPoCurrent(g_psw)) return true;
    return FDamageImmune(pa, kind);
}

// orig: 0x001A0B18
void InitDamage(ACTOR* pa)
{
    DAMAGE& d = Dmg(pa);
    pa->nSubDF4 = -1;
    d.nFlash = -1;
    d.dtFlash = 2.0f;
    d.cCapacity = 0x28;
    // the counter: Sly's is the save's health (0x3D4AB0: the save +0x50); TODO(actors) the others' (Bentley's /
    // Murray's post-loads)
    static int s_nCounterNone = 0x28;
    d.pcount = dynamic_cast<SLY*>(pa) ? reinterpret_cast<int*>(g_gamesave.ab + 0x50) : &s_nCounterNone;
}

int* DmgCounterOf(ACTOR* pa) { return Dmg(pa).pcount; }
int DmgCapacityOf(ACTOR* pa) { return Dmg(pa).cCapacity; }
void SetDmgCounter(ACTOR* pa, int* pn) { Dmg(pa).pcount = pn; }
void SetDmgCapacity(ACTOR* pa, int c) { Dmg(pa).cCapacity = c; }

namespace
{
// orig: 0x001A0B40's Sly parts: his records +0x19B8 / +0x19BC (the part table 0x2BE990's tags 241 / 250) are chain
// segments (mode 16); the reactions set their chain's gravity (ALOCHAIN +0x20, via the record's +0xA8 segment and its
// +0x60 chain): the hanging parts fall (-750), float up in the water (500) or stop (0)
void SetSlyPartGravity(ALO* palo, uint32_t ib, const glm::vec3& v)
{
    // (the record's chain segment by the chains themselves: the original writes through +0xA8 whatever the record's
    // mode, also while an animation holds the segment (R10))
    for (auto& [o, pxf] : palo->apartxf)
        if (o == ib && pxf)
            for (ALOCHAIN& c : palo->achain)
                for (const ALOCHAINE& e : c.ae)
                    if (e.pxf == pxf) c.vec20 = v;
}
}  // namespace

// orig: 0x001A0B40
void SetDamageState(ACTOR* pa, int n)
{
    if (std::getenv("SLY2N_DMGSTATE")) {  // (debug: damage state with caller address)
#ifdef _MSC_VER
        void* caller = _ReturnAddress();
#else
        void* caller = __builtin_return_address(0);
#endif
        std::printf("damage state %d: %d -> %d t %.3f (from %p)\n", pa->palo ? pa->palo->oid : -1, pa->nSubDF4, n, g_clock.t, caller);
    }
    DAMAGE& d = Dmg(pa);
    ALO* palo = pa->palo;
    const int old = pa->nSubDF4;
    if (n == old) return;
    const bool fSly = FIsLoDerivedFrom(palo, 0x35);
    // the old one's end
    if (old == 3 || old == 10)
    {
        if (fSly) SetSlyPartGravity(palo, 0x19B8, glm::vec3(0.0f));
    }
    else if (old == -1)
    {
        if (fSly)
        {
            // (Sly: his +0x19B8 / +0x19BC parts' chains' gravity to (0, 0, -750), D_002BFA00)
            SetSlyPartGravity(palo, 0x19B8, glm::vec3(0.0f, 0.0f, -750.0f));
            SetSlyPartGravity(palo, 0x19BC, glm::vec3(0.0f, 0.0f, -750.0f));
        }
        if (SLY* psly = dynamic_cast<SLY*>(pa)) SetSlyMode(psly, -1);  // vt slot 91 (Sly 0x18D548; TODO(actors) the others')
        pa->vecVel = glm::vec3(0.0f);
        if (PODATA* ppd = PpodataFromLo(palo)) ppd->n15C = 0;
        d.tFlashFade = FLT_MAX;
    }
    ASEG* paseg = nullptr;
    int grfapl = 1;
    switch (n)
    {
    case -1:
        // (Sly: the +0x19B8 / +0x19BC parts' chains' gravity zeroed)
        if (fSly)
        {
            SetSlyPartGravity(palo, 0x19B8, glm::vec3(0.0f));
            SetSlyPartGravity(palo, 0x19BC, glm::vec3(0.0f));
        }
        if (PODATA* ppd = PpodataFromLo(palo)) ppd->n15C = 1;
        d.tFlashFade = g_clock.t;
        break;
    case 0: case 6: case 7:
        paseg = PasegReaction(pa, n);
        break;
    case 8:
    {
        // a launch that kills: thrown backward (the velocity against the heading's forward row +0x1B0 below -250)
        // keeps 8 and turns toward where it flies by clamp(-1 + 0.004 |v.xy|, 0, 1) (D_002BFA20 / D_002BBB60), else 9
        const glm::vec3 v = VelAloPos(palo);  // vt slot 33
        if (glm::dot(v, pa->matHeading[0]) > -250.0f) n = 9;
        else
        {
            const float blend = std::clamp(-1.0f + 0.004f * std::hypot(v.x, v.y), 0.0f, 1.0f);
            const float rad = cmm::RadNormalize(std::atan2(v.y, v.x) + 3.1415927f - pa->radHeading);  // 0x11CF60
            SetStepPhysHeading(palo, pa->radHeading + rad * blend);  // 0x204660
            TurnActorNow(pa);                                        // 0x204CA8
        }
        paseg = PasegReaction(pa, n);
        grfapl = 0x21;
        break;
    }
    case 9:
        paseg = PasegReaction(pa, n);
        grfapl = 0x21;
        break;
    case 10:
        // facing the camera (0x2046B0 toward the CM's position), then as 3
        SetStepPhysHeadingToward(palo, g_cm.pos - PosAloCenter(palo));
        grfapl = 0x21;
        [[fallthrough]];
    case 3:
        // in the water (Sly: his +0x19B8 part's chain's gravity to (0, 0, 500), D_002BFA10)
        paseg = PasegReaction(pa, n);
        if (fSly) SetSlyPartGravity(palo, 0x19B8, glm::vec3(0.0f, 0.0f, 500.0f));
        break;
    case 5: case 11:
        // facing the camera (0x2046B0 toward the CM's position)
        SetStepPhysHeadingToward(palo, g_cm.pos - PosAloCenter(palo));
        paseg = PasegReaction(pa, n);
        if (n == 11) grfapl = 0x21;
        break;
    case 1: case 2:
        // orig: 0x1A0F98..0x1A0FCC: retain the actual body heading (0x204740),
        // then copy the new goal into the controller's +0x18 before playing the reaction.
        SetStepPhysHeading(palo, RadStepHeading(pa));
        if (PODATA* ppd = PpodataFromLo(palo); ppd && ppd->ppadctl)
            ppd->ppadctl->radJoy = pa->radHeading;
        grfapl = 0;
        paseg = PasegReaction(pa, n);
        break;
    case 4: case 12:
        // no turning (vt slot 36 zero), the body's heading kept (0x204740) and given to the controller's +0x18
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        SetStepPhysHeading(palo, RadStepHeading(pa));
        if (PODATA* ppd = PpodataFromLo(palo); ppd && ppd->ppadctl)
            ppd->ppadctl->radJoy = pa->radHeading;
        paseg = PasegReaction(pa, n);
        if (n == 12) grfapl = 0x21;
        break;
    default:
        WarnOnce("0x1A0B40's reaction " + std::to_string(n));
        paseg = PasegReaction(pa, n);
        break;
    }
    MarkAloDirty(palo);  // 0x1F4BF8
    pa->nSubDF4 = n;
    d.tStart = g_clock.t;
    if (n == -1) return;
    if (!paseg) paseg = PasegReaction(pa, n < 8 ? 0 : 8);
    if (pa->pasega) RetractAsegaPp(&pa->pasega);  // 0x130658
    if (pa->psma)                                  // 0x1F15B8 / 0x1F1448(0, ...)
    {
        RetractSma(pa->psma);
        pa->psma = nullptr;
    }
    if (paseg) ApplyAseg(paseg, palo, 0.0f, 1.0f, grfapl, &pa->pasega);  // 0x1305B8
    if (std::getenv("SLY2N_HITDEBUG"))
        std::printf("damage state: t %.3f %d -> %d clip %d (%.3f s)\n", g_clock.t, palo->oid, n, paseg ? paseg->oid : -1, paseg ? PasegdOf(paseg)->tMax : 0.0f);
    UpdateActorStepMesh(pa);  // vt slot 90 (the reaction's body mesh: 0x1A23F8)
}

// orig: 0x001A19A0
bool FAcceptDamage(ACTOR* pa, CTMSG* pctm)
{
    DAMAGE& d = Dmg(pa);
    if (std::getenv("SLY2N_HITDEBUG"))  // (the AI code's debug: who damaged whom)
        std::printf("damage: t %.3f %d from %d (filter 0x%X state %d) vec %.1f %.1f %.1f count %d\n", g_clock.t, pa->palo ? pa->palo->oid : -1, pctm->ploSender ? pctm->ploSender->oid : -1, pctm->grfFilter, pctm->nState,
                    pctm->vec.x, pctm->vec.y, pctm->vec.z, d.pcount ? *d.pcount : -1);
    if (std::getenv("SLY2N_HITDEBUG") && pctm->ploSender)  // (where the sender is, and its instance)
    {
        const glm::vec3 posS = PosLoWorld(pctm->ploSender);
        const glm::vec3 posR = pa->palo ? PosLoWorld(pa->palo) : glm::vec3(0.0f);
        std::printf("  sender instance %d cid 0x%X at %.1f %.1f %.1f; receiver at %.1f %.1f %.1f\n", pctm->ploSender->oidInstance,
                    pctm->ploSender->Cid(), posS.x, posS.y, posS.z, posR.x, posR.y, posR.z);
    }
    CTMSG req = *pctm;
    int delta = 0;
    if (FDamageFatal(pa, pctm))  // vt slot 86 (0x1A2358)
    {
        if (static_cast<unsigned>(pa->nSubDF4 - 8) < 5) return false;
        delta = -0x8000;
    }
    else
    {
        if (FImmuneVt(pa, int(req.grfFilter))) return false;  // vt slot 76
        if (pa->n4C8 == 1)
        {
            // the hiding spot it is in (+0x4D4) takes the hit: its slot 75 with the same message (the barrel's
            // 0x183E08: a hit of contact byte 8 knocks it over, its noise 706)
            LO* pt = pa->inter.ploTarget;
            if (!pt || !FIsLoDerivedFrom(pt, 0x30)) return false;
            using PFN = bool (*)(ALO*, CTMSG*);
            PFN pfn = reinterpret_cast<PFN>(PvNativeVt(pt, 75));
            return pfn ? pfn(static_cast<ALO*>(pt), pctm) : pctm->grfAccepted != 0;
        }
        // the change: none with the actor's authored +0x29C (EOPID 232: e.g. Sly, the AI partner of Boardroom
        // Brawl's battle 10:1591, option 358) or while flashing, else scaled
        if (pa->palo && pa->palo->U32Field(0, 0x29C)) delta = 0;
        else if (d.nFlash != 1) delta = NScaleDamage(req.nState);
        if (req.grfFilter == 0x100000)
        {
            // healing
            if (0 < *d.pcount + delta)
            {
                if (d.nFlash != 1)
                {
                    d.nFlash = 0;
                    d.tFlash = d.tFlashFade = g_clock.t;
                }
                *d.pcount += delta;
                // the healing clip (+0x108, ApplyAsegCur from 0 at speed 1, grf 1, kept at +0x10C), then the hit's
                // noise as every accepted change (0x1A19A0's tail); natively both were skipped (audit 2026-11 D2)
                for (const auto& [ib, ppart] : pa->palo->apart)
                    if (ib == 0xEF8 && ppart && FIsLoDerivedFrom(ppart, 0x6D))
                        ApplyAsegCur(static_cast<ASEG*>(ppart), pa->palo, 0.0f, 1.0f, 1, &d.pasegaHeal);
                if (NOISETMPL* pt = PtmplNoiseFixed(6)) PnoiseNew(pt, PosAloCenter(pa->palo), pa->palo);
                return true;
            }
            req.grfFilter = 0x200;
            req.vec = glm::vec3(0.0f);
            req.ploSender = nullptr;
        }
        // a guard's (cid 0x38) hit while it is shrunk (+0xFBC modes 8 / 13: the size destabilizer) halves it
        // (toward zero)
        if (req.ploSender && FIsLoDerivedFrom(req.ploSender, 0x38))
            if (const GUARD* pg = PguardFromLo(req.ploSender); pg && (pg->nMode == 8 || pg->nMode == 13)) delta /= 2;
    }
    // the actor's state machine (vt slot 88 (3): Sly's state 4)
    NotifyActorJumpKind(pa, 3);
    // (+0x11A8's +0x158 +8, or the counter out: 0x143380 the interaction cleared)
    // (the carried booty +0x11A8: fragile (its record +8) or the counter out: dropped, 0x143380 (booty.cpp))
    if (PlootCarriedBy(pa) && (FActorBootyFragile(pa) || *d.pcount + delta <= 0)) DropActorBooty(pa);
    BroadcastTaskEvent(g_psw, 15, req.ploSender);  // 0x1619A0(0x2DEB30, 15, sender)
    d.req = req;
    d.req.nState = delta;
    d.pfnNext = nullptr;  // (0x2A6768)
    *d.pcount = std::max(*d.pcount + delta, 0);
    if (0 < *d.pcount)
    {
        d.nFlash = 1;
        d.tFlash = g_clock.t;
    }
    switch (req.grfFilter)
    {
    case 0x100: case 0x200:
        ApplyDamageVelocity(pa);
        if (400.0f < VelAloPos(pa->palo).z)
        {
            // launched upward: the class's cry (0x1FBB90 + SetSource: Sly 0x69, Murray 0x84, Bentley 0x83, else -1);
            // natively only a warning, reached in 90 of 348 random-play starts (audit 2026-11 D1)
            const int cid = pa->palo->Cid();
            const int id = cid == 0x35 ? 0x69 : cid == 0x36 ? 0x84 : cid == 0x37 ? 0x83 : -1;
            if (CSnd* ps = PsndNewPlain(id)) ps->SetSource(pa->palo);
        }
        d.pfnNext = NextGetUp;  // (0x2A6770)
        break;
    case 0x40:
        ApplyDamageVelocity(pa);
        d.pfnNext = NextGetUp;
        break;
    case 0x800: case 0x1000:
        SetAloVelocityTo(pa->palo, glm::vec3(0.0f));       // vt slot 35 (0x2A2DD0)
        SetAloAngVelocityTo(pa->palo, glm::vec3(0.0f));    // vt slot 36
        SetDamageState(pa, req.grfFilter == 0x800 ? 1 : 2);
        d.pfnNext = NextStunned;  // (0x2A6780)
        break;
    case 0x400:
        SetAloVelocityTo(pa->palo, glm::vec3(0.0f, 0.0f, -1000.0f));  // (0x2BFB10)
        SetDamageState(pa, 0 < *d.pcount ? 4 : 12);
        break;
    case 0x2000:
        if (0 < *d.pcount)
        {
            SetDamageState(pa, 3);
            d.pfnNext = NextRecover;  // (0x2A6788)
        }
        else SetDamageState(pa, 10);
        break;
    case 0x4000:
        if (0 < *d.pcount)
        {
            SetDamageState(pa, 5);
            d.pfnNext = NextRecover;
        }
        else SetDamageState(pa, 11);
        break;
    case 0x8000:
        SetDamageState(pa, 7);
        d.pfnNext = NextSpecial;  // (0x2A6778)
        break;
    default: break;
    }
    // the hit's noise (0x156698(g_game +0x4038: the fixed template 712, the actor's position, the actor))
    if (NOISETMPL* pt = PtmplNoiseFixed(6)) PnoiseNew(pt, PosAloCenter(pa->palo), pa->palo);
    return true;
}

// orig: 0x001A1358
void UpdateDamage(ACTOR* pa)
{
    DAMAGE& d = Dmg(pa);
    if (d.nFlash != -1)
    {
        float u = 0.0f;
        bool fDone = false;
        if (pa->nSubDF4 == -1)
        {
            const float dt = g_clock.t - d.tFlashFade;
            if (d.dtFlash < dt)
            {
                d.nFlash = -1;
                d.gFlash = 0.0f;
                MarkAloDirty(pa->palo);
                fDone = true;
            }
            else u = dt / d.dtFlash;
        }
        if (!fDone)
        {
            const float w = std::sin((g_clock.t - d.tFlash) * 10.0f);  // (0x244E10)
            const float a = std::clamp(1.0f + u * (0.0f + u * -0.75f), 0.0f, 1.0f);   // (0x2BFAA0)
            d.gFlash = a * std::clamp(0.7f + w * (0.3f + w * 0.0f), 0.0f, 1.0f);       // (0x2BFAB0)
        }
    }
    if (pa->nSubDF4 == 7)
    {
        const glm::vec3 v = VelAloPos(pa->palo);  // vt slot 33
        const float rad = std::atan2(v.y, v.x) + F32(0x40490FDB);
        static const cmm::SMP s_smp{3.5f, 0.0f, 0.5f};  // (0x2BFAC0)
        SetStepPhysHeading(pa->palo, cmm::RadSmooth(pa->radHeading, rad, g_clock.dt, &s_smp, nullptr));
        if (0.25f < g_clock.t - d.tStart && pa->pxpFloor) EndDamageReaction(pa);
        else if (FActorStuckOnContacts(pa, d.tStart, nullptr)) EndDamageReaction(pa);  // 0x19A098 (held off the floor by contacts)
    }
}

// orig: 0x001A15D0
void EndDamageReaction(ACTOR* pa)
{
    DAMAGE& d = Dmg(pa);
    void (*pfn)(ACTOR*) = d.pfnNext;
    d.pfnNext = nullptr;
    if (pfn) pfn(pa);
    else if (static_cast<unsigned>(pa->nSubDF4 - 8) < 5)
    {
        // vt slot 93 (the actors' 0x1A2388): the eyes shut (0x1F80D0(1): the class 0x8E children's 0x1EE3B0 over the
        // hierarchy: TODO, cosmetic), then the task event 0x10 (0x140EB0): the job restarts at its checkpoint
        // (task.cpp's JobOnEvent, once no transition is busy)
        SetAloEyesClosed(pa->palo, 1.0f);  // 0x1F80D0
        BroadcastTaskEvent(g_psw, 0x10, pa->palo);
    }
    else
    {
        ALO* ps = d.req.ploSender && FIsLoDerivedFrom(d.req.ploSender, 0x30) ? static_cast<ALO*>(d.req.ploSender) : nullptr;
        if (ps && (Sort(ps).grf148 & 0x200000000ull)) NextRecover(pa);
        else SetJumpKind(&pa->jump, 11, 0, 0);  // 0x195A00
    }
}

// orig: 0x001A12F0
void UpdateDamageTransitions(ACTOR* pa)
{
    int n;
    do
    {
        n = pa->nSubDF4;
        if (!pa->pasega || FAsegaRetracted(pa->pasega)) EndDamageReaction(pa);  // 0x131968
    } while (pa->nSubDF4 != -1 && pa->nSubDF4 != n);
}

// orig: 0x001A23C0
int NDamageStatus(const ACTOR* pa)
{
    if (pa->nSubDF4 != -1) return *pa->dmg.pcount <= 0 ? 2 : 1;
    return pa->dmg.nFlash == 1 ? 1 : 0;
}

// orig: 0x001A2100
void AdjustDamageXp(ACTOR* pa, XP* pxp)
{
    switch (pa->nSubDF4)
    {
    case 1: case 2: case 3: case 5: case 8: case 9: case 10: case 11:
        pxp->mu = 1.0f;   // (0x2BFB20)
        pxp->elas = 0.3f; // (0x2BFB24)
        break;
    case 4: case 12:
        pxp->mu = 1.0f;
        pxp->elas = 0.0f;
        break;
    default: break;
    }
}

// orig: 0x001A1148
void UpdateDamageMotion(ACTOR* pa)
{
    ALO* palo = pa->palo;
    const SOST* ps = Psost(palo);
    const glm::vec3 gGravity = ps ? ps->dvGravity : glm::vec3(0.0f);
    static const CLQ s_clq{0.0f, 10.0f, 0.01f, 0.0f};  // (0x2BFA90)
    switch (pa->nSubDF4)
    {
    case 1: case 2:
        // the gravity cancelled (0x1FAC30), the velocity damped to rest (0x1F5E10)
        AccelAloPos(palo, -gGravity);
        AccelAloTowardPosition(palo, glm::vec3(0.0f), nullptr, glm::vec3(0.0f), &s_clq);
        break;
    case 5: case 11:
    {
        // the same, but the clip's vertical motion kept (+0x198: its root velocity's z, reversed)
        AccelAloPos(palo, -gGravity);
        glm::vec3 v(0.0f);
        if (pa->vecVel.z != 0.0f) v.z = VelAloPos(palo).z;
        AccelAloTowardPosition(palo, glm::vec3(0.0f), nullptr, v, &s_clq);
        if (pa->vecVel.z != 0.0f) AccelAloPos(palo, glm::vec3(0.0f, 0.0f, (-pa->vecVel.z - v.z) / g_clock.dt));
        break;
    }
    default: break;
    }
}

namespace
{
// orig: 0x00140F50 (the actors' vt slot 75: a contact accepted is a hit on +0xDF0)
bool FActorAcceptContact(ALO* palo, CTMSG* pctm)
{
    ACTOR* pa = PactorFromLo(palo);
    return pa && FAcceptDamage(pa, pctm);
}
NATIVEFN(0x140F50, FActorAcceptContact);
// orig: 0x00140F70 (the actors' vt slot 77: the controller's status)
int NActorDamageStatus(LO* plo)
{
    ACTOR* pa = PactorFromLo(plo);
    return pa ? NDamageStatus(pa) : 0;
}
NATIVEFN(0x140F70, NActorDamageStatus);
// orig: 0x001A21A0 (Sly's vt slot 76)
int FSlyImmuneVt(LO* plo, int kind)
{
    ACTOR* pa = PactorFromLo(plo);
    return pa && FSlyImmune(pa, kind) ? 1 : 0;
}
NATIVEFN(0x1A21A0, FSlyImmuneVt);
}  // namespace

// ---- the water (the actor's +0x108C) ----

glm::vec3 VelWaterAtPoint(ALO* pw, const glm::vec3& pos);  // water.cpp (0x228C18)
namespace
{
// water.cpp's 0x2297F8 (the ALO's slot 72)
void ApplyWaterToAlo(ALO* po, ALO* pw)
{
    using PFN = void (*)(ALO*, ALO*);
    if (PFN pfn = reinterpret_cast<PFN>(PvNativeFn(0x2297F8))) pfn(po, pw);
}
}  // namespace

LO* PloWaterField(ALO* pw, uint32_t ib);  // water.cpp (an object option of the water by its oid)
namespace
{
// orig: 0x001A2788 (the water's surface at the actor: the first point where the segment from its position + (0, 0,
// +0x109C) down to its position + (0, 0, +0x1098) (100 / -100 for every actor) enters one of the water's live entries,
// 0x14CDB0 with the filter 0x229D60)
bool FWaterSurfaceUnder(ACTOR* pa, ALO* pw, glm::vec3* ppos)
{
    const glm::vec3 p = PosAloCenter(pa->palo);
    const glm::vec3 a = p + glm::vec3(0.0f, 0.0f, 100.0f), b = p + glm::vec3(0.0f, 0.0f, -100.0f);
    bool f = false;
    float tBest = 0.0f;
    for (const SOE& e : pw->asoe)
    {
        if (e.f50) continue;
        LSG alsg[16];
        const int c = ClipSegmentEntry(&e, a, b, 16, alsg);
        for (int i = 0; i < c; ++i)
            if (!f || alsg[i].t0 < tBest)
            {
                f = true;
                tBest = alsg[i].t0;
                *ppos = alsg[i].pos0;
            }
    }
    return f;
}
}  // namespace

// orig: 0x001A2460 (the actor's update: its point (+0x10A0 above its center, 10 lower in the reactions 3 / 10) in the
// water +0x108C: falling (or standing without a reaction 7) it takes the water's contact (filter +0x188, state +0x184:
// 0x2000 / -10 -> the reaction 3); the splash points +0x1090 make the effect +0x174's bursts (slot 51 messages 4))
void UpdateActorWater(ACTOR* pa)
{
    ALO* pw = pa->ploWater && FIsLoDerivedFrom(pa->ploWater, 0x30) ? static_cast<ALO*>(pa->ploWater) : nullptr;
    if (!pw) return;
    glm::vec3 pos = PosAloCenter(pa->palo);
    pos.z += pa->sWaterHeight;
    if (pa->nSubDF4 == 3 || pa->nSubDF4 == 10) pos.z -= 10.0f;
    if (!FAloContainsPoint(pw, pos)) return;  // vt slot 73
    const int nState = static_cast<int>(pw->U32Field(0, 0x184, static_cast<uint32_t>(-10)));
    if (nState)
    {
        const glm::vec3 v = VelAloPos(pa->palo);  // vt slot 33
        if (v.z < 0.0f || (pa->jump.state == -1 && pa->nSubDF4 != 7))
        {
            CTMSG ctm;
            ctm.ploSender = pw;
            ctm.ploReceiver = pa->palo;
            ctm.grfFilter = pw->U32Field(0, 0x188, 0x2000);
            ctm.nState = nState;
            ctm.pos = PosAloCenter(pa->palo);
            ctm.vec = v;
            PaloAcceptContact(&ctm);  // 0x1F7598
        }
    }
    // the splash points: with the water's effect +0x174 and its surface above or below (0x1A2788), each point not
    // splashed in the last 0.05 s whose position and its position a frame on (0x2309E8: its record's motion) are on
    // either side of the surface: the effect's burst (vt slot 51, message 4) there at the surface's height
    LO* ploEffect = PloWaterField(pw, 0x174);
    if (!ploEffect) return;
    glm::vec3 posSurface;
    if (!FWaterSurfaceUnder(pa, pw, &posSurface)) return;
    for (auto& [plo, tLast] : pa->asplash)
    {
        if (g_clock.t - tLast < 0.05f) continue;
        const glm::vec3 posP = PosLoWorld(plo);
        // (vt slot 32's frame: an ALO's object record, a LOX's own record (0x231440: record x local))
        XF* pxf = FIsLoDerivedFrom(plo, 0x30) ? PxfObj(static_cast<ALO*>(plo))
                  : FIsLoDerivedFrom(plo, 3)  ? static_cast<LOX*>(plo)->pxf
                                              : nullptr;
        const glm::vec3 posNext = posP + (pxf ? VelXfPoint(pxf, nullptr, posP) : glm::vec3(0.0f)) * g_clock.dt;
        const bool fCross = posSurface.z <= posP.z ? posNext.z <= posSurface.z : posSurface.z <= posNext.z;
        if (!fCross) continue;
        BURSTPARAMS bp;
        bp.grf = 4;
        bp.pos = glm::vec3(posP.x, posP.y, posSurface.z);
        if (std::getenv("SLY2N_RIPHIT"))
            std::fprintf(stderr, "RIPHIT splash t %.3f point %d at %.0f %.0f %.0f effect %d\n", g_clock.t, plo->oid, bp.pos.x, bp.pos.y, bp.pos.z, ploEffect->oid);
        ExplodeLo(ploEffect, &bp);
        tLast = g_clock.t;
    }
}

namespace
{
// orig: 0x001A28B0 (the players' vt slot 72: in the water during a physics frame: rising in the jump's state 5 or the
// reaction 7 nothing, falling the ALO's; the reactions 3 / 10 the surface's hold (0x1A2788, 0x228C18): TODO; else the
// ALO's 0x2297F8)
void ApplyWaterToActor(ALO* po, ALO* pw)
{
    ACTOR* pa = PactorFromLo(po);
    if (!pa) { ApplyWaterToAlo(po, pw); return; }
    if (pa->jump.state == 5 || pa->nSubDF4 == 7)
    {
        if (VelAloPos(po).z < 0.0f) ApplyWaterToAlo(po, pw);
        return;
    }
    if (pa->nSubDF4 == 3 || pa->nSubDF4 == 10)
    {
        // held at the surface: the water's surface point under the actor (0x1A2788: the line +100 .. -100 through the
        // water's entries), the drag toward the flow (0x228C18) less the clip's motion (+0x190 in the heading's frame)
        // times +0x10A4 (-10), the height smoothed onto the surface (+0x10A0 above the center, SMPA +0x10A8 250 / 0 / 1
        // / 4500) against the gravity
        const glm::vec3 posC = PosAloCenter(po);
        std::vector<SOE*> apsoe;
        for (SOE& e : pw->asoe) apsoe.push_back(&e);
        LSG lsg;
        if (apsoe.empty() || !PsoeHitTestLineObjects(0, posC + glm::vec3(0, 0, 100.0f), posC + glm::vec3(0, 0, -100.0f), apsoe, &lsg))
            return;
        const glm::vec3 posS = lsg.pos0;
        const glm::vec3 vLocal = pa->matHeading * pa->vecVel;  // (the rows +0x1B0.. weighted by +0x190)
        const glm::vec3 vFlow = VelWaterAtPoint(pw, posS);
        const glm::vec3 v = VelAloPos(po);
        glm::vec3 a = (v + vLocal - vFlow) * -10.0f;  // (+0x10A4)
        static const cmm::SMPA s_smpa{250.0f, 0.0f, 1.0f, 4500.0f};  // (+0x10A8)
        float vzNext = 0.0f;
        cmm::GSmoothA(posC.z + pa->sWaterHeight, v.z - vFlow.z, posS.z, g_clock.dt, &s_smpa, &vzNext);  // 0x11D1C0
        const SOST* ps = Psost(po);
        const float g = (vzNext - v.z) / g_clock.dt - (ps ? ps->dvGravity.z : 0.0f);
        a.z = std::max(a.z, g);
        AccelAloPos(po, a);  // 0x1FAC30
        return;
    }
    ApplyWaterToAlo(po, pw);
}
NATIVEFN(0x1A28B0, ApplyWaterToActor);
}  // namespace

namespace
{
// orig: 0x00140F70 via 0x001FFD98 (EOPID 883, option 805 of the classes 0x32..0x4E: the getter is vt slot 77, the
// damage controller's status 0x1A23C0: 2 knocked out). World 17's boss respawn (s2945) starts SCPT 65479 unless 2.
struct INITSTATUS77GETTER
{
    INITSTATUS77GETTER()
    {
        RegisterObjectGetter(883, [](LO* plo) {
            CRef r;
            r.SetS32(NPlayerStatus77(plo));
            return r;
        });
    }
} s_initstatus77getter;
}
