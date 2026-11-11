// The interest objects (noises). See ainoise.h. Reference: Sly2-Reference/Sly2-Release/game/m_1564E8_p3.cpp, m_205E70_p3.cpp.

#include "ainoise.h"
#include "worldglobals.h"

#include <deque>
#include <new>
#include "activate.h"
#include "sw.h"

#include "aipursuit.h"
#include "bsp.h"
#include "clock.h"
#include "cls.h"
#include "detect.h"
#include "guard.h"
#include "objmethod.h"
#include "so.h"
#include "crv.h"
#include "step.h"
#include "aijobs.h"
#include "sw.h"
#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr float G_RADIUS_POLY[3] = {0.15f, 1.1f, -0.25f};  // D_002BDCF0 / F4 / F8
constexpr float T_RADIUS_MAX = 1.0f;                        // D_002BDCE0
constexpr float T_ALARM_REPEAT = 1.0f;                      // D_002C3DB8
constexpr float T_AWARE_WAIT = 2.0f;                        // D_002C3DB0

std::unordered_map<const LO*, std::unique_ptr<NOISETMPL>>& MpTmpl()
{
    static std::unordered_map<const LO*, std::unique_ptr<NOISETMPL>> s_map;
    WORLD_TABLE_LOCAL(s_map);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_map;
}
std::vector<std::unique_ptr<NOISE>>& AnoiseOwned()
{
    static std::vector<std::unique_ptr<NOISE>> s_a;
    WORLD_TABLE_LOCAL(s_a);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_a;
}
std::deque<NOISE*>& ApnoiseFree()  // (the pool: released noises, oldest first)
{
    static std::deque<NOISE*> s_a;
    WORLD_TABLE_LOCAL(s_a);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_a;
}
std::vector<NOISE*>& Anoise()
{
    static std::vector<NOISE*> s_a;
    WORLD_TABLE_LOCAL(s_a);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_a;
}
// Original listener +0xE0/+0xE4 retains up to eight noises. Native noise
// storage already survives removal until ResetNoises, including for AI jobs.
// Keep these pointers until original pruning/removal would release them.
std::unordered_map<LO*, std::vector<NOISE*>>& MpListenerNoises()
{
    static std::unordered_map<LO*, std::vector<NOISE*>> s_map;
    WORLD_TABLE_LOCAL(s_map);   // (PC: moved aside while a kept world loads, worldglobals.h)
    return s_map;
}
void UpdateNoiseListener(LO* plo);
void TraceNoiseListener(LO* plo, const char* action, const NOISE* pn, size_t count, int duplicates = 0)
{
    static const char* selected = std::getenv("SLY2N_NOISELISTENERTRACE");
    if (!selected || (std::strcmp(selected, "all") && std::atoi(selected) != plo->oid)) return;
    std::printf("noise listener: t %.9g oid %d clone %d %s noise %d template %d retained %zu duplicates %d\n",
                g_clock.t, plo->oid, plo->iclone, action, pn ? pn->nSerial : -1,
                pn && pn->ptmpl && pn->ptmpl->plo ? pn->ptmpl->plo->oid : -1, count, duplicates);
}
int s_nSerial = 0;  // SW +0x3D68
WORLD_VALUE(s_nSerial);   // (PC: moved aside while a kept world loads, worldglobals.h)

float FieldF(const LO* plo, uint32_t ib, float g)
{
    uint32_t u;
    std::memcpy(&u, &g, 4);
    u = plo->U32Field(0, ib, u);
    float r;
    std::memcpy(&r, &u, 4);
    return r;
}

// the world's fixed templates, by oid (0x1569B8 with D_002BDD00..)
void FixedOf(NOISETMPL* pt, OID oid)
{
    switch (oid)
    {
    case 0x2C2: break;
    case 0x2C3: pt->nType = 1; pt->tLife = -1.0f; break;
    case 0x2C4: pt->sRadius = 750.0f; break;
    case 0x2C5: pt->sRadius = 2500.0f; pt->tLife = 0.35f; break;
    case 0x2C7: pt->nKind = 3; pt->tLife = -1.0f; break;
    case 0x2C6: pt->sRadius = 4000.0f; pt->tLife = 0.5f; break;
    case 0x2C8: pt->sRadius = 1000.0f; break;
    default: break;
    }
}

}  // namespace
void ExpireNoises();
namespace
{
// the expiry (0x156728 for each object of the list)
struct INITNOISE
{
    INITNOISE()
    {
        RegisterSwPass(0x156728, [](SW*) { ExpireNoises(); });
        RegisterLoUpdate(0x156EB8, UpdateNoiseListener);
        // orig: 0x0010D840 (NOISETMPL option 214): vector and LO arguments,
        // then 0x00156698 creates and returns the noise. The owner may be null;
        // the object argument is used directly, without an OID lookup.
        RegisterObjectMethod(0x10D840, [](LO* plo, const std::vector<CRef>& a) {
            CRef result;
            if (a.size() < 2 || a[0].m_tagk != TAGK_Vector) return result;
            LO* owner = a[1].m_tagk == TAGK_Basic ? dynamic_cast<LO*>(a[1].m_pbasic) : nullptr;
            result.SetBasic(PnoiseNew(PtmplNoise(plo), a[0].GetVector(), owner));
            return result;
        });
        // NOISE option 959 is the investigation destination, separate from where the sound is heard.
        RegisterObjectGetter(1098, [](LO* plo) {
            CRef result;
            result.SetVector(static_cast<NOISE*>(plo)->pos30);
            return result;
        });
        RegisterObjectSetter(1098, [](LO* plo, const CRef& value) {
            static_cast<NOISE*>(plo)->pos30 = value.GetVector();
        });
    }
} s_initnoise;
}  // namespace

// orig: 0x00156728 for each object of the list
void ExpireNoises()
{
    // (debug: SLY2N_NOISECOUNT=1 prints the allocated / live / pooled noises every 10 s of game time)
    if (static const bool s_f = std::getenv("SLY2N_NOISECOUNT") != nullptr; s_f)
        if (static float s_t = 0.0f; g_clock.t >= s_t + 10.0f || g_clock.t < s_t)
        {
            s_t = g_clock.t;
            std::printf("noisecount t %.1f allocated %zu live %zu pooled %zu serial %d\n", g_clock.t, AnoiseOwned().size(), Anoise().size(),
                        ApnoiseFree().size(), s_nSerial);
        }
    for (NOISE* pn : std::vector<NOISE*>(Anoise()))
        if (pn->ptmpl && 0.0f <= pn->ptmpl->tLife && pn->t + pn->ptmpl->tLife < g_clock.t) RemoveNoise(pn);
}

NOISETMPL* PtmplNoise(LO* plo)
{
    if (!plo) return nullptr;
    auto& p = MpTmpl()[plo];
    if (p) return p.get();
    p = std::make_unique<NOISETMPL>();
    p->plo = plo;
    if (plo->oid >= 0x2C2 && plo->oid <= 0x2C8 && plo->aopt.empty()) FixedOf(p.get(), plo->oid);
    p->nType = static_cast<int>(plo->U32Field(0, 0x50, static_cast<uint32_t>(p->nType)));
    p->sRadius = FieldF(plo, 0x54, p->sRadius);
    p->tLife = FieldF(plo, 0x60, p->tLife);
    p->nKind = static_cast<int>(plo->U32Field(0, 0x64, static_cast<uint32_t>(p->nKind)));
    for (const OPT& o : plo->aopt)
        if (o.eopid == 1092 && !o.aval.empty()) p->rz = glm::vec2(o.aval[0].ag[0], o.aval[0].ag[1]);
    return p.get();
}

NOISETMPL* PtmplNoiseFixed(int i)
{
    static const OID s_aoid[7] = {0x2C2, 0x2C3, 0x2C4, 0x2C5, 0x2C6, 0x2C7, 0x2C8};
    if (i < 0 || i >= 7 || !g_psw) return nullptr;
    for (const auto& plo : g_psw->aploAll)
        if (plo->oid == s_aoid[i] && plo->Cid() == 0x75) return PtmplNoise(plo.get());
    return nullptr;
}

// orig: 0x00156548
NOISE* PnoiseNew(NOISETMPL* ptmpl, const glm::vec3& pos, LO* ploOwner)
{
    if (!ptmpl) return nullptr;
    if (std::getenv("SLY2N_NOISEDEBUG")) {
#ifdef _MSC_VER
        void* caller = _ReturnAddress();
#else
        void* caller = __builtin_return_address(0);
#endif
        std::fprintf(stderr, "noise new: t %.3f type %d tmpl %d r %.0f at (%.0f %.0f %.0f) owner %d from %p\n", g_clock.t, ptmpl->nType,
                     ptmpl->plo ? ptmpl->plo->oid : -1, ptmpl->sRadius, pos.x, pos.y, pos.z, ploOwner ? ploOwner->oid : -1,
                     caller);
    }
    // orig 0x11CCC0: a slot of the pool (SW +0x4008); native: the oldest released before this frame, else a new one
    NOISE* p = nullptr;
    if (auto& af = ApnoiseFree(); !af.empty() && af.front()->tFree < g_clock.t)
    {
        p = af.front();
        af.pop_front();
        ForgetLoListeners(p);  // (the original's slot is cleared: 0x2333B4(p, 0, 0x60))
        p->~NOISE();
        new (p) NOISE();
    }
    else
    {
        AnoiseOwned().push_back(std::make_unique<NOISE>());
        p = AnoiseOwned().back().get();
    }
    p->pcls = PclsFromCid(0x93);
    p->ptmpl = ptmpl;
    p->pos20 = pos;
    p->pos30 = pos;
    p->ploOwner40 = ploOwner;
    p->nSerial = ++s_nSerial;
    p->t = g_clock.t;
    p->nKind = ptmpl->nKind;
    NOISE* pn = p;
    Anoise().push_back(pn);
    AddRefNoise(pn);  // (the list's)
    // orig: 0x256EE0 followed by slot 3, event 2. The new noise is already
    // listed when its template's synchronous start handler runs.
    AddRefNoise(pn);  // (the start's own hold)
    pn->pframe = ptmpl->plo ? ptmpl->plo->pframe : nullptr;
    HandleLoSpliceEvent(pn, 2, false, 0, nullptr);
    ReleaseNoise(pn);
    return pn;
}

// orig: 0x00156780 (the object kept owned until the world's end: the jobs may still point at it)
void RemoveNoise(NOISE* pn)
{
    if (!FNoiseLive(pn)) return;
    AddRefNoise(pn);  // (the guard of the call)
    NotifyLo(pn, 15, pn);
    // A listener may already have removed it. The original checks again before
    // unlinking and running the synchronous end event.
    if (FNoiseLive(pn))
    {
        auto& a = Anoise();
        a.erase(std::remove(a.begin(), a.end(), pn), a.end());
        HandleLoSpliceEvent(pn, 3, false, 0, nullptr);
        ReleaseNoise(pn);  // (the list's)
    }
    ReleaseNoise(pn);
}

// orig: 0x001566B8
void AddRefNoise(NOISE* pn)
{
    if (pn) ++pn->cRef;
}

// orig: 0x001566C8 (0x11CD18: back to the pool at 0)
void ReleaseNoise(NOISE* pn)
{
    if (!pn || pn->cRef <= 0) return;
    if (--pn->cRef > 0) return;
    pn->tFree = g_clock.t;
    ApnoiseFree().push_back(pn);
}

// orig: 0x00156BE0
void SetHeldNoise(NOISE** ppn, NOISETMPL* ptmpl, const glm::vec3& pos, LO* ploOwner)
{
    if (NOISE* pold = *ppn)
    {
        RemoveNoise(pold);   // 0x156780
        *ppn = nullptr;
        ReleaseNoise(pold);  // vt slot 2
    }
    if (!ptmpl) return;
    *ppn = PnoiseNew(ptmpl, pos, ploOwner);
    AddRefNoise(*ppn);  // vt slot 1
}

size_t CNoiseAllocated() { return AnoiseOwned().size(); }
size_t CNoisePooled() { return ApnoiseFree().size(); }
// orig: 0x00140F90 event 0x27C walks the live DLI, including appended rows and
// skipping rows removed by callbacks. New noises append; storage remains owned
// until world teardown, so an already-visited object cannot be reused here.
int RemoveActorAlarms(LO* actor)
{
    std::unordered_set<NOISE*> visited;
    int removed = 0;
    for (;;)
    {
        auto next = std::find_if(Anoise().begin(), Anoise().end(),
                                 [&](NOISE* pn) { return !visited.count(pn); });
        if (next == Anoise().end()) return removed;
        NOISE* pn = *next;
        visited.insert(pn);
        if (pn->nKind == 4 && pn->ploTarget44 == actor)
        {
            RemoveNoise(pn);
            if (!FNoiseLive(pn)) ++removed;
        }
    }
}
bool FNoiseLive(const NOISE* pn) { return std::find(Anoise().begin(), Anoise().end(), pn) != Anoise().end(); }
const std::vector<NOISE*>& ApnoiseAll() { return Anoise(); }
void ResetNoises()
{
    MpListenerNoises().clear();
    Anoise().clear();
    ApnoiseFree().clear();
    AnoiseOwned().clear();
    MpTmpl().clear();
    s_nSerial = 0;
}

// orig: 0x00156898
float SRadiusNoise(const NOISE* pn)
{
    const NOISETMPL* pt = pn->ptmpl;
    if (!(0.0f < pt->tLife)) return pt->sRadius;
    float u = (g_clock.t - pn->t) / std::min(pt->tLife, T_RADIUS_MAX);
    u = std::min(1.0f, u);
    return pt->sRadius * (G_RADIUS_POLY[0] + u * (G_RADIUS_POLY[1] + u * G_RADIUS_POLY[2]));
}

// orig: 0x00156918
static bool FNoiseReachesScaled(const NOISE* pn, const glm::vec3& pos, float scale)
{
    const glm::vec3 d = pos - pn->pos20;
    glm::vec2 height = pn->ptmpl->rz;
    if (scale != 1.0f)
    {
        const float half = (height.y - height.x) * 0.5f;
        const float center = height.x + half;
        height = glm::vec2(center - half * scale, center + half * scale);
    }
    if (!(height.x <= d.z && d.z <= height.y)) return false;
    return !(scale * SRadiusNoise(pn) < std::sqrt(d.x * d.x + d.y * d.y));
}

bool FNoiseReaches(const NOISE* pn, const glm::vec3& pos) { return FNoiseReachesScaled(pn, pos, 1.0f); }

// orig: 0x00156FE8 (a listener (cid 0x2F) hears a noise: it is in the world (+0x28 bit 60), the noise's template
// is heard by radius (+0x50 0), the height difference is in its range (+0x58, 0x11D910) and it is horizontally closer
// than the template's radius (+0x54, not the growing one))
bool FListenerHearsNoise(const LO* plo, const NOISE* pn)
{
    if (!plo->fInWorld) return false;
    const NOISETMPL* pt = pn->ptmpl;
    if (pt->nType != 0) return false;
    const glm::vec3 d = PosLoWorld(plo) - pn->pos20;
    if (!(pt->rz.x <= d.z && d.z <= pt->rz.y)) return false;
    return std::sqrt(d.x * d.x + d.y * d.y) < pt->sRadius;
}

namespace
{
// orig: 0x00156EB8, with 0x00156DE0(false) first. The base splice update may
// change the active noises or remove this listener, so read its state after it.
void UpdateNoiseListener(LO* plo)
{
    if (PFNUPDATE base = PfnUpdateOrig(0x1A5E28)) base(plo);
    auto& heard = MpListenerNoises()[plo];
    for (auto it = heard.begin(); it != heard.end();)
    {
        if (FNoiseLive(*it)) { ++it; continue; }
        NOISE* expired = *it;
        it = heard.erase(it);
        TraceNoiseListener(plo, "prune", expired, heard.size());
        ReleaseNoise(expired);  // vt slot 2
    }
    int duplicates = 0;
    for (NOISE* pn : Anoise())
    {
        if (std::find(heard.begin(), heard.end(), pn) != heard.end()) { ++duplicates; continue; }
        if (!FListenerHearsNoise(plo, pn)) continue;
        if (heard.size() < 8)
        {
            AddRefNoise(pn);  // vt slot 1
            heard.push_back(pn);
            TraceNoiseListener(plo, "notify", pn, heard.size(), duplicates);
            HandleLoSpliceEvent(plo, 5, false, 0, nullptr);
            // The synchronous handler may remove its listener or its noises.
            // Do not access the retained list or the noise iterator again.
        }
        else TraceNoiseListener(plo, "full", pn, heard.size(), duplicates);
        return;  // original stops at the first new eligible noise even when full
    }
    if (duplicates) TraceNoiseListener(plo, "repeat", nullptr, heard.size(), duplicates);
}
}  // namespace

// orig: 0x00156D58, after base 0x1A5848: 0x156DE0(true) releases all retained
// noises, then the listener leaves the live list. FNoiseListenersReach below
// uses the class list plus the same in-world flag instead of a separate DLI.
void OnRemoveNoiseListener(LO* plo)
{
    auto& listeners = MpListenerNoises();
    auto it = listeners.find(plo);
    TraceNoiseListener(plo, "remove", nullptr, it == listeners.end() ? 0 : it->second.size());
    if (it != listeners.end())
    {
        const std::vector<NOISE*> an = std::move(it->second);
        listeners.erase(it);
        for (NOISE* pn : an) ReleaseNoise(pn);  // 0x156DE0(true): vt slot 2 each
    }
}

// the listeners of the list 0x3E1468 (cid 0x2F, added by their slot 11 0x156D20)
bool FNoiseListenersReach(const NOISE* pn)
{
    if (!g_psw || !pn || !pn->ptmpl) return false;
    for (LO* plo : g_psw->mpcidaplo[0x2F])
        if (plo && FListenerHearsNoise(plo, pn)) return true;
    return false;
}

// orig: 0x00208D70 (in guard.cpp too: FGuardFree)
static bool FGuardFreeN(const GUARD* pg)
{
    uint64_t m = 0;
    for (int n : {0xB, 0x1D, 7, 5, 6, 0x10, 1, 0x16, 0x2A}) m |= GrfStateBit(n);
    if (m & pg->grfState) return false;
    return pg->nE64 != 0;
}

// orig: 0x00208BE0 (vt slot 89)
bool FGuardAcceptsNoise(GUARD* pg, NOISE* pn)
{
    // Rajan's slot 89 (0x001CBEE0) doubles the sonic dart's hearing range for his check only.
    const bool sonicDart = pn->ptmpl->nType == 0 && pn->ploOwner40 && pn->ploOwner40->oid == 0x246;
    const bool rajan = sonicDart && pg->palo->pcls && pg->palo->pcls->PfnOrig(89) == 0x1CBEE0;
    const float rangeScale = rajan ? 2.0f : 1.0f;
    static const bool s_fDebug = std::getenv("SLY2N_NOISEDEBUG") != nullptr;
    if (s_fDebug)
        std::fprintf(stderr, "noise? guard %d noise kind %d type %d owner %d free %d e68 %d reach %d (r %.0f d %.0f dz %.0f)\n",
                     pg->palo->oid, pn->nKind, pn->ptmpl->nType, pn->ploOwner40 ? pn->ploOwner40->oid : -1, FGuardFreeN(pg), pg->nE68,
                     FNoiseReachesScaled(pn, PosAi(pg->palo), rangeScale), rangeScale * SRadiusNoise(pn),
                     glm::length(glm::vec2(PosAi(pg->palo) - pn->pos20)),
                     PosAi(pg->palo).z - pn->pos20.z);
    if (!FGuardFreeN(pg)) return false;
    if (pn->nKind < pg->nE68) return false;
    if (pn->ploOwner40 && pn->ploOwner40 == pg->palo) return false;
    if (pn->nKind == 4)
    {
        LO* px = pn->ploTarget44;
        const DETENTRY* pe = pg->pdet ? PdetentryFind(pg->pdet, px) : nullptr;
        if (px != pg->palo && !pe) return false;
    }
    const glm::vec3 pos = PosAi(pg->palo);
    if (pn->ptmpl->nType == 0) return FNoiseReachesScaled(pn, pos, rangeScale);
    if (pn->ptmpl->nType != 1) return false;
    // seen: within the live view volume, nothing solid on the line (0x20FAA0 with 0x20FA10, 0x14CC58)
    if (!pg->pdet) return false;
    return FGuardSeesPoint(pg->palo, pg->pdet->ploVolume, pos, pn->pos20);  // (detect.cpp)
}

// orig: 0x0020A168 (by the template's perception, the kind, the age, the distance, the order)
int NCompareNoises(GUARD* pg, const NOISE* a, const NOISE* b)
{
    if (int d = a->ptmpl->nType - b->ptmpl->nType) return d;
    if (int d = b->nKind - a->nKind) return d;
    if (b->t < a->t) return -1;
    if (a->t < b->t) return 1;
    const glm::vec3 pos = PosAi(pg->palo);
    const glm::vec3 da = a->pos20 - pos, db = b->pos20 - pos;
    const float sa = glm::dot(da, da), sb = glm::dot(db, db);
    if (sa < sb) return -1;
    if (sa < sb) return 1;
    return a->nSerial < b->nSerial ? -1 : (b->nSerial < a->nSerial ? 1 : 0);
}

// orig: 0x0020B140
void RaiseGuardAlarm(GUARD* pg, LO* ptarget)
{
    const glm::vec3 posT = PosAi(ptarget);
    if (std::getenv("SLY2N_NOISEDEBUG"))  // (debug: the alarms, the reference's 0x20B140 entries)
        std::fprintf(stderr, "alarm: t %.3f guard %d target %d\n", g_clock.t, pg->palo->oid, ptarget ? ptarget->oid : -1);
    NOISETMPL* pt = pg->ploE60 ? PtmplNoise(pg->ploE60) : PtmplNoiseFixed(4);  // (+0xE60, else SW +0x4030: 710)
    NOISE* pn = PnoiseNew(pt, PosAi(pg->palo), pg->palo);
    if (!pn) return;
    pn->pos30 = posT;
    pn->ploTarget44 = ptarget;
    pn->nKind = 4;
    pg->tF44 = g_clock.t + T_ALARM_REPEAT;
}

// orig: 0x00140F90 event 0x27C writes GAME +0x4758.
void SetGuardAlarmResume(float t)
{
    if (g_psw) g_psw->tGuardAlarmResume = t;
}

// orig: 0x00176098 reads GAME +0x4758 before emitting a pursuit alarm.
float TGuardAlarmResume()
{
    return g_psw ? g_psw->tGuardAlarmResume : 0.0f;
}

// orig: 0x0020A408
LO* PloGuardInterest(GUARD* pg, LO* ploCur)
{
    static const bool s_fDebug = std::getenv("SLY2N_NOISEDEBUG") != nullptr;
    if (s_fDebug) std::fprintf(stderr, "interest? guard %d free %d noises %zu\n", pg->palo->oid, FGuardFreeN(pg), Anoise().size());
    if (!FGuardFreeN(pg)) return ploCur;
    NOISE* best = ploCur ? dynamic_cast<NOISE*>(ploCur) : nullptr;
    LO* pbest = ploCur;
    for (NOISE* pn : Anoise())
        if (FGuardAcceptsNoise(pg, pn) && (!pbest || (best && NCompareNoises(pg, pn, best) < 0)))
        {
            best = pn;
            pbest = pn;
        }
    if (best && best->nKind == 4 && pg->pdet)
        if (DETENTRY* pe = PdetentryFind(pg->pdet, best->ploTarget44))
        {
            if (pe->st == 0)
            {
                SetDetentryState(pg->pdet, pe, 2);
                pg->tF04 = g_clock.t + T_AWARE_WAIT;
            }
            return ploCur;
        }
    return pbest;
}

// a harness's import (the reference's list 0x3E148C, oldest first)
void ImportNoise(NOISETMPL* ptmpl, const glm::vec3& pos20, const glm::vec3& pos30, LO* powner, LO* ptarget, int nSerial, float t,
                 int nKind)
{
    NOISE* pn = PnoiseNew(ptmpl, pos20, powner);
    if (!pn) return;
    pn->pos30 = pos30;
    pn->ploTarget44 = ptarget;
    pn->nSerial = nSerial;
    pn->t = t;
    pn->nKind = nKind;
}
void ClearNoiseList() { Anoise().clear(); }


// ---- 0xAB: the investigation ----

#include "aipursuit.h"
// xfmath.h and the splice value types both declare SMP; only these math
// functions are used here (the method registration above needs splice types).
float GRandInRange(float gLow, float gHigh);
float RadNormalize(float rad);

// orig: 0x00171F58
void JOBINVESTIGATE::Init()
{
    AddGuardState(pguard, 0x1F);
    st = -1;
    t40 = tEnd = FLT_MAX;
}
// orig: 0x00171FA0
void JOBINVESTIGATE::Enter() { SetInvestigateState(0); }
// orig: 0x00171FD0
void JOBINVESTIGATE::Exit()
{
    AIJOB::Exit();
    SetInvestigateState(-1);
    RemoveGuardState(pguard, 0x1F);
    SetNoise(nullptr);
}
// orig: 0x001725E0
void JOBINVESTIGATE::SetNoise(NOISE* pn)
{
    if (pn == pnoise) return;
    ReleaseNoise(pnoise);  // vt slot 2
    pnoise = pn;
    AddRefNoise(pn);       // vt slot 1
    fNew = 1;
}
// orig: 0x00172760 (an alarm about the guard itself: where it was raised, else the place it points at)
glm::vec3 JOBINVESTIGATE::PosNoise() const
{
    if (!pnoise) return PosAi(pguard->palo);
    if (pnoise->nKind == 4 && pnoise->ploTarget44 == pguard->palo) return pnoise->pos20;
    return pnoise->pos30;
}
// orig: 0x00172658 (no walk, or not on duty (+0xE78 2): yes; else the noise nearer than the guard's +0xE74 object + 50)
bool JOBINVESTIGATE::FNear()
{
    GUARD* pg = pguard;
    if (!(pg->grfFE8 & 2) || pg->nE78 == 2) return true;
    if (!pg->ploE74) return false;
    const glm::vec3 pos = PosAi(pg->palo);
    const glm::vec3 a = PosAi(pg->ploE74) - pos, b = PosNoise() - pos;
    if (std::getenv("SLY2N_NOISEDEBUG"))
        std::fprintf(stderr, "fnear guard %d at %.0f %.0f: home %d at %.0f %.0f, noise at %.0f %.0f\n", pg->palo->oid, pos.x, pos.y, pg->ploE74->oid,
                     PosAi(pg->ploE74).x, PosAi(pg->ploE74).y, PosNoise().x, PosNoise().y);
    return std::sqrt(b.x * b.x + b.y * b.y) < std::sqrt(a.x * a.x + a.y * a.y) + 50.0f;
}
// orig: 0x00172560 (the wait +0xE6C..+0xE70; the search around after 2 s when +0xE78 is 1)
void JOBINVESTIGATE::SetDeadlines()
{
    GUARD* pg = pguard;
    tEnd = g_clock.t + GRandInRange(pg->gE6C, pg->gE70);
    tSearch = pg->nE78 == 1 ? g_clock.t + 2.0f : FLT_MAX;  // (D_002BE5A0 / D_002A53B8)
}
namespace
{
// the place-finder's query (orig 0x850 bytes on the stack): the guard's place and heading, the places found
struct WAYQUERY
{
    glm::vec3 pos{0.0f};
    float rad = 0.0f;
    struct ROW
    {
        glm::vec3 pos;
        float rad;
    };
    std::vector<ROW> arow;
};
// orig: 0x00172798: a place 200..1200 away (D_002A53C0) and within 45 degrees of level (D_002A53C8), at most 64; its
// angle from the heading (0..2 pi; the original's negative case gives 2 pi - a, kept)
void AddSearchPlace(WAYQUERY* pq, const glm::vec3& pos)
{
    const glm::vec3 d = pos - pq->pos;
    const float s = glm::length(d);
    const float pan = RadNormalize(std::atan2(d.y, d.x));
    const float tilt = RadNormalize(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)));
    if (!(200.0f <= s && s <= 1200.0f)) return;
    if (!(-0.7853982f <= tilt && tilt <= 0.7853982f)) return;
    if (pq->arow.size() >= 0x40) return;
    float g = RadNormalize(pan - pq->rad);
    if (g < 0.0f) g = 6.2831855f - g;
    pq->arow.push_back({pos, g});
}
}  // namespace

// orig: 0x001728B0: four places to search: from the world's lists of places (the curves of classes 0xB / 0xD (their
// points nearest the guard), the objects of 0x58 / 0x27 / 0x12 (bit 0 and a 0x13) / 0x62, and those of 0x23 / 0x67 /
// 0x1B), by their angle from the heading, each 200 towards it from the guard and 150 apart; the rest at random around
// the point 350 behind the guard (the angles going on by -0.7..-0.5, 300..400 away). TODO(ai): the lists 0x3E13FC
// (with its +0x10C reach), 0x3E13F0, 0x3E1408 (empty in Paris and world 9).
void JOBINVESTIGATE::FindSearchPlaces()
{
    GUARD* pg = pguard;
    WAYQUERY q;
    q.pos = PosAi(pg->palo);
    q.rad = St(pg).radHeading;  // 0x204740
    auto objects = [&](CID cid, bool fCurve, auto&& filter) {
        if (!g_psw) return;
        for (LO* plo : g_psw->mpcidaplo[cid])
        {
            if (!plo->fInWorld || !filter(plo)) continue;
            if (fCurve)
            {
                if (const CRV* pcrv = PcrvFromLo(plo))
                {
                    glm::vec3 pos(0.0f);
                    NearestCrvAll(pcrv, q.pos, &pos, nullptr, nullptr, nullptr);
                    AddSearchPlace(&q, pos);
                }
            }
            else
                AddSearchPlace(&q, PosAi(plo));
        }
    };
    auto any = [](LO*) { return true; };
    objects(0x58, false, any);    // 0x3E13E4
    objects(0xB, true, any);      // 0x3E13C0
    objects(0x27, false, any);    // 0x3E13D8
    objects(0xD, true, any);      // 0x3E13CC
    objects(0x12, false, [](LO* plo) { return FCidDerivedFrom(plo->Cid(), 0x13); });  // 0x3E13B4 (TODO(ai): its +0xE8 bit 0)
    objects(0x23, false, any);    // 0x15E9C8 (0x505, cid 0x23)
    objects(0x67, false, any);
    objects(0x1B, false, any);
    objects(0x62, false, any);    // 0x3E1450
    std::stable_sort(q.arow.begin(), q.arow.end(), [](const WAYQUERY::ROW& a, const WAYQUERY::ROW& b) { return a.rad < b.rad; });
    int c = 0;
    float radLast = 0.0f;
    for (const WAYQUERY::ROW& row : q.arow)
    {
        if (c >= 4) break;
        glm::vec3 d = row.pos - q.pos;
        d.z = 0.0f;
        const float s = std::sqrt(d.x * d.x + d.y * d.y);
        const glm::vec3 dir = s < 0.0001f ? glm::vec3(1.0f, 0.0f, 0.0f) : d / s;
        const glm::vec3 pos = q.pos + dir * 200.0f;
        if (c > 0)
        {
            const glm::vec3 e = pos - aposWay[c - 1];
            if (std::sqrt(e.x * e.x + e.y * e.y) < 150.0f) continue;
        }
        radLast = row.rad;
        aposWayNear[c] = row.pos;
        aposWay[c++] = pos;
    }
    const glm::vec3 posAnchor = q.pos + glm::vec3(std::cos(q.rad + 3.1415927f), std::sin(q.rad + 3.1415927f), 0.0f) * -350.0f;  // 0x11E1A0
    for (; c < 4; ++c)
    {
        radLast += GRandInRange(-0.7f, -0.5f);
        const float rad = radLast + q.rad;
        const float s = GRandInRange(300.0f, 400.0f);
        aposWay[c] = aposWayNear[c] = posAnchor + glm::vec3(std::cos(rad), std::sin(rad), 0.0f) * s;
    }
    iway = 0;
    tSearch = -FLT_MAX;  // (+0x3C: D_002A53D0)
}

// orig: 0x00172F10: the next place once the go-to has arrived and a second has passed (its +0x3C time FLT_MAX while
// walking), in turn
void JOBINVESTIGATE::NextSearchPlace()
{
    if (iway >= 4) return;
    auto* o = static_cast<JOBGOTO*>(pchild);
    if (tSearch == FLT_MAX)
    {
        if (o && o->NArrived()) tSearch = g_clock.t + 1.0f;
    }
    if (g_clock.t < tSearch) return;
    const glm::vec3 pos = aposWay[iway++];
    if (iway >= 4) iway = 0;
    GOAL g;
    g.pos = glm::vec4(pos, 0.0f);
    if (o) o->SetGoal(g);
    tSearch = FLT_MAX;  // (D_002A53D4)
}

// orig: 0x00172320
void JOBINVESTIGATE::SetInvestigateState(int n)
{
    if (st == n) return;
    EndChildJob(&pchild);
    st = n;
    tSt = g_clock.t;
    GUARD* pg = pguard;
    switch (n)
    {
    case 0:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->nAnim = 12;
        o->Enter();
        break;
    }
    case 1:
    {
        auto* o = static_cast<JOBGOTO*>(PjobPush(pg, 0xBA));
        o->fC0 = 1;
        GOAL g;
        g.pos = glm::vec4(PosNoise(), 0.0f);
        o->SetGoal(g);
        o->Enter();
        tEnd = FLT_MAX;  // (D_002A5398)
        break;
    }
    case 2:
    {
        const glm::vec3 d = PosNoise() - PosAi(pg->palo);
        SetMoveHeading(&pg->move, std::atan2(d.y, d.x));
        PjobPush(pg, 0x9E)->Enter();
        SetDeadlines();
        break;
    }
    case 3:
    {
        auto* o = static_cast<JOBGOTO*>(PjobPush(pg, 0xBA));
        o->fC0 = 1;
        FindSearchPlaces();
        NextSearchPlace();
        o->Enter();
        break;
    }
    case 4:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9D));
        o->nAnim = 14;
        o->Enter();
        SetNoise(nullptr);
        fNew = 0;
        break;
    }
    case 5:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pg, 0x9E));
        o->nAnim = 1;
        o->Enter();
        break;
    }
    default: break;
    }
}
// orig: 0x00172018
void JOBINVESTIGATE::Update()
{
    AIJOB::Update();
    GUARD* pg = pguard;
    auto childDone = [&]() { return pchild && static_cast<JOBMOVE*>(pchild)->st == 1; };  // (0x174FB8)
    while (!FJobDone(this))
    {
        const int s = st;
        switch (s)
        {
        case 0:
            if (childDone()) SetInvestigateState(1);
            break;
        case 1:
            if (FGuardStill(pg)) { SetInvestigateState(5); break; }
            if (fNew)
            {
                if (FNear())
                {
                    SetInvestigateState(2);
                    fNew = 0;
                    break;
                }
                GOAL g;
                g.pos = glm::vec4(PosNoise(), 0.0f);
                if (auto* o = static_cast<JOBGOTO*>(pchild)) o->SetGoal(g);
                fNew = 0;
                break;
            }
            if (FNear()) { SetInvestigateState(2); break; }
            if (auto* o = static_cast<JOBGOTO*>(pchild))
                if (o->NArrived() || o->FStuck()) SetInvestigateState(2);  // (0x17DCB8 / 0x17DCF8)
            break;
        case 2:
            if (FGuardStill(pg)) { SetInvestigateState(5); break; }
            if (fNew)
            {
                if (!FNear())
                {
                    SetInvestigateState(1);
                    fNew = 0;
                    break;
                }
                SetDeadlines();
                fNew = 0;
                break;
            }
            if (tEnd < g_clock.t || childDone()) { SetInvestigateState(4); break; }
            if (tSearch < g_clock.t) { SetInvestigateState(3); break; }
            {
                const glm::vec3 d = PosNoise() - PosAi(pg->palo);
                SetMoveHeading(&pg->move, std::atan2(d.y, d.x));
            }
            break;
        case 3:
            if (FGuardStill(pg)) { SetInvestigateState(5); break; }
            if (fNew)
            {
                SetInvestigateState(FNear() ? 2 : 1);
                fNew = 0;
                break;
            }
            if (tEnd < g_clock.t) SetInvestigateState(4);
            else NextSearchPlace();
            break;
        case 4:
            if (fNew)
            {
                SetInvestigateState(FNear() ? 2 : 1);
                fNew = 0;
                break;
            }
            if (childDone()) SetInvestigateState(-1);
            break;
        case 5:
            if (!FGuardStill(pg)) SetInvestigateState(1);
            break;
        default: break;
        }
        if (st == s) break;
    }
}

std::unique_ptr<AIJOB> PjobNewNoise(CID cid)
{
    if (cid == 0xAB) return std::make_unique<JOBINVESTIGATE>();
    return nullptr;
}
