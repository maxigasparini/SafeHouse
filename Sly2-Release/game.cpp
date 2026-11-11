// The game loop (orig main 0x1A7518's loop, in the Sly1-Release style): per frame the transition (0x20E890),
// UpdateJoy (0x1871C0), UpdateClock (0x14D0B8), the world update (0x226550), then the frame drawn with the
// renderer's scene from the object model, paced to 60 Hz.
//
//   Sly2-Release [--world W] [--frames N] [--headless] [--fixed-dt] [--press BTN@frame+n,...]
//               [--frame-dump file] [--screenshot file]

#include <malloc.h>
#include "so.h"
#include "crv.h"
#include "vtnative.h"
#include "game.h"
#include "breakable.h"
#include "nav.h"
#include "skipcut.h"
#include "debugmenu.h"
#include "pccharacter.h"
#include "glob.h"
#include "pickup.h"
#include "spiop.h"
#include "asega.h"
#include "frameinterp.h"
#include "splicetypes.h"
#include <unordered_set>
#include "objmethod.h"
#include "screenrt.h"
#include "overlay.h"
#include "photo.h"
#include <unordered_map>
#include <functional>
#include "actor.h"
#include "gadget.h"
#include "damage.h"
#include "jt.h"
#include "gang.h"
#include "so.h"
#include "xfphys.h"
#include "vecmat.h"
#include "bif.h"
float GRandInRange(float gLow, float gHigh);  // xfmath.h (0x11D448)
int NRandom();  // xfmath.h (0x11D3C0)
#include "refram.h"
#include <cstdint>
extern uint32_t g_uRandState;  // xfmath.cpp
#include "task.h"
#include "pcinput.h"
#include "stepphys.h"
#include "cplcy.h"
#include "sound.h"
#include "sly.h"
#include "climbsurf.h"
#include "hide.h"
#include "worldaccess.h"

#include "anim.h"
#include "activate.h"
#include "smrun.h"
#include "rtgt.h"

#include "alo.h"
#ifdef __SWITCH__
#include "passhelpers.h"
#endif
#include "brx.h"
#include "clock.h"
#include "guard.h"
#include "aijobs.h"
#include "cmworld.h"
#include "cls.h"
#include "display.h"
#include "emitter.h"
#include "gl.h"
#include "frontend.h"
#include "hudwidget.h"
#include "joy.h"
#include "light.h"
#include "lightsw.h"
#include "render.h"
#include "scene.h"
#include "scenesw.h"
#include "shd.h"
#include "sw.h"
#include "update.h"
#include "savedata.h"
#include "../platform/platform.h"
#include "../platform/frame_pacer.h"
#include "../platform/game_sched.h"
#include "transition.h"
#include "po.h"
#include "shop.h"
#include "step.h"
#include "jump.h"
#include "ai.h"
#include "aseg.h"
#include "so.h"
#include "xfphys.h"
#include "cm.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cfenv>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3   // (newlib on the Switch: the FPS log's thread CPU column; clock_gettime fails for it)
#endif

double g_msGpuFpsLog = 0, g_msHudFpsLog = 0;   // (SLY2N_FPSLOG)

namespace
{
std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The renderer's options from a reference RAM dump (the cel border colour, FOGCOL) and its camera (CM 0x2DDF40, (its +0x60 matrix, row 0 the
// view direction, row 3 the position; +0x44 the frustum tangent, +0x4C near), else above the world's origin.

CAMERA CameraFromRam(const std::string& ramPath, RENDEROPTS& opts)
{
    CAMERA cam;
    cam.pos = glm::vec3(0, -3000, 3000);
    cam.yaw = 90;
    cam.pitch = -30;
    const std::vector<uint8_t> ram = ReadAll(ramPath);
    if (ram.size() < 0x2000000) return cam;
    const uint32_t cm = 0x2DDF40;
    float m[16], ty, sNear;
    std::memcpy(m, ram.data() + cm + 0x60, 64);
    std::memcpy(&ty, ram.data() + cm + 0x44, 4);
    std::memcpy(&sNear, ram.data() + cm + 0x4C, 4);
    std::memcpy(&cam.rMRDAdjust, ram.data() + cm + 0x1C, 4);
    cam.pos = glm::vec3(m[12], m[13], m[14]);
    cam.yaw = glm::degrees(std::atan2(m[1], m[0]));
    cam.pitch = glm::degrees(std::asin(std::clamp(m[2], -1.0f, 1.0f)));
    if (ty > 0.01f && ty < 10.0f) cam.fovY = glm::degrees(2.0f * std::atan(ty));
    if (sNear > 0.1f && sNear < 1000.0f) cam.sNear = sNear;
    std::memcpy(&opts.rgbaCel, ram.data() + 0x2DEB50, 4);
    std::memcpy(&opts.sFogNear, ram.data() + cm + 0x240, 4);
    std::memcpy(&opts.sFogFar, ram.data() + cm + 0x244, 4);
    std::memcpy(&opts.uFogMax, ram.data() + cm + 0x248, 4);
    std::memcpy(&opts.rgbaFog, ram.data() + 0x2DE18C, 4);
    opts.fFog = opts.sFogNear > 0 && opts.sFogFar > opts.sFogNear;
    return cam;
}
}

// One frame of objects' state, in the reference's SLY2_FRAMEDUMP format (Sly2-Reference runtime/interp.cpp).
extern uint32_t g_uRandState;   // (xfmath.cpp)
extern uint64_t g_cRandLibc;
void WriteFrameDump(FILE* pf, const SW* psw, int frame)
{
    std::fprintf(pf, "F %d %.6f %.6f\n", frame, g_clock.t, g_clock.dt);
    if (static const bool s_fRand = std::getenv("SLY2N_DUMPRAND") != nullptr; s_fRand)   // (debug: the shared random stream)
        std::fprintf(pf, "R %08x %llu\n", g_uRandState, static_cast<unsigned long long>(g_cRandLibc));
    std::fprintf(pf, "C %.3f %.3f %.3f %.5f %.5f %.5f %.5f\n", g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.mat[0].x, g_cm.mat[0].y,
                 g_cm.mat[0].z, g_cm.radFOV);  // the camera (cm.h)
    for (int cid = 0; cid < CID_Max; ++cid)
        for (const LO* plo : psw->mpcidaplo[cid])
        {
            glm::vec3 pos(0.0f);
            if (FCidDerivedFrom(cid, 0x30))
            {
                const ALO* palo = static_cast<const ALO*>(plo);
                if (!palo->axf.empty()) pos = glm::vec3(palo->axf[0]->matWorld[3]);
            }
            std::fprintf(pf, "%d %d %08x %.3f %.3f %.3f\n", cid, plo->oid, plo->iupd >= 0 ? 0u : plo->grfUpd, pos.x, pos.y,
                         pos.z);
        }
}

// SLY2N_MEMBDUMP=<file>:<frame>: the objects' activation state at that frame's start, in the format of the
// reference-side script: "cid oid root st removed inworld active queued registered mask radius
// parent"
void WriteMembDump(const SW* psw, int frame)
{
    static const char* s_spec = std::getenv("SLY2N_MEMBDUMP");
    if (!s_spec) return;
    const std::string spec(s_spec);
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos || std::atoi(spec.c_str() + colon + 1) != frame) return;
    FILE* pf = std::fopen(spec.substr(0, colon).c_str(), "w");
    if (!pf) return;
    for (int cid = 0; cid < CID_Max; ++cid)
        for (LO* plo : psw->mpcidaplo[cid])
        {
            LO* ploRoot = PloRoot(plo);
            std::fprintf(pf, "%d %d %d %d %d %d %d %d %d 0x%x %.1f 0x%x root %d cid 0x%X group %d clone %d\n", cid,
                         plo->oid, plo->paloParent ? 0 : 1, plo->st & 15, plo->fRemoved, plo->fInWorld,
                         ploRoot->fNodeActive, ploRoot->fQueued, plo->iupd >= 0, plo->bMask, plo->sRadius,
                         plo->paloParent ? 1u : 0u, ploRoot->oid, ploRoot->Cid(), ploRoot->igrp, ploRoot->iclone);
        }
    if (std::FILE* pfx = std::fopen((spec.substr(0, colon) + ".expl").c_str(), "w"))
    {
        for (CID cid = 0x20; cid <= 0x21; ++cid)
            for (LO* plo : psw->mpcidaplo[cid])
            {
                const LOX* px = static_cast<const LOX*>(plo);
                glm::mat4 m = px->mat;
                if (px->pxf) m = px->pxf->matWorld * px->mat;
                else if (plo->paloParent && !plo->paloParent->axf.empty()) m = plo->paloParent->axf[0]->matWorld * px->mat;
                const glm::vec3 v(m[3]);
                std::fprintf(pfx, "%d %d %.0f %.0f %.0f\n", cid, plo->oid, v.x, v.y, v.z);
            }
        std::fclose(pfx);
    }
    if (std::FILE* pfs = std::fopen((spec.substr(0, colon) + ".sma").c_str(), "w"))
    {
        for (const SMA* p : g_apsma)
            std::fprintf(pfs, "%d %d %d %d\n", p->psm ? p->psm->oid : -1, p->palo ? p->palo->oid : -1,
                         OidFromSmIsms(p->psm, p->ismsCur), OidFromSmIsms(p->psm, p->ismsGoal));
        std::fclose(pfs);
    }
    std::fclose(pf);
}

// The render-target owners: class 0x2D objects (oids 686 / 687, children of Sly 7, Bentley 8 and Murray 9: the
// binocucom's talking heads). Their options: 1060 the vertical fov (+0x11C), 1061 / 1062 the clips (+0x120 /
// +0x124), 1064 (0x1BEAA8) an object of the list (+0xE0 count, +0xE4 objects: the speaker and his parts), 1063
// the camera's object (+0x118; unset: the owner, placed by its own LOX matrix), 1065 (0x1BEAD0) a light of the
// target (+0x104 / +0x108: set +0x15C / +0x160, removed from the world).
namespace
{
struct RTOWNER
{
    std::vector<LO*> aplo;          // +0xE4
    std::vector<uint8_t> agrf;      // +0xF4: 1 added by the take, 2 hidden by it
    LO* ploCamera = nullptr;        // +0x118 (option 1063; none: the owner itself, a LOX)
    std::vector<LO*> aploLight;     // +0x104 / +0x108 (option 1065, 0x1BEAD0: the target's lights, out of the world;
                                    // TODO: light the target's objects with them)
    float radFov = 1.0f, sNear = 4.0f;
};
SW* s_pswRtgt = nullptr;
uint32_t s_serialRtgt = 0;
std::unordered_map<const LO*, RTOWNER> s_mprtowner;

RTOWNER* PrtownerOf(const void* powner)
{
    auto it = s_mprtowner.find(static_cast<const LO*>(powner));
    return it == s_mprtowner.end() ? nullptr : &it->second;
}

// the class 0x2D objects' options, once per world
void BuildRtgtOwners(SW* psw)
{
    s_mprtowner.clear();
    for (LO* plo : psw->mpcidaplo[0x2D])
    {
        RTOWNER& r = s_mprtowner[plo];
        for (const OPT& o : plo->aopt)
            for (const OPTVAL& v : o.aval)
                switch (o.eopid)
                {
                case 1060: r.radFov = v.ag[0]; break;
                case 1061: r.sNear = v.ag[0]; break;
                case 1064: if (LO* p = PloFindSwObjectByOid(psw, v.n)) r.aplo.push_back(p); break;
                case 1063: r.ploCamera = PloFindSwObjectByOid(psw, v.n); break;   // (field +0x118)
                case 1065:
                    // orig: 0x1BEAD0: the light into the owner's list, its +0x160 and +0x15C set (not baked, not a
                    // light-context candidate) and removed from the world (vt slot 9). Else the characters' own
                    // lights (Sly 3797, Bentley 7430, Murray 7031) lit everything near them (N08: the flop's dome 6958)
                    if (LO* p = PloFindSwObjectByOid(psw, v.n))
                    {
                        r.aploLight.push_back(p);
                        p->fLightOwned = true;
                        RemoveLo(psw, p);
                    }
                    break;
                default: break;
                }
        r.agrf.assign(r.aplo.size(), 0);
        if (std::getenv("SLY2_RTGTDUMP"))
        {
            std::printf("rtgt owner %d (parent %d): fov %.3f camera %d list", plo->oid, plo->paloParent ? plo->paloParent->oid : -1,
                        r.radFov, r.ploCamera ? r.ploCamera->oid : -1);
            for (LO* p : r.aplo) std::printf(" %d", p->oid);
            std::printf("\n");
            if (r.ploCamera)
            {
                std::printf("  camera options:");
                for (const OPT& o : r.ploCamera->aopt)
                    for (const OPTVAL& v : o.aval) std::printf(" [%d k%d n%d g %.3f %.3f %.3f]", o.eopid, v.k, v.n, v.ag[0], v.ag[1], v.ag[2]);
                std::printf("\n");
            }
        }
    }
}

// a child of plo (the namespace it loaded) with this oid (0x15E938 kind 1)
LO* PloChildOfOid(LO* plo, OID oid)
{
    if (!plo) return nullptr;
    for (LO* p : plo->aploOwned)
        if (p && p->oid == oid) return p;
    return nullptr;
}

// orig 0x001BEB28's owner side: the list's objects out of the world added (vt slot 8, their root's node made
// active: 0x11A220 + slot 23), the drawn ones hidden in the frame (slot 21: natively their entries go into the
// target instead, ApplyRtgtOwners), the owner shown
void TakeRtgt(const void* powner, int)
{
    RTOWNER* pr = PrtownerOf(powner);
    if (std::getenv("SLY2_RTGTDUMP"))
        std::printf("rtgt taken by %d (parent %d)\n", pr ? static_cast<const LO*>(powner)->oid : -1,
                    pr && static_cast<const LO*>(powner)->paloParent ? static_cast<const LO*>(powner)->paloParent->oid : -1);
    if (!pr || !s_pswRtgt) return;
    for (size_t i = 0; i < pr->aplo.size(); ++i)
    {
        LO* plo = pr->aplo[i];
        pr->agrf[i] = 0;
        if (!plo->fInWorld)
        {
            pr->agrf[i] |= 1;
            AddLo(s_pswRtgt, plo);
            if (LO* proot = PloRoot(plo); !FLoActive(plo) && !proot->fNodeActive)
            {
                proot->fNodeActive = true;   // (activate.cpp's SetNodeActive)
                RegisterLoTree(s_pswRtgt, proot);
            }
        }
    }
}

// orig 0x001BED18's owner side: the objects it added removed again (slot 9)
void GiveRtgt(const void* powner)
{
    RTOWNER* pr = PrtownerOf(powner);
    if (!pr || !s_pswRtgt) return;
    for (size_t i = 0; i < pr->aplo.size(); ++i)
    {
        if (pr->agrf[i] & 1) RemoveLo(s_pswRtgt, pr->aplo[i]);
        pr->agrf[i] = 0;
    }
}
}  // namespace

// orig 0x001BEED8: the object a screen quad's render target id names: 0x2AF the player's child of that oid, 0x2AE
// the same child of the player's binocucom speaker (a PLAYER's +0xF40 = BINOC +0x40), otherwise the object of
// that oid; the render target itself belongs to that object (rtgt.h)
const void* PvRtgtOwnerOfId(int id)
{
    LO* pp = g_transition.ploPlayer;
    // (0x15E938(G, 1, id, object): the object or one below it with that oid)
    // (as spliceglue.cpp's kind 1: its namespace, recursively, then any object whose parents include it)
    std::function<LO*(LO*, int)> inNs = [&](LO* plo, int oid) -> LO* {
        for (LO* pc : plo->aploOwned)
        {
            if (pc->oid == oid || pc->oidInstance == oid) return pc;
            if (LO* p = inNs(pc, oid)) return p;
        }
        return nullptr;
    };
    auto below = [&](LO* plo, int oid) -> LO* {
        if (!plo) return nullptr;
        if (plo->oid == oid) return plo;
        if (LO* p = inNs(plo, oid)) return p;
        if (s_pswRtgt)
            for (const auto& up : s_pswRtgt->aploAll)
                if (up->oid == oid || up->oidInstance == oid)
                    for (const LO* q = up->paloParent; q; q = q->paloParent)
                        if (q == plo) return up.get();
        return nullptr;
    };
    if (std::getenv("SLY2N_RTGTBIND") && id == 0x2AF && pp)
        for (LO* q = pp; q; q = q->paloParent) std::printf("rtgt player chain %d cid 0x%X\n", q->oid, int(q->Cid()));
    if (id == 0x2AF) return below(pp, 0x2AF);
    if (id == 0x2AE)
    {
        LO* pspk = pp;
        if (pp && FIsLoDerivedFrom(pp, 0x34))
            if (ACTOR* pa = PactorFromLo(pp)) pspk = pa->binoc.ploSpeaker;
        return below(pspk, 0x2AE);
    }
    return s_pswRtgt ? PloFindSwObjectByOid(s_pswRtgt, id) : nullptr;
}

// Each frame after the draw pass: the owned targets' lists' entries (and their drawn child objects) placed into
// their target instead of the frame, the targets' cameras from the owners' camera objects (0x1BE6F0: the
// object's world matrix rows b0 up, b1 forward, its position)
LIGHTSET s_alightsRtgt[CRTGT];
// the ALOs with flag bit 34 of their glob set (ALO +0xD0; option 69, SetSoIgnoreLocked 0x1F2768): drawn in the
// foreground kinds while no big screen object shows (RenderGlob 0x168638, scene.h fForeground)
// (each entry with the world it was set in, SW::serial: an object of an earlier world at a reused address never
// matches; entries of earlier worlds are dropped)
std::unordered_map<const LO*, uint32_t> s_mpploForeground;
struct INITFOREGROUND
{
    INITFOREGROUND()
    {
        RegisterObjectSetter(69, [](LO* plo, const CRef& r) {
            const bool f = r.m_tagk == TAGK_Bool ? r.m_bool : r.m_n != 0;
            if (f) s_mpploForeground[plo] = g_psw ? g_psw->serial : 0;
            else s_mpploForeground.erase(plo);
        });
    }
} s_initforeground;   // each owned target's own lights (0x1A4AD8 from the owner's +0x104 / +0x108)

void ApplyRtgtOwners(SCENE& scene, const std::vector<ALO*>& apalo)
{
    for (SCENEGLOBSET& sgs : scene.aglobset) sgs.irtgt = -1;
    for (LIGHTSET& l : s_alightsRtgt) l = LIGHTSET{};
    std::unordered_map<const ALO*, int> mpirtgt;
    for (int i = 0; i < CRTGT; ++i)
    {
        const RTGT* pt = PrtgtGet(i);
        RTOWNER* pr = pt ? PrtownerOf(pt->powner) : nullptr;
        if (!pr) continue;
        std::function<void(LO*, bool)> add = [&](LO* plo, bool fTop) {
            ALO* palo = dynamic_cast<ALO*>(plo);
            if (!palo || plo->Cid() == 0x2D) return;
            const uint32_t pfnDraw = palo->pcls ? palo->pcls->PfnOrig(19) : 0;
            const bool fDraws = pfnDraw != 0 && pfnDraw != 0x1A5ED8 && palo->gFade != 0.0f;
            // orig: 0x1A5448 / 0x1F2C30: only children before the first removed
            // child belong to the active tree. Removed attachments can retain draw flags.
            if (!fTop && (plo->fRemoved || !(GrfUpdLo(s_pswRtgt, plo) & 8))) return;
            if (fDraws) mpirtgt[palo] = i;
            for (LO* pc : palo->aploChild) add(pc, false);
        };
        for (LO* plo : pr->aplo) add(plo, true);
        LIGHTSET& ls = s_alightsRtgt[i];
        ls.fFixed = true;
        ls.worldShader = WorldLights().worldShader;
        for (LO* plight : pr->aploLight)
        {
            LIGHTREC rec;
            LIGHTSEL sel;
            if (FLightOfLo(plight, rec, sel)) ls.actx.emplace_back(rec, sel);
        }
        {
            LO* ploCam = pr->ploCamera ? pr->ploCamera : const_cast<LO*>(static_cast<const LO*>(pt->powner));
            const glm::mat4 m = MatJtLo(ploCam);
            // (debugging: SLY2_RTGTDUMP, every 120th frame)
            static const bool s_fDump = std::getenv("SLY2_RTGTDUMP") != nullptr;
            if (static int s_c = 0; s_fDump && s_c++ % 120 == 0)
            {
                if (const LOX* px = dynamic_cast<const LOX*>(ploCam))
                    std::printf("rtgt cam ref: ixf %d owner %d parent %d local pos %.1f %.1f %.1f r0 %.2f %.2f %.2f\n", px->ixfRef,
                                px->pxf && px->pxf->palo ? px->pxf->palo->oid : -1, px->paloParent ? px->paloParent->oid : -1,
                                px->mat[3][0], px->mat[3][1], px->mat[3][2], px->mat[0][0], px->mat[0][1], px->mat[0][2]);
                for (const auto& [rec, sel] : s_alightsRtgt[i].actx)
                    std::printf("rtgt light kind %u rgba %.2f %.2f %.2f %.2f dir %.2f %.2f %.2f off %.3f scale %.3f\n", rec.kind, rec.rgba.r, rec.rgba.g,
                                rec.rgba.b, rec.rgba.a, rec.dirOrPos.x, rec.dirOrPos.y, rec.dirOrPos.z, rec.angularOffset, rec.angularScale);
                std::printf("rtgt cam %d cid 0x%X: r0 %.2f %.2f %.2f r1 %.2f %.2f %.2f r2 %.2f %.2f %.2f\n", ploCam->oid, ploCam->Cid(),
                            m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
            }
            // 0x1BE6F0 hands CombineEyeLookAtProj (0x14D8F8) the rows (-r2, -r0, r1) of the object's 0x1A6318
            // matrix: forward -r2, up r1
            SetRtgtCamera(i, glm::vec3(m[3]), glm::vec3(m[1]), -glm::vec3(m[2]), -glm::vec3(m[0]), pr->radFov, pr->sNear);
        }
    }
    if (static const bool s_f = std::getenv("SLY2_RTGTDUMP") != nullptr; s_f)
    {
        static int s_c = 0;
        if (s_c++ % 60 == 0)
        {
            std::printf("rtgt frame: %zu objects", mpirtgt.size());
            for (auto& [p, i] : mpirtgt)
            {
                const glm::vec3 q = PosLoWorld(p);
                std::printf(" %d:%d(%.0f %.0f %.0f)", p->oid, i, q.x, q.y, q.z);
            }
            for (int i = 0; i < CRTGT; ++i)
                if (const RTGT* pt = PrtgtGet(i); pt && pt->powner)
                    std::printf(" | cam%d %.0f %.0f %.0f fwd %.2f %.2f %.2f", i, pt->pos.x, pt->pos.y, pt->pos.z, pt->dirForward.x, pt->dirForward.y, pt->dirForward.z);
            std::printf("\n");
        }
    }
    if (mpirtgt.empty()) return;
    for (size_t k = 0; k < scene.aglobset.size() && k < apalo.size(); ++k)
        if (auto it = mpirtgt.find(apalo[k]); it != mpirtgt.end())
        {
            PlaceSceneEntry(apalo[k], scene.aglobset[k]);
            scene.aglobset[k].irtgt = int8_t(it->second);
        }
}

bool FImportSlyStart(SW* psw, const std::string& pathRam, const std::string& pathTrace, int vbBase);  // slycheck.cpp
bool FImportAiStart(SW* psw, const std::string& pathRam);  // animref.cpp (SLY2N_AI_START=1: the guards too)
int g_iframeMain = 0;
void CompareSlyFrame(SW* psw, int frame);
void PrepareSlyFrame(int frame);
extern int g_iframeHarness;

// Debug (F6, temporary, for testing the gadgets): every powerup acquired (the progress "powerUps mppukfAcquired",
// items 7..0x2A are the shop's; the others are the vault's / the episodes' rewards), 9999 coins, Bentley and Murray
// selectable at the safe house however far the episode is (po.cpp UnlockSafeHouseCharacters: the hub's script
// otherwise allows a character once his first job's task is 1+, e.g. Paris: Bentley task 1586, Murray 1878), and
// the players told as after a purchase (0x19F0A8). The gadgets read the progress when used: no reload needed.
void DebugUnlockAll(SW* psw)   // (also the debug menu's Player page, debugmenu.cpp)
{
    int c = 0;
    CRef refTrue;
    refTrue.SetBool(1);
    for (int i = 0; i < 64; ++i)
        if (FSetProgress({"powerUps", "mppukfAcquired", std::to_string(i)}, refTrue)) ++c;
    CRef refCoins;
    refCoins.SetS32(9999);
    FSetProgress({"loot", "ccoin"}, refCoins);
    const int csh = UnlockSafeHouseCharacters(psw);
    NotifyPlayersPowerups(psw);
    std::printf("F6: %d powerups acquired, 9999 coins, Sly / Bentley / Murray selectable at the safe house "
                "(%d safe house%s here updated; others as they load)\n", c, csh, csh == 1 ? "" : "s");
}
void ReportSlyHarness();
bool FImportGangStart(SW* psw, const std::string& pathRam, const std::string& pathTrace, int vbBase);  // gangcheck.cpp
void PrepareGangFrame(int frame);
void ReportGangHarness();

bool g_fDevCard = true;

int RunGame(const GAMEOPTS& go)
{
    // SLY2N_RTZ=1: the PS2's float rounding (toward zero) for the whole game (the differential harnesses: the
    // reference's interpreter rounds its FPU / VU arithmetic that way)
    if (const char* e = std::getenv("SLY2N_RTZ"); e && *e == '1') std::fesetround(FE_TOWARDZERO);
    // debug: SLY2N_RANDSEED=n: the game's random state before the world's load (another draw of the guards' looks)
    if (const char* sz = std::getenv("SLY2N_RANDSEED")) g_uRandState = static_cast<uint32_t>(std::strtoul(sz, nullptr, 0));
    // the splices' random (bif 53) draws from the game's random state (GRandInRange 0x11D448), not the default LCG:
    // the guards' random looks follow the game's rand (AI code)
    if (!std::getenv("SLY2N_SPLICELCG")) { g_pfnGRandomSplice = GRandInRange; g_pfnNRandomSplice = NRandom; }  // (debug: the old LCG)
    InitClasses();
    StartupGameSound();  // orig 0x1FCEA0
    std::string error;
    // the save (orig: the card's slot loaded by the menus 0x1AD740.., copied to 0x3D4A60)
    {
        std::string path = go.save;
        if (path.empty())
        {
            const std::string user = cane::utf8(cane::userDirectory() / "Saves" / "save1");
            const std::string dev = SLY2N_DEV_ROOT "/build/saves/round4-user-card/memcard0/BASCUS-97316YAOTWTD!/save1";
            path = std::filesystem::exists(cane::path(user)) || !g_fDevCard ? user : dev;
        }
        if (FLoadGameSave(path, error)) std::printf("save: %s\n", path.c_str());
        else std::printf("save: none (%s)\n", error.c_str());
    }

    // the first world through the transition (orig: the boot sequence's request, 0x1A4E08), as the reference's
    // warp does (0x210990 + 0x22B490 at point 393)
    std::unique_ptr<SW> psw;
    // boot: the disc's boot sequence (0x1A4E08: the logo movie, the slots, the title); frontend: the front end world
    // (test: SLY2N_READSLOTS=1: a world run reads <user-dir>/Saves's slots too, so the data access pages show them)
    if (go.world != "boot" && go.world != "frontend" && std::getenv("SLY2N_READSLOTS")) ReadSaveSlots("");
    if (go.world == "boot") StartBootSequence(!go.fHeadless && !std::getenv("SLY2_NOMOVIE"));
    else if (go.world == "frontend")
    {
        ReadSaveSlots(SLY2N_DEV_ROOT "/build/saves/round4-user-card/memcard0/BASCUS-97316YAOTWTD!");
        RequestTransition(nullptr, g_disc.PbElf(VA_FK_FRONTEND, 0x40), 0x189, -1, 0, -1);
    }
    else if (go.task >= 0) RequestTaskStart(nullptr, std::atoi(go.world.c_str()), go.task);
    else RequestWorldWarp(nullptr, std::atoi(go.world.c_str()), 0x189, -1);
    // (the window comes later: the boot's movie plays when the loop starts)
    // debug: SLY2N_SETPROGRESS=a.b.c=value[,...] sets progress values once the first world is loaded (its layout) (an int, or a float
    // with a '.'; e.g. a fresh episode: episode.nightclub.dt=0.0,episode.nightclub.percent_complete=0)
    auto applySetProgress = [] {
    if (const char* sz = std::getenv("SLY2N_SETPROGRESS"))
    {
        std::string all = sz;
        for (size_t i = 0, j; i < all.size(); i = j + 1)
        {
            j = all.find(',', i);
            if (j == std::string::npos) j = all.size();
            const std::string item = all.substr(i, j - i);
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::vector<std::string> apath;
            for (size_t a = 0, b; a <= eq; a = b + 1)
            {
                b = item.find('.', a);
                if (b == std::string::npos || b > eq) b = eq;
                apath.push_back(item.substr(a, b - a));
            }
            const std::string v = item.substr(eq + 1);
            CRef ref;
            if (v.find('.') != std::string::npos) ref.SetF32(float(std::atof(v.c_str())));
            else ref.SetS32(std::atoi(v.c_str()));
            std::printf("progress %s: %s\n", item.c_str(), FSetProgress(apath, ref) ? "set" : "not set");
        }
    }
    };
#ifdef __SWITCH__
    // (the CPU's loading profile while the first world loads and its scene builds; CaneSwitch::Shutdown ends it on the
    // error returns)
    CaneSwitch::BeginLoadingBoost();
#endif
    psw = PswUpdateTransition(std::move(psw));
    applySetProgress();
    if (!psw)
    {
        std::fprintf(stderr, "world %s did not load\n", go.world.c_str());
        return 1;
    }
    std::printf("world %s: %zu objects, %zu in the update table\n", go.world.c_str(), psw->aploAll.size(), psw->aupd.size());
    // The HUD starts inside the transition's 0x20E740, before post-load/player selection.
    // The auto-save writes the user's slot (as F5).
    g_pfnWriteSave = [] {
        const std::string path = cane::utf8(cane::userDirectory() / "Saves" / "save1");
        std::string err;
        const bool f = FWriteGameSave(path, err);
        if (!f) std::printf("save: %s\n", err.c_str());
        return f;
    };
    for (const std::string& p : go.aprintProgress)
    {
        std::vector<std::string> apath;
        for (size_t i = 0, j; i <= p.size(); i = j + 1)
        {
            j = p.find('.', i);
            if (j == std::string::npos) j = p.size();
            apath.push_back(p.substr(i, j - i));
        }
        const CRef ref = RefGetProgress(apath);
        if (ref.m_tagk == TAGK_S32) std::printf("progress %s = %d\n", p.c_str(), ref.m_n);
        else if (ref.m_tagk == TAGK_F32) std::printf("progress %s = %g\n", p.c_str(), ref.m_g);
        else if (ref.m_tagk == TAGK_Bool) std::printf("progress %s = %s\n", p.c_str(), ref.m_bool ? "#t" : "#f");
        else std::printf("progress %s: tag %d\n", p.c_str(), int(ref.m_tagk));
    }

    SCENE scene;
    std::vector<ALO*> apalo;
    RENDEROPTS opts;
    const std::string ramPath = go.loadmaps + "/w" + go.world + "/final.ram";
    CAMERA cam = CameraFromRam(ramPath, opts);
    const bool fWindow = !go.fHeadless;
    // (a headless run draws for --screenshot or SLY2N_SHOTEVERY's frames: the scene and GL then exist)
    const bool fDraws = fWindow || !go.screenshot.empty() || std::getenv("SLY2N_SHOTEVERY") != nullptr ||
                        std::getenv("SLY2N_RENDERALL") != nullptr;
    if (fDraws)
    {
        CBuildSceneFromSw(psw.get(), scene, &apalo);
        // the world's lights natively (lightsw.h: the lights' records from their options and transforms)
        if (UpdateWorldLightsFromSw(psw.get()) > 0) ApplyLighting(scene, WorldLights());
        else ApproximateBakeLighting(scene);
        if (!g_gl.FInit(go.width, go.height, fWindow, "Sly2-Release", error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        BuildSceneGpu(scene);
        if (fWindow)
        {
            // the PC options (display.h; <user-dir>/Saves/system_settings.dat, loaded by main)
            InstallDisplayCallbacks(g_gl.window);
            ApplyDisplaySettings(g_gl.window);
        }
    }
#ifdef __SWITCH__
    CaneSwitch::EndLoadingBoost();
#endif

    FILE* pfDump = go.frameDump.empty() ? nullptr : std::fopen(go.frameDump.c_str(), "w");
    // the original's NTSC frame time (its clock reads the EE counter: 1001/60000 s a frame), so clip events on frame
    // multiples cross on the reference's frames (test aid: SLY2N_FIXEDDT=s: that fixed step instead, e.g. 0.0166667)
    if (go.fFixedDt) g_dtFixedStep = DT_FRAME_NTSC;
    // (fixed-dt runs are not paced by the host clock: the music's clock follows the frames; SLY2N_SNDHOSTCLOCK keeps the host's)
    g_fSndFrameClock = go.fFixedDt && !std::getenv("SLY2N_SNDHOSTCLOCK");
    if (const char* sz = std::getenv("SLY2N_FIXEDDT"); sz && go.fFixedDt) g_dtFixedStep = std::strtof(sz, nullptr);
    cane::FramePacer pacer;   // (the frame-rate cap: platform/frame_pacer.h)
    if (fWindow)
    {
        // (Windows: the 1 ms timer, no power throttling, MMCSS "Games", the thread above normal as Sly1-Release's:
        // platform/game_sched.h)
        cane::sched::StartupGameScheduling();
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    }
    g_clock.tick = TickNow();
    bool& fFreeCam = g_fDebugFreeCam;   // (F1; also the debug menu's Display page)
    bool fF1Last = false;
    double msUpdate = 0, msRender = 0, msSwap = 0, msScene = 0;
    int cframeTimed = 0;
    auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    // the display rates above 60 Hz: 60 Hz steps, the frames drawn between them (frameinterp.h)
    FRAMEACCUM accum;
    static const char* s_szFrameInterp = std::getenv("SLY2N_FRAMEINTERP");   // (=fps: forced, a virtual display at that rate)
    for (int frame = 0; go.cframe < 0 || frame < go.cframe;)
    {
        const double t0 = now();
        if (fWindow && glfwWindowShouldClose(g_gl.window)) break;
        // (debug: SLY2N_HEAPCHECK=N: every N frames the heap's free lists consolidated (glibc checks their links), with a
        // stderr mark: heap corruption shows near the frame that caused it instead of at exit)
        if (static const int s_cHeapCheck = std::getenv("SLY2N_HEAPCHECK") ? std::atoi(std::getenv("SLY2N_HEAPCHECK")) : 0;
            s_cHeapCheck > 0 && frame % s_cHeapCheck == 0)
        {
            std::fprintf(stderr, "heapcheck: frame %d\n", frame);
            malloc_trim(0);
        }
        auto stepSim = [&](int frame) {
        g_iframeMain = frame;  // (debug prints: SLY2N_TRANSLOG)
        InvalidateSceneDrawnTests();   // (scenesw.h: the placement's FAloDrawn answers do not outlive a step)
        // orig 0x20E890: a pending world change
        // (a transition with a wipe is pending only once the wipe is out (0x22B220 -> WIPES_Loading): the queued
        // movies wait for it, as the original plays them inside the transition after its sound reset; played at
        // once they had the menu's music over them (U30: the episode menu's first visit, exit wipe 0x30A))
        if (FMoviesQueued() && !g_transition.fWipeReq)
        {
            if (std::getenv("SLY2N_MOVIEAUDIT"))
                std::fprintf(stderr, "movieaudit frame %d: queued movies, transition pending %d, live tracks %d\n", frame,
                             int(g_transition.fPending), CSpiopLiveTracks());
            if (g_transition.fPending && fWindow) StopSoundsForMovie();   // (the menu's music must not play over it)
            PlayQueuedMovies(fWindow);
        }   // orig 0x1B3A48's movies, before the world they lead to
        if (g_transition.fPending)
        {
#ifdef __SWITCH__
            CaneSwitch::LoadingBoost boost;   // (the CPU's loading profile while the world loads and its scene builds)
#endif
            psw = PswUpdateTransition(std::move(psw));
            scene = SCENE{};
            apalo.clear();
            ResetStepSnapshots();
            (void)FPcCharTakeSceneRebuild();
            if (fDraws)
            {
                CBuildSceneFromSw(psw.get(), scene, &apalo);
                ApproximateBakeLighting(scene);
                BuildSceneGpu(scene);
            }
        }
        else if (FPcCharTakeSceneRebuild() && fDraws)
        {
            // (PC addition, debug menu only: a guest character / outfit changed the meshes drawn, pccharacter.h; the
            // lookups into the old scene's glob sets dropped, as a world's unload does)
            void InvalidateSceneLookups();   // emitter.cpp
            InvalidateSceneLookups();
            scene = SCENE{};
            apalo.clear();
            ResetStepSnapshots();
            CBuildSceneFromSw(psw.get(), scene, &apalo);
            ApproximateBakeLighting(scene);
            BuildSceneGpu(scene);
        }
        // debug: SLY2N_RANDCOUNT=1: the rand draws and state at each frame's start
        if (static const bool s_fRc = std::getenv("SLY2N_RANDCOUNT") != nullptr; s_fRc)
        {
            extern uint64_t g_cRandLibc;
            std::printf("randcount frame %d: %llu draws, state 0x%08X\n", frame, (unsigned long long)g_cRandLibc, g_uRandState);
        }
        // debug: SLY2N_RANDRAM=file: the game's random state (the C library's rand, *(0x2C49D4) + 0x58) taken from a
        // reference RAM at the first frame (the original never seeds it: it starts at newlib's 1 and advances with
        // every rand call since boot, so a native warp's state differs from the reference's)
        // debug: SLY2N_RANDSTATE=n: the random state set at the first frame
        if (frame == 0)
            if (const char* sz = std::getenv("SLY2N_RANDSTATE")) g_uRandState = static_cast<uint32_t>(std::strtoul(sz, nullptr, 0));
        if (frame == 0)
            if (const char* sz = std::getenv("SLY2N_RANDRAM"))
            {
                RAM ram;
                if (ram.FLoad(sz)) g_uRandState = ram.U32(ram.U32(0x2C49D4) + 0x58);
            }
        if (frame == 0 && !go.slyStart.empty() && std::getenv("SLY2N_AI_START")) FImportAiStart(psw.get(), go.slyStart);
        if (frame == 0 && !go.slyStart.empty()) FImportSlyStart(psw.get(), go.slyStart, go.slyRef, go.vbBase);
        if (frame == 0 && !go.gangStart.empty()) FImportGangStart(psw.get(), go.gangStart, go.gangRef, go.vbBase);
        // debug: SLY2N_SAVEAT=frame:slot (1..3): the save into that slot at that frame, as the menu's Save Game
        // writes it (FWriteSaveSlot)
        if (static const char* s_sz = std::getenv("SLY2N_SAVEAT"); s_sz && std::atoi(s_sz) == frame && std::strchr(s_sz, ':'))
        {
            g_islotChosen = std::atoi(std::strchr(s_sz, ':') + 1) - 1;
            std::printf("debug save: slot %d %s\n", g_islotChosen + 1, FWriteSaveSlot(g_islotChosen) ? "written" : "not written");
        }
        // (a cutscene's skip runs the game alone: skipcut.h)
        UpdateJoy(&g_joy, fWindow && !fFreeCam && !FSkipCutFastForward() ? g_gl.window : nullptr, frame, go.apress);
        JoyBeginFrame();   // (the presses taken this frame: the scripts' (SCPT 0x42) before the HUD's and the menus')
        // (debugmenu.h, Debug keys only) the debug menu open: the game frozen, the step is the menu's
        if (FDebugMenuOpen())
        {
            UpdateDebugMenu(psw.get());
            g_clock.tick = TickNow();   // (the frozen time is no step for the clock)
            return;
        }
        // TODO(M3): 0x14BA38, 0x14D470 (camera)
        UpdateClock(&g_clock, false);
        if (pfDump) WriteFrameDump(pfDump, psw.get(), frame);
        WriteMembDump(psw.get(), frame);
        // debug: SLY2N_FRAMETICK=N: every N frames "tick f F t T po OID pos X Y Z" (tools/progression.sh: the
        // timeline its stall / softlock checks put the task lines on)
        if (static const int s_cTick = std::getenv("SLY2N_FRAMETICK") ? std::atoi(std::getenv("SLY2N_FRAMETICK")) : 0;
            s_cTick > 0 && frame % s_cTick == 0)
        {
            LO* ppo = PloPoCurrent(psw.get());
            const glm::vec3 p = ppo ? PosLoWorld(ppo) : glm::vec3(0.0f);
            std::printf("tick f %d t %.3f po %d pos %.0f %.0f %.0f\n", frame, g_clock.t, ppo ? ppo->oid : -1, p.x, p.y, p.z);
            // (SLY2N_CHAINTICK=1: the current player's chain segments (his tail, ears, ...): how many, how many their
            // chain drives (mode 16), how many hold a NaN)
            if (static const bool s_fChain = std::getenv("SLY2N_CHAINTICK") != nullptr; s_fChain && ppo && FCidDerivedFrom(ppo->Cid(), 0x30))
            {
                int cSeg = 0, c16 = 0, cNan = 0;
                for (const ALOCHAIN& c : static_cast<ALO*>(ppo)->achain)
                    for (const ALOCHAINE& e : c.ae)
                    {
                        if (!e.pxf) continue;
                        cSeg++;
                        c16 += e.pxf->mode == 16 ? 1 : 0;
                        cNan += (std::isnan(e.pos.x + e.pos.y + e.pos.z + e.v.x + e.v.y + e.v.z) ||
                                 std::isnan(e.pxf->mat[0].x + e.pxf->mat[1].y + e.pxf->mat[2].z)) ? 1 : 0;
                    }
                std::printf("chains po %d: segments %d chain-driven %d nan %d\n", ppo->oid, cSeg, c16, cNan);
            }
        }
        // debug: SLY2N_TASKDUMP=file:frame (each task's state and each job's, by oid, at the frame's start)
        if (const char* sz = std::getenv("SLY2N_TASKDUMP"))
        {
            // (file:@T: at the first frame whose game time reaches T, as the reference's RAM dump's clock: the
            // reference's dt comes from the EE tick (~1/59.94 s, and its first frames' longer steps))
            const char* pc = std::strrchr(sz, ':');
            static bool s_fDone = false;
            bool fNow = false;
            if (pc && pc[1] == '@')
            {
                const float tWant = static_cast<float>(std::atof(pc + 2));
                if (!s_fDone && g_clock.t + 0.5f * (g_clock.dt > 0.0f ? g_clock.dt : 1.0f / 60.0f) >= tWant) fNow = s_fDone = true;
            }
            else fNow = pc && std::atoi(pc + 1) == frame;
            if (fNow)
                if (std::FILE* pf = std::fopen(std::string(sz, pc - sz).c_str(), "w"))
                {
                    std::fprintf(pf, "cm %d object %d pos %.1f %.1f %.1f fov %.4f target %.4f\n", g_cm.nCur, g_cm.ploCur ? g_cm.ploCur->oid : -1, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.radFOV, g_cm.radFOVTarget);
                    if (LO* ppo = PloPoCurrent(psw.get()))
                    {
                        const glm::vec3 p = PosLoWorld(ppo);
                        std::fprintf(pf, "player %d pos %.1f %.1f %.1f\n", ppo->oid, p.x, p.y, p.z);
                    }
                    for (CID cid : {0x84, 0x85})
                        for (LO* plo : psw->mpcidaplo[cid])
                            if (cid == 0x85)
                            {
                                std::fprintf(pf, "task %d %d", plo->oid, static_cast<TASK*>(plo)->nState);
                                for (int16_t k : static_cast<TASK*>(plo)->prE0.akey) std::fprintf(pf, " %d", k);
                                std::fprintf(pf, "\n");
                            }
                            else std::fprintf(pf, "job %d\n", plo->oid);
                    std::fclose(pf);
                }
        }
        if (s_pswRtgt != psw.get() || s_serialRtgt != psw->serial)
        {
            s_pswRtgt = psw.get();
            s_serialRtgt = psw->serial;
            BuildRtgtOwners(s_pswRtgt);
        }
        g_pfnRtgtOwnerOfId = PvRtgtOwnerOfId;
        g_pfnRtgtTaken = TakeRtgt;
        g_pfnRtgtGiven = GiveRtgt;
        // the activation's viewpoint: the native camera's position (CM +0x90) and flags (CM +0x14, cm.h nCur)
        g_actview.posCm = g_cm.pos;
        g_actview.grfCm = static_cast<uint32_t>(g_cm.nCur);
        // (--camera-ram: the picture's camera is the reference's, so is the activation's viewpoint (CM +0x90 / +0x14 of
        // that RAM); else the objects the reference camera activates are missing from the native picture)
        // Only for the last frames of the run (SLY2N_CAMRAMACT=N, default 60; -1 = every frame): from the start, the
        // world near the native player is deactivated and a long route (intro.route) fails at its first climb.
        static const int s_cframeCamAct = std::getenv("SLY2N_CAMRAMACT") ? std::atoi(std::getenv("SLY2N_CAMRAMACT")) : 60;
        if (!go.cameraRam.empty() && (s_cframeCamAct < 0 || go.cframe < 0 || frame + s_cframeCamAct >= go.cframe))
        {
            static RAM s_ramCam;
            static const bool s_fRam = s_ramCam.FLoad(go.cameraRam);
            if (s_fRam)
            {
                g_actview.posCm = glm::vec3(s_ramCam.F32(0x2DDF40 + 0x90), s_ramCam.F32(0x2DDF40 + 0x94), s_ramCam.F32(0x2DDF40 + 0x98));
                g_actview.grfCm = static_cast<uint32_t>(s_ramCam.S32(0x2DDF40 + 0x14));
            }
        }
        // debug (the harness without the camera's differences): SLY2N_ACTVIEW=x,y,z,grf
        static const char* s_szActview = std::getenv("SLY2N_ACTVIEW");
        if (s_szActview)
        {
            float x = 0, y = 0, z = 0;
            unsigned grf = 0;
            if (std::sscanf(s_szActview, "%f,%f,%f,%u", &x, &y, &z, &grf) == 4)
            {
                g_actview.posCm = glm::vec3(x, y, z);
                g_actview.grfCm = grf;
            }
        }
        // (debug: SLY2N_DROPCOINS="X Y Z@tT,N[,P]" drops N coins there at game time T (again every P s when given), as a
        // breakable's item 10 0x154E10)
        if (static const char* s_szDrop = std::getenv("SLY2N_DROPCOINS"); s_szDrop)
        {
            static bool s_fDone = false;
            static float s_tNext = -1.0f;
            float x = 0, y = 0, z = 0, t = 0, dtRepeat = 0;
            int n = 1;
            const bool fPlayer = s_szDrop[0] == 'P';  // "P@tT,N": above the current player
            const bool fParsed = fPlayer ? std::sscanf(s_szDrop, "P@t%f,%d,%f", &t, &n, &dtRepeat) >= 1
                                         : std::sscanf(s_szDrop, "%f %f %f@t%f,%d,%f", &x, &y, &z, &t, &n, &dtRepeat) >= 4;
            if (s_tNext < 0.0f) s_tNext = t;
            if (!s_fDone && fParsed && s_tNext <= g_clock.t)
            {
                if (dtRepeat > 0.0f) s_tNext = g_clock.t + dtRepeat;
                else s_fDone = true;
                if (LO* plo = fPlayer ? PloPoCurrent(psw.get()) : nullptr; plo && FCidDerivedFrom(plo->Cid(), 0x30))
                {
                    const glm::vec3 q = PosAloCenter(static_cast<ALO*>(plo));
                    x = q.x + 150.0f; y = q.y; z = q.z + 50.0f;
                }
                DropPickups(glm::vec3(x, y, z), n, 0, nullptr);
            }
        }
        // (debug: SLY2N_PTELEPORT="X Y Z@tT[,H];..." puts the current player there at game time T, heading H)
        if (static const char* s_szPTele = std::getenv("SLY2N_PTELEPORT"); s_szPTele)
        {
            static std::vector<const char*> s_aqDone;
            for (const char* q = s_szPTele; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                float x = 0, y = 0, z = 0, t = 0, h = 0;
                const int c = std::sscanf(q, "%f %f %f@t%f,%f", &x, &y, &z, &t, &h);
                if (c < 4 || g_clock.t < t || std::find(s_aqDone.begin(), s_aqDone.end(), q) != s_aqDone.end()) continue;
                LO* plo = PloPoCurrent(psw.get());
                STEP* pstep = plo ? PstepFromLo(plo) : nullptr;
                if (!pstep) continue;
                s_aqDone.push_back(q);
                // (a ledge hang (the jump's state 17) holds the actor at its ledge: let go first)
                if (ACTOR* pa = PactorFromLo(plo); pa && pa->jump.state == 17) SetJumpAnimState(&pa->jump, 4, nullptr);
                SetStepPos(pstep, glm::vec3(x, y, z) - glm::vec3(pstep->vecBase));
                glm::mat3 r(1.0f);
                r[0] = glm::vec3(std::cos(h), std::sin(h), 0.0f);
                r[1] = glm::vec3(-std::sin(h), std::cos(h), 0.0f);
                SetStepRotation(pstep, r);
                SetAloVelocityTo(pstep->palo, glm::vec3(0.0f));
                CutCm(&g_cm);
                std::printf("ptele: %d to %.0f %.0f %.0f at t %.2f\n", plo->oid, x, y, z, g_clock.t);
            }
        }
        // (debug: SLY2N_FINDN=n@frame: the objects' option values and ASEG event arguments equal to n)
        if (static const char* s_szFindN = std::getenv("SLY2N_FINDN"); s_szFindN && std::strchr(s_szFindN, '@') &&
            frame == std::atoi(std::strchr(s_szFindN, '@') + 1))
        {
            const int n = std::atoi(s_szFindN);
            std::function<bool(const OPTVAL&)> f = [&](const OPTVAL& v) {
                if (v.n == n) return true;
                for (const OPTVAL& w : v.alist) if (f(w)) return true;
                return false;
            };
            for (const auto& up : psw->aploAll)
            {
                for (const OPT& o : up->aopt)
                    for (const OPTVAL& v : o.aval)
                        if (f(v)) std::printf("findn: %d (cid 0x%X) option %d\n", up->oid, up->Cid(), o.eopid);
                if (ASEG* pa = dynamic_cast<ASEG*>(up.get()))
                    if (ASEGD* pd = PasegdOf(pa))
                        for (const auto& aev : pd->aaev)
                            for (const ASEGEV& e : aev)
                                for (int x : e.an)
                                    if (x == n) std::printf("findn: aseg %d event kind %d\n", up->oid, e.kind);
            }
        }
        // (debug: SLY2N_XFDUMP="oid,oid@f1,f2,..": the objects' records' world matrices at those frames)
        if (static const char* s_sz = std::getenv("SLY2N_XFDUMP"); s_sz && std::strchr(s_sz, '@'))
        {
            bool fAt = false;
            for (const char* q = std::strchr(s_sz, '@') + 1; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                fAt = fAt || std::atoi(q) == frame;
            if (fAt)
                for (const auto& up : psw->aploAll)
                {
                    if (!FCidDerivedFrom(up->Cid(), 0x30)) continue;
                    const std::string so = std::to_string(up->oid);
                    const char* pAt = std::strchr(s_sz, '@');
                    bool f = false;
                    for (const char* q = s_sz; q < pAt; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : pAt)
                        f = f || std::atoi(q) == up->oid;
                    if (!f || !up->fInWorld) continue;
                    const ALO* pa = static_cast<const ALO*>(up.get());
                    for (size_t i = 0; i < pa->axf.size(); ++i)
                    {
                        const glm::mat4& m = pa->axf[i]->matWorld;
                        std::printf("xfdump %d %d %p xf %zu: x %.3f %.3f %.3f pos %.1f %.1f %.1f mode %d\n", frame, up->oid, (const void*)pa, i, m[0].x, m[0].y,
                                    m[0].z, m[3].x, m[3].y, m[3].z, pa->axf[i]->mode);
                    }
                }
        }
        // (debug: SLY2N_LOOPTS="oid,oid@frame": the objects' loaded options)
        if (static const char* s_sz = std::getenv("SLY2N_LOOPTS"); s_sz && std::strchr(s_sz, '@') && frame == std::atoi(std::strchr(s_sz, '@') + 1))
            for (const auto& up : psw->aploAll)
            {
                if (!std::strstr(s_sz, (std::to_string(up->oid) + ",").c_str()) && !std::strstr(s_sz, (std::to_string(up->oid) + "@").c_str())) continue;
                for (const OPT& o : up->aopt)
                {
                    std::printf("loopts %d (cid 0x%X): eopid %d", up->oid, up->Cid(), o.eopid);
                    for (const OPTVAL& v : o.aval) std::printf(" [n %d g %g]", v.n, v.ag[0]);
                    std::printf("\n");
                }
            }
        // (debug: SLY2N_ASEGADUMP=frame lists the live ASEGAs: their ASEG, object, time, speed, flags)
        if (static const char* s_sz = std::getenv("SLY2N_ASEGADUMP"); s_sz && frame == std::atoi(s_sz))
            for (const ASEGA* pa : g_apasega)
                std::printf("asegadump aseg %d alo %d %p t %.3f svt %.3f grfapl 0x%X\n", pa->paseg ? pa->paseg->oid : -1, pa->palo ? pa->palo->oid : -1, (void*)pa->palo, pa->t,
                            pa->svt, pa->grfapl);
        if (static const char* s_sz = std::getenv("SLY2N_ASEGADUMP"); s_sz && frame == std::atoi(s_sz))
            for (const SMA* ps : g_apsma)
                std::printf("smadump sm %d alo %d\n", ps->psm ? ps->psm->oid : -1, ps->palo ? ps->palo->oid : -1);
        // (debug: SLY2N_BIGGLOBS="radius@frame": the globs of a radius above that, by object)
        if (static const char* s_szFog = std::getenv("SLY2N_FOGPRINT"); s_szFog && frame % std::max(1, std::atoi(s_szFog)) == 0)   // (debug: the camera's fog every N frames)
            std::printf("fog frame %d t %.2f near %g far %g max %g rgba %08X\n", frame, g_clock.t, g_cm.sNearFog, g_cm.sFarFog, g_cm.uFogMax, g_cm.rgbaFog);
        if (static const char* s_szBig = std::getenv("SLY2N_BIGGLOBS"); s_szBig && std::strchr(s_szBig, '@') &&
            frame == std::atoi(std::strchr(s_szBig, '@') + 1))
            for (const auto& up : psw->aploAll)
            {
                LO* plo = up.get();
                if (!FCidDerivedFrom(plo->Cid(), 0x30)) continue;
                ALO* palo = static_cast<ALO*>(plo);
                if (!palo->pglobset) continue;
                for (size_t i = 0; i < palo->pglobset->aglob.size(); ++i)
                {
                    const GLOB& g = palo->pglobset->aglob[i];
                    if (g.sRadius < std::atof(s_szBig)) continue;
                    std::printf("bigglob %d cid 0x%X inworld %d glob %zu rp %d meshk %d grf 0x%X grfGroup 0x%X hidden %d zorder %g fog %g rtck %d c %.0f %.0f %.0f r %.0f vtx %zu sub %zu\n",
                                plo->oid, plo->Cid(), plo->fInWorld, i, g.rp, g.meshk, g.grfMesh, g.grfGroup, g.fHidden, g.gZOrder, g.uFog, g.rtck,
                                g.posCenter.x, g.posCenter.y, g.posCenter.z, g.sRadius, g.avtx.size(), g.asubglob.size());
                }
            }
        // (debug: SLY2N_LOSTATE="oid,oid@frame": every copy's removed / in-world flags and its parents')
        if (static const char* s_sz = std::getenv("SLY2N_LOSTATE"); s_sz && std::strchr(s_sz, '@') && frame == std::atoi(std::strchr(s_sz, '@') + 1))
            for (const auto& up : psw->aploAll)
            {
                const std::string so = "," + std::to_string(up->oid) + ",";
                if (!std::strstr((std::string(",") + s_sz).c_str(), so.c_str()) &&
                    !std::strstr((std::string(",") + s_sz).c_str(), ("," + std::to_string(up->oid) + "@").c_str())) continue;
                std::printf("lostate %d %p cid 0x%X removed %d inworld %d st %d clone of %p alias %d channel %d ns %d:", up->oid, (void*)up.get(), up->Cid(), up->fRemoved, up->fInWorld, up->st, (void*)up->ploCloneOf, up->oidInstance, up->ichannel, up->ploNs ? up->ploNs->oid : -1);
                for (const LO* p = up->paloParent; p; p = p->paloParent)
                    std::printf(" <- %d (removed %d inworld %d)", p->oid, p->fRemoved, p->fInWorld);
                std::printf("\n");
            }
        // (debug: SLY2N_CIDPOS="cid,cid,...@frame" lists the objects of those classes (and derived) with their places)
        // debug: SLY2N_PINLOCAL=oid:x,y,z@f0-f1: that object's position record's local position set at those frames
        // (SLY2N_PINWORLD: the same with a world position, converted into the record's parent frame)
        static const bool s_fPinWorld = std::getenv("SLY2N_PINWORLD") != nullptr;
        if (static const char* s_szPin = s_fPinWorld ? std::getenv("SLY2N_PINWORLD") : std::getenv("SLY2N_PINLOCAL"); s_szPin && std::strchr(s_szPin, '@'))
        {
            int oid = 0, f0 = 0, f1 = 0;
            float x = 0, y = 0, z = 0, rad = 0;
            // (optional 4th value: the rotation record's world heading about z, oid:x,y,z,rad@f0-f1)
            const bool fRad = std::sscanf(s_szPin, "%d:%f,%f,%f,%f@%d-%d", &oid, &x, &y, &z, &rad, &f0, &f1) == 7;
            if ((fRad || std::sscanf(s_szPin, "%d:%f,%f,%f@%d-%d", &oid, &x, &y, &z, &f0, &f1) == 6) && frame >= f0 && frame <= f1)
            {
                // (of several instances in the world with that oid: the nearest to the point)
                ALO* palo = nullptr;
                float dBest = 3.4e38f;
                for (const auto& up : psw->aploAll)
                    if (up->oid == oid && FCidDerivedFrom(up->Cid(), 0x30) && (up->fInWorld || !palo))
                    {
                        const float d = glm::length(PosAloCenter(static_cast<ALO*>(up.get())) - glm::vec3(x, y, z));
                        if (up->fInWorld && d < dBest) { dBest = d; palo = static_cast<ALO*>(up.get()); }
                        else if (!palo) palo = static_cast<ALO*>(up.get());
                    }
                if (palo)
                    if (XF* pxf = palo->apxfRef[0])
                    {
                        const glm::vec3 pos = s_fPinWorld ? PosConvertXfs(nullptr, pxf->pxfRef, glm::vec3(x, y, z)) : glm::vec3(x, y, z);
                        pxf->mat[3] = glm::vec4(pos, 1.0f);
                        if (XF* pxr = palo->apxfRef[1]; fRad && pxr)
                        {
                            const glm::mat3 m = MatConvertXfs(nullptr, pxr->pxfRef, cmm::BuildRotateMatrixAxis(rad, glm::vec3(0.0f, 0.0f, 1.0f)));
                            for (int i = 0; i < 3; ++i) pxr->mat[i] = glm::vec4(m[i], 0.0f);
                        }
                    }
            }
        }
        // debug: SLY2N_RECONPRINT=1: the recon camera policy's pan / tilt every 30 frames
        if (static const bool s_fRp = std::getenv("SLY2N_RECONPRINT") != nullptr; s_fRp && frame % 30 == 0)
            std::printf("recon f %d pan %.4f tilt %.4f cm %.0f %.0f %.0f\n", frame, g_cmpolicies.cprecon.radPan, g_cmpolicies.cprecon.radTilt, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z);
        // debug: SLY2N_SETCOUNT="h=N@F;p=N@F": the current player's health (the damage counter) / gadget power set to N
        // at frame F (as the reference's SLY2_POKE of 0x3D4AB0 / 2E1E40@+12FC@)
        if (static const char* s_szCount = std::getenv("SLY2N_SETCOUNT"); s_szCount)
            for (const char* q = s_szCount; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                char k = 0;
                int n = 0, f = -1;
                if (std::sscanf(q, "%c=%d@%d", &k, &n, &f) != 3 || f != frame) continue;
                ACTOR* pa = PactorFromLo(PloPoCurrent(psw.get()));
                if (!pa) continue;
                if (k == 'h' && DmgCounterOf(pa)) *DmgCounterOf(pa) = n;
                if (k == 'p') SetActorPower(pa, n);
                std::printf("setcount frame %d %c = %d\n", frame, k, n);
            }
        // debug (tests): SLY2N_TASKREQ="oid=state@F;...": the task's state requested at frame F (RequestTaskState, so its
        // events and the job's bookkeeping run as when the game asks for it), e.g. a job's other parts done
        {
            void UpdateAutoplay(SW* psw, int frame);   // (debug harness: SLY2N_AUTOPLAY, autoplay.cpp)
            UpdateAutoplay(psw.get(), frame);
        }
        if (static const char* s_szReq = std::getenv("SLY2N_TASKREQ"); s_szReq)
            for (const char* q = s_szReq; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                int oid = 0, n = 0, f = -1;
                if (std::sscanf(q, "%d=%d@%d", &oid, &n, &f) != 3 || f != frame) continue;
                if (FRequestTaskStateOid(oid, n)) { std::printf("taskreq frame %d task %d = %d\n", frame, oid, n); }
            }
        // debug: SLY2N_TRACKALL=oid:N: every in-world instance of that oid (clones / proxies too) every N frames
        if (static const char* s_szAll = std::getenv("SLY2N_TRACKALL"); s_szAll && std::strchr(s_szAll, ':') &&
            frame % std::max(1, std::atoi(std::strchr(s_szAll, ':') + 1)) == 0)
            for (const auto& up : psw->aploAll)
                if (up->oid == std::atoi(s_szAll) && up->fInWorld && FCidDerivedFrom(up->Cid(), 0x30))
                {
                    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(up.get()));
                    const ALO* pa = static_cast<ALO*>(up.get());
                    const glm::vec3 l1 = pa->axf.size() > 1 ? glm::vec3(pa->axf[1]->mat[3]) : glm::vec3(0.0f);
                    std::printf("trackall %d %p t %.3f pos %.1f %.1f %.1f parent %d xf1 local %.1f %.1f %.1f mode %d\n", up->oid, (void*)up.get(), g_clock.t, pos.x, pos.y, pos.z,
                                up->paloParent ? up->paloParent->oid : -1, l1.x, l1.y, l1.z, pa->axf.size() > 1 ? int(pa->axf[1]->mode) : -1);
                }
        // debug: SLY2N_CIDTRACE=cid:N: every in-world object derived from that class (hex) every N frames (oid, instance, centre)
        if (static const char* s_szCid = std::getenv("SLY2N_CIDTRACE"); s_szCid && std::strchr(s_szCid, ':') &&
            frame % std::max(1, std::atoi(std::strchr(s_szCid, ':') + 1)) == 0)
            for (const auto& up : psw->aploAll)
                if (up->fInWorld && FCidDerivedFrom(up->Cid(), int(std::strtol(s_szCid, nullptr, 16))) && FCidDerivedFrom(up->Cid(), 0x30))
                {
                    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(up.get()));
                    std::printf("cidtrace %d %p cid 0x%X t %.3f pos %.1f %.1f %.1f\n", up->oid, (void*)up.get(), unsigned(up->Cid()), g_clock.t, pos.x, pos.y, pos.z);
                    if (std::getenv("SLY2N_CIDTRACEXF"))
                        if (const XF* pxf = PxfPos(static_cast<ALO*>(up.get())))
                        {
                            const glm::mat4& m = pxf->matWorld;
                            std::printf("   xf %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.1f %.1f %.1f\n", m[0][0], m[0][1], m[0][2], m[1][0],
                                        m[1][1], m[1][2], m[2][0], m[2][1], m[2][2], m[3][0], m[3][1], m[3][2]);
                        }
                }
                // (a class that is no ALO, e.g. the uses 0x23: its world position, in world or not)
                else if (FCidDerivedFrom(up->Cid(), int(std::strtol(s_szCid, nullptr, 16))) && !FCidDerivedFrom(up->Cid(), 0x30))
                {
                    const glm::vec3 pos = PosLoWorld(up.get());
                    std::printf("cidtrace %d %p cid 0x%X t %.3f pos %.1f %.1f %.1f inworld %d\n", up->oid, (void*)up.get(), unsigned(up->Cid()), g_clock.t,
                                pos.x, pos.y, pos.z, int(up->fInWorld));
                }
        // debug: SLY2N_NANOID=oid: the first frame an XF under that object (its own or its parts') has a NaN local or
        // world matrix: each such record (owner, index, kind, mode, state, local translation)
        if (static const char* s_szNan = std::getenv("SLY2N_NANOID"); s_szNan)
        {
            static int s_cPrinted = 0;
            const int oidNan = std::atoi(s_szNan);
            for (const auto& up : psw->aploAll)
            {
                if (s_cPrinted > 40) break;
                bool fUnder = false;
                for (const LO* plo = up.get(); plo && !fUnder; plo = plo->paloParent) fUnder = plo->oid == oidNan;
                if (!fUnder || !FCidDerivedFrom(up->Cid(), 0x30)) continue;
                const ALO* palo = static_cast<const ALO*>(up.get());
                for (size_t i = 0; i < palo->axf.size(); ++i)
                {
                    const XF* pxf = palo->axf[i].get();
                    auto fNan = [](const glm::mat4& m) { for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) if (std::isnan(m[c][r])) return true; return false; };
                    if (!pxf || (!fNan(pxf->mat) && !fNan(pxf->matWorld))) continue;
                    ++s_cPrinted;
                    std::printf("nanoid frame %d t %.3f: oid %d cid 0x%X xf %zu kind %d mode %d state %d local nan %d world nan %d ref %d t %.1f %.1f %.1f g %.3f vel %.1f %.1f %.1f aseg0 %.2f %.2f %.2f %.2f\n", frame, g_clock.t, palo->oid, unsigned(palo->Cid()), i, pxf->kind, pxf->mode,
                                pxf->state, fNan(pxf->mat), fNan(pxf->matWorld), pxf->pxfRef && pxf->pxfRef->palo ? pxf->pxfRef->palo->oid : -1, pxf->mat[3][0], pxf->mat[3][1], pxf->mat[3][2], pxf->G(),
                                pxf->vel.x, pxf->vel.y, pxf->vel.z, pxf->aseg[0].x, pxf->aseg[0].y, pxf->aseg[0].z, pxf->aseg[0].w);
                }
            }
        }
        // debug: SLY2N_SLYTRACE=file: Sly's interaction (+0x4C8 kind, +0x4CC state), camera zone +0x1024, position record,
        // +0x190 (vecVel) every frame
        if (static std::FILE* s_pfSt = std::getenv("SLY2N_SLYTRACE") ? std::fopen(std::getenv("SLY2N_SLYTRACE"), "w") : nullptr; s_pfSt)
            if (SLY* ps = PslyGlobal())
            {
                const XF* pxf = PxfPos(ps->palo);
                const glm::vec3 q = pxf ? glm::vec3(pxf->matWorld[3]) : glm::vec3(0.0f);
                std::fprintf(s_pfSt, "%d,%.4f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", frame, g_clock.t, ps->n4C8, ps->nSub4CC,
                             ps->plo1024 ? ps->plo1024->oid : 0, q.x, q.y, q.z, ps->vecVel.x, ps->vecVel.y, ps->vecVel.z);
                std::fflush(s_pfSt);
            }
        // debug: SLY2N_MURRAYTRACE=file: Murray's states (+0x13D0, jump +0x2DC, +0x1810, +0x18C8), his carry fields
        // (+0x13EC / +0x13F0 / +0x1448 / +0x144C / +0x1450 / +0x10DC) and his position record, every frame
        if (static std::FILE* s_pfMt = std::getenv("SLY2N_MURRAYTRACE") ? std::fopen(std::getenv("SLY2N_MURRAYTRACE"), "w") : nullptr; s_pfMt)
            if (MURRAY* pm = PmurrayFromLo(PloFindSwObjectByOid(psw.get(), 9)))
            {
                const XF* pxf = PxfPos(pm->palo);
                const glm::vec3 q = pxf ? glm::vec3(pxf->matWorld[3]) : glm::vec3(0.0f);
                auto oid = [](const LO* plo) { return plo ? plo->oid : -1; };
                std::fprintf(s_pfMt, "%d,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.5f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%d,%d,%d,%.6f\n", frame,
                             g_clock.t, pm->state, pm->jump.state, pm->n1810, -1, oid(pm->plo13EC), oid(pm->plo13F0), pm->n1448,
                             pm->n144C, pm->n1450, oid(pm->plo10DC), q.x, q.y, q.z, pm->radHeading, g_cm.mat[0].x, g_cm.mat[0].y,
                             g_cm.mat[0].z, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, pm->f1818, pm->f181C, pm->f1820,
                             pm->pasega ? pm->pasega->t : -1.0f);
                std::fflush(s_pfMt);
            }
        // debug (test harness): SLY2N_FOLLOWOID="A:B:dx dy dz@F": from frame F, object A put at object B's position plus
        // (dx, dy, dz) every frame (e.g. the RC chopper 50723 held over its jeep to test the bombs and the job's end)
        if (static const char* s_szFol = std::getenv("SLY2N_FOLLOWOID"); s_szFol)
        {
            int oidA = -1, oidB = -1, f0 = 0;
            float dx = 0, dy = 0, dz = 0;
            if (std::sscanf(s_szFol, "%d:%d:%f %f %f@%d", &oidA, &oidB, &dx, &dy, &dz, &f0) == 6 && frame >= f0)
            {
                LO* pa = PloFindSwObjectByOid(psw.get(), oidA);
                LO* pb = PloFindSwObjectByOid(psw.get(), oidB);
                if (pa && pb && FCidDerivedFrom(pa->Cid(), 0x30) && FCidDerivedFrom(pb->Cid(), 0x30))
                {
                    SetAloPos(static_cast<ALO*>(pa), PosAloCenter(static_cast<ALO*>(pb)) + glm::vec3(dx, dy, dz));
                    UpdateAloXfWorlds(static_cast<ALO*>(pa));
                }
            }
        }
        // debug (test harness): SLY2N_SETPOS="F:OID[/K]=X Y Z;..." at frame F the K-th in-world instance (default 0) of OID
        // put at (X, Y, Z) through its position setter (vt slot 29) with its velocities zeroed (slots 35 / 36): the
        // reference's SLY2_SETPOS (host/devwarp.cpp) does the same through the original's slots
        if (static const char* s_szSet = std::getenv("SLY2N_SETPOS"); s_szSet)
            for (const char* q = s_szSet; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr)
            {
                int f = -1, oid = -1, k = 0;
                float x = 0, y = 0, z = 0;
                const bool fK = std::sscanf(q, "%d:%d/%d=%f %f %f", &f, &oid, &k, &x, &y, &z) == 6;
                if (!fK && std::sscanf(q, "%d:%d=%f %f %f", &f, &oid, &x, &y, &z) != 5) continue;
                if (f != frame) continue;
                int i = 0;
                for (const auto& up : psw->aploAll)
                    if (LO* plo = up.get(); plo->oid == oid && plo->fInWorld && FCidDerivedFrom(plo->Cid(), 0x30) && i++ == k)
                    {
                        ALO* palo = static_cast<ALO*>(plo);
                        // (the class's own slot 29 when it has a port, e.g. the actors' 0x143CF8 clears the safe
                        // ground's count as the reference's slot 29 does; else the ALO's)
                        if (auto pfn = reinterpret_cast<void (*)(ALO*, const glm::vec3&)>(PvNativeVt(plo, 29)))
                            pfn(palo, glm::vec3(x, y, z));
                        else SetAloPos(palo, glm::vec3(x, y, z));
                        SetAloVelocityTo(palo, glm::vec3(0.0f));
                        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
                        std::printf("setpos: frame %d oid %d/%d at %.2f %.2f %.2f\n", frame, oid, k, x, y, z);
                        break;
                    }
            }
        // debug: SLY2N_TRACKOID=oid[,oid..]:N: those objects' world position, state and heading every N frames
        if (static const char* s_szTrack = std::getenv("SLY2N_TRACKOID"); s_szTrack && std::strchr(s_szTrack, ':') &&
            frame % std::max(1, std::atoi(std::strchr(s_szTrack, ':') + 1)) == 0)
            for (const char* q = s_szTrack; q && *q && *q != ':'; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                for (const auto& upTrack : psw->aploAll)
                    if (LO* plo = upTrack.get(); plo->oid == std::atoi(q) && FCidDerivedFrom(plo->Cid(), 0x03) && !FCidDerivedFrom(plo->Cid(), 0x30))
                    {
                        // (a located object (LOX): its world position, e.g. the RC tank's aim point 72932)
                        const glm::vec3 pos = glm::vec3(MatWorldLox(plo)[3]);
                        std::printf("tracklox %d f %d t %.3f pos %.2f %.2f %.2f\n", plo->oid, frame, g_clock.t, pos.x, pos.y, pos.z);
                    }
                    else if (plo->oid == std::atoi(q) && plo->fInWorld && FCidDerivedFrom(plo->Cid(), 0x30))
                    {
                        const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(plo));
                        XF* pxf = static_cast<ALO*>(plo)->apxfRef[0];  // (+0x58)
                        const glm::vec3 posL = pxf ? glm::vec3(pxf->mat[3]) : glm::vec3(0.0f);
                        STEPPHYS* pst = PstepPhysOf(static_cast<ALO*>(plo));
                        if (frame == 60 && pxf)
                        {
                            const glm::mat4 m = pxf->matWorld * glm::inverse(pxf->mat);
                            std::printf("track %d parent rows %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f pos %.2f %.2f %.2f\n", plo->oid,
                                        m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2], m[3][0], m[3][1], m[3][2]);
                        }
                        const GUARD* pgTrack = PguardFromLo(plo);
                        std::printf("track %d f %d t %.3f st %d pos %.1f %.1f %.1f local %.2f %.2f %.2f head %.2f grf %llx 32c %d mode %d inst %d\n", plo->oid, frame, g_clock.t, plo->nState,
                                    pos.x, pos.y, pos.z, posL.x, posL.y, posL.z, pst ? pst->radHeading : 0.0f,
                                    pgTrack ? static_cast<unsigned long long>(pgTrack->grfState) : 0ull, pgTrack ? pgTrack->f32C : 0, pgTrack ? pgTrack->nMode : -9, plo->oidInstance);
                        if (pgTrack && std::getenv("SLY2N_TRACKJOBS"))   // (the job stack, each with its child chain)
                        {
                            std::printf("  jobs %d:", plo->oid);
                            for (const auto& pj : pgTrack->stack.ajob)
                            {
                                std::printf(" [");
                                for (const AIJOB* q = pj.get(); q; q = q->pchild) std::printf(" %X", unsigned(q->cid));
                                std::printf(" ]");
                            }
                            std::printf("\n");

                        }
                        if (pxf && std::getenv("SLY2N_TRACKXF"))  // the +0x58 record's world, its parent record's world / local
                        {
                            const XF* pr = pxf->pxfRef;
                            std::printf("trackxf %d f %d w %.2f %.2f %.2f rw %.2f %.2f %.2f rl %.2f %.2f %.2f rr %d vv %.4f %.4f %.4f\n", plo->oid, frame,
                                        pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z, pr ? pr->matWorld[3].x : 0.0f,
                                        pr ? pr->matWorld[3].y : 0.0f, pr ? pr->matWorld[3].z : 0.0f, pr ? pr->mat[3].x : 0.0f,
                                        pr ? pr->mat[3].y : 0.0f, pr ? pr->mat[3].z : 0.0f, pr && pr->pxfRef && pr->pxfRef->palo ? pr->pxfRef->palo->oid : -1, pst ? pst->vecVel.x : 0.0f, pst ? pst->vecVel.y : 0.0f, pst ? pst->vecVel.z : 0.0f);
                            if (const XF* q = static_cast<ALO*>(plo)->apxfRef[1])  // +0x5C: the rotation record's world rows
                                std::printf("trackrot %d f %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f w %.4f %.4f %.4f mode %d v %.3f %.3f %.3f mode %d\n", plo->oid, frame, q->matWorld[0].x,
                                            q->matWorld[0].y, q->matWorld[0].z, q->matWorld[1].x, q->matWorld[1].y, q->matWorld[1].z,
                                            q->matWorld[2].x, q->matWorld[2].y, q->matWorld[2].z, q->vel.x, q->vel.y, q->vel.z, q->mode, pxf->vel.x, pxf->vel.y, pxf->vel.z, pxf->mode);
                        }
                    }
        // debug: SLY2N_SLEEPAT=oid@frame (oid 0: all): every in-world guard of that oid gets reaction 8 (asleep, job 0xAF) as from a
        // sleep dart (0x176BE8's 0x80000 path at +0xE88 <= 0)
        if (static const char* s_szSleep = std::getenv("SLY2N_SLEEPAT"); s_szSleep && std::strchr(s_szSleep, '@') &&
            frame == std::atoi(std::strchr(s_szSleep, '@') + 1))
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); (plo->oid == std::atoi(s_szSleep) || std::atoi(s_szSleep) == 0) && plo->fInWorld)
                    if (GUARD* pg = PguardFromLo(plo))
                        if (auto* pr = dynamic_cast<JOBREACT*>(PjobFind(pg, 0x9C)))
                        {
                            AIMSG m;
                            m.k = 8;
                            pr->React(m);
                        }
        // debug: SLY2N_SPINTRACE=N: N frames after each world load (and every N after that, SLY2N_SPINEVERY=M), every
        // in-world non-actor object whose object record (+0x60) turned since the last frame: its world angular velocity
        // (the axis of R(t) R(t-1)^T over dt, rad/s), oid / instance / class / place (tools/spintripregress.py, R55)
        if (static const char* s_szSpin = std::getenv("SLY2N_SPINTRACE"); s_szSpin && psw)
        {
            static std::unordered_map<const ALO*, glm::mat3> s_mpmat;
            static int s_cLoads = -1, s_frameLoad = 0;
            extern int g_cTransitionLoads;
            if (s_cLoads != g_cTransitionLoads)
            {
                s_cLoads = g_cTransitionLoads;
                s_frameLoad = frame;
                s_mpmat.clear();
            }
            const int rel = frame - s_frameLoad, n = std::max(1, std::atoi(s_szSpin));
            static const int s_every = std::getenv("SLY2N_SPINEVERY") ? std::atoi(std::getenv("SLY2N_SPINEVERY")) : 0;
            const bool fPrint = rel == n || (s_every > 0 && rel > n && (rel - n) % s_every == 0);
            const float dt = g_clock.dt > 0.0f ? g_clock.dt : 1.0f / 60.0f;
            for (const auto& up : psw->aploAll)
            {
                LO* plo = up.get();
                if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
                bool fActorPart = false;  // (an actor or a part of one: guards' bodies turn with their AI)
                for (LO* q = plo; q && !fActorPart; q = q->paloParent) fActorPart = PactorFromLo(q) || FCidDerivedFrom(q->Cid(), 0x38);
                if (fActorPart) continue;
                const XF* pxo = PxfObj(static_cast<ALO*>(plo));
                if (!pxo) continue;
                glm::mat3 m(glm::vec3(pxo->matWorld[0]), glm::vec3(pxo->matWorld[1]), glm::vec3(pxo->matWorld[2]));
                for (int i = 0; i < 3; ++i)
                    if (const float l = glm::length(m[i]); l > 1e-6f) m[i] /= l;
                const ALO* palo = static_cast<const ALO*>(plo);
                auto it = s_mpmat.find(palo);
                if (fPrint && it != s_mpmat.end())
                {
                    const glm::mat3 d = m * glm::transpose(it->second);
                    const float c = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
                    const float ang = std::acos(c);
                    glm::vec3 ax(d[1][2] - d[2][1], d[2][0] - d[0][2], d[0][1] - d[1][0]);
                    if (const float l = glm::length(ax); l > 1e-7f) ax /= l;
                    const float w = ang / dt;
                    if (w > 0.05f && w < 60.0f && glm::length(ax) > 0.5f)
                    {
                        const glm::vec3 p = glm::vec3(pxo->matWorld[3]);
                        std::printf("spin w %d rel %d oid %d inst %d cid 0x%X pos %.0f %.0f %.0f w %.3f axis %.3f %.3f %.3f\n",
                                    g_transition.iwrecCur, rel, plo->oid, plo->oidInstance, plo->Cid(), p.x, p.y, p.z, w, ax.x, ax.y, ax.z);
                    }
                }
                s_mpmat[palo] = m;
            }
        }
        // debug: SLY2N_CIDLIST=lo-hi@frame: every object whose class is in [lo, hi] (oid, class, in world, parent)
        if (static const char* s_szCid = std::getenv("SLY2N_CIDLIST"); s_szCid && std::strchr(s_szCid, '@') &&
            frame == std::atoi(std::strchr(s_szCid, '@') + 1))
        {
            char* pchEnd = nullptr;
            const long lo = std::strtol(s_szCid, &pchEnd, 0);
            const long hi = *pchEnd == '-' ? std::strtol(pchEnd + 1, nullptr, 0) : lo;
            const long oidOnly = std::getenv("SLY2N_CIDLIST_OID") ? std::atol(std::getenv("SLY2N_CIDLIST_OID")) : -1;
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); oidOnly >= 0 ? (plo->oid == oidOnly || plo->oidInstance == oidOnly) : (plo->Cid() >= lo && plo->Cid() <= hi))
                {
                    const glm::vec3 v = FCidDerivedFrom(plo->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(plo))
                                        : FCidDerivedFrom(plo->Cid(), 3) ? PosJtLo(plo) : glm::vec3(0.0f);  // (a LOX: its record's)
                    std::printf("cidlist %d cid 0x%X inworld %d parent %d pos %.1f %.1f %.1f%s\n", plo->oid, plo->Cid(), plo->fInWorld,
                                plo->paloParent ? plo->paloParent->oid : -1, v.x, v.y, v.z,
                                FCidDerivedFrom(plo->Cid(), 0x62) ? (FBreakableIntact(plo) ? " intact" : " broken") : "");
                }
        }
        // debug: SLY2N_PARTLIST=oid@frame: the object's parts (offset -> oid)
        if (static const char* s_szPart = std::getenv("SLY2N_PARTLIST"); s_szPart && std::strchr(s_szPart, '@') &&
            frame == std::atoi(std::strchr(s_szPart, '@') + 1))
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); plo->oid == std::atoi(s_szPart) && FCidDerivedFrom(plo->Cid(), 0x30))
                {
                    std::printf("partlist %d:", plo->oid);
                    for (const auto& [ib, pp] : static_cast<ALO*>(plo)->apart) std::printf(" [+0x%X %d]", ib, pp ? pp->oid : -1);
                    std::printf("\n");
                }
        // debug: SLY2N_SOELIST=oid@frame (or kKIND@frame): each in-world instance's class, parent and collision entries (tag, kind, state)
        if (static const char* s_szSoe = std::getenv("SLY2N_SOELIST"); s_szSoe && std::strchr(s_szSoe, '@') &&
            frame == std::atoi(std::strchr(s_szSoe, '@') + 1))
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); FCidDerivedFrom(plo->Cid(), 0x30) &&
                    (s_szSoe[0] == 'k' ? [&] {  // (kKIND@frame: every object with an entry of that contact kind)
                        for (const SOE& e : static_cast<ALO*>(plo)->asoe)
                            if (e.id == static_cast<uint32_t>(std::strtoul(s_szSoe + 1, nullptr, 0))) return true;
                        return false;
                    }() : plo->oid == std::atoi(s_szSoe)))
                {
                    ALO* palo = static_cast<ALO*>(plo);
                    std::printf("soelist %d cid 0x%X inworld %d parent %d grf148 0x%llX:", plo->oid, plo->Cid(), plo->fInWorld,
                                plo->paloParent ? plo->paloParent->oid : -1, static_cast<unsigned long long>(Sort(palo).grf148));
                    for (const SOE& e : palo->asoe)
                        std::printf(" [tag %d id 0x%X st %d r %.0f%s%s]", e.tag, e.id, e.state, e.sRadius, e.f49 ? " sphere" : "", e.f50 ? " off" : "");
                    std::printf("\n");
                }
        // debug: SLY2N_HZPRINT=N: every N frames the player's time, health, state, damage reaction and place (the
        // hazard sweep's counterpart of the reference's SLY2_PEEK of 2E1E40 +0xE00@ / +0x18C8 / +0xDF4 / +0x58@+0x70)
        if (static const char* s_szHz = std::getenv("SLY2N_HZPRINT"); s_szHz && frame % std::max(1, std::atoi(s_szHz)) == 0)
            if (ACTOR* pa = PactorFromLo(PloPoCurrent(psw.get())); pa && pa->palo)
            {
                const glm::vec3 pos = PosAloCenter(pa->palo);
                int oidFloor = -1;
                if (pa->pxpFloor)
                    for (const XPD& e : pa->pxpFloor->axpd)
                        if (e.palo && e.paloRoot != pa->palo) oidFloor = e.palo->oid;
                const glm::vec3 vel = VelAloPos(pa->palo);
                std::printf("hz %d t %.3f hp %d state %d dmg %d at %.2f %.2f %.2f floor %d aware %d %.2f %d vel %.2f %.2f %.2f jump %d xfmode %d\n", frame, g_clock.t,
                            DmgCounterOf(pa) ? *DmgCounterOf(pa) : -1, dynamic_cast<SLY*>(pa) ? static_cast<SLY*>(pa)->state : -1, pa->nSubDF4,
                            pos.x, pos.y, pos.z, oidFloor, pa->n10C8, pa->g10CC, pa->n10EC, vel.x, vel.y, vel.z, pa->jump.state, PxfPos(pa->palo) ? PxfPos(pa->palo)->mode : -9);
            }
        // debug: SLY2N_NAVPATH="X Y Z>X Y Z@F": at frame F the nav regions at both points and, in one region, its path
        if (const char* sz = std::getenv("SLY2N_NAVPATH"); sz && std::strchr(sz, '@') && frame == std::atoi(std::strchr(sz, '@') + 1))
        {
            glm::vec3 a(0.0f), b(0.0f);
            if (std::sscanf(sz, "%f %f %f>%f %f %f", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z) == 6)
            {
                NAVREGION* pa = PregionAt(a, false);
                NAVREGION* pb = PregionAt(b, false);
                std::printf("navpath: regions %p / %p\n", (void*)pa, (void*)pb);
                if (pa && pa == pb)
                {
                    glm::vec4 apos[64];
                    const int c = CposNavPath(pa, a, b, 64, apos);
                    for (int i = 0; i < c; ++i) std::printf("navpath %d: %.0f %.0f %.0f\n", i, apos[i].x, apos[i].y, apos[i].z);
                }
            }
        }
        bool FPointInVolume(LO* plo, const glm::vec3& pos);  // (trigger.cpp)
        // debug: SLY2N_VOLPROBE="OID X0 Y0 X1 Y1 STEP Z@F": at frame F, which grid points are inside that volume ("vprobe")
        if (const char* sz = std::getenv("SLY2N_VOLPROBE"); sz && std::strchr(sz, '@') && frame == std::atoi(std::strchr(sz, '@') + 1))
        {
            int oid = 0;
            float x0, y0, x1, y1, st, z;
            if (std::sscanf(sz, "%d %f %f %f %f %f %f", &oid, &x0, &y0, &x1, &y1, &st, &z) == 7 && st > 0.0f)
                for (const auto& up : psw->aploAll)
                    if (up->oid == oid)
                        for (float y = y1; y >= y0; y -= st)
                        {
                            std::printf("vprobe %6.0f ", y);
                            for (float x = x0; x <= x1; x += st) std::printf("%c", FPointInVolume(up.get(), glm::vec3(x, y, z)) ? '#' : '.');
                            std::printf("\n");
                        }
        }
        // debug: SLY2N_HEIGHTMAP="X0 Y0 X1 Y1 STEP@F": at frame F the top surface under each grid point (a line from
        // z 6000 to -3000 through every attachment's solid entries): "hmap x y z cid oid" (no hit: z -99999)
        if (const char* sz = std::getenv("SLY2N_HEIGHTMAP"); sz && std::strchr(sz, '@') && frame == std::atoi(std::strchr(sz, '@') + 1))
        {
            float x0, y0, x1, y1, st;
            if (std::sscanf(sz, "%f %f %f %f %f", &x0, &y0, &x1, &y1, &st) == 5 && st > 0.0f)
                for (float y = y0; y <= y1; y += st)
                    for (float x = x0; x <= x1; x += st)
                    {
                        const glm::vec3 a(x, y, 6000.0f), b(x, y, -3000.0f);
                        std::vector<SOE*> apsoe;
                        for (OXA* poxa : g_physw.apoxa)
                        {
                            if (x < poxa->posMin.x || poxa->posMax.x < x || y < poxa->posMin.y || poxa->posMax.y < y) continue;
                            for (SOE* psoe : poxa->apsoe)
                                if (!psoe->f48 && psoe->palo && apsoe.size() < 0x100) apsoe.push_back(psoe);
                        }
                        LSG lsg;
                        SOE* ph = apsoe.empty() ? nullptr : PsoeHitTestLineObjects(0, a, b, apsoe, &lsg);
                        std::vector<SOE*> apsoeW;  // (the water volumes, cid 0x59: their top)
                        for (OXA* poxa : g_physw.apoxa)
                        {
                            if (x < poxa->posMin.x || poxa->posMax.x < x || y < poxa->posMin.y || poxa->posMax.y < y) continue;
                            for (SOE* psoe : poxa->apsoe)
                                if (psoe->palo && FCidDerivedFrom(psoe->palo->Cid(), 0x59) && apsoeW.size() < 0x100) apsoeW.push_back(psoe);
                        }
                        LSG lsgW;
                        SOE* phW = apsoeW.empty() ? nullptr : PsoeHitTestLineObjects(0, a, b, apsoeW, &lsgW);
                        std::printf("hmap %.0f %.0f %.0f 0x%X %d water %.0f\n", x, y, ph ? lsg.pos0.z : -99999.0f, ph && ph->palo ? unsigned(ph->palo->Cid()) : 0u,
                                    ph && ph->palo ? ph->palo->oid : -1, phW ? lsgW.pos0.z : -99999.0f);
                    }
        }
        // debug: SLY2N_SOETRACK=oid: that object's collision entries every frame (tag, bits 48 / 50, state, matrix position)
        if (const char* s_szSoeTrack = std::getenv("SLY2N_SOETRACK"))
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); plo->oid == std::atoi(s_szSoeTrack) && FCidDerivedFrom(plo->Cid(), 0x30))
                {
                    std::printf("soetrack %d f %d:", plo->oid, frame);
                    for (const SOE& e : static_cast<ALO*>(plo)->asoe)
                        std::printf(" [%d %d%d%d st %d %.0f %.0f %.0f xf %.0f %.0f %.0f s %d/%d/%d]", e.tag, int(e.f48), int(e.f49), int(e.f50), e.state, e.mat[3].x, e.mat[3].y, e.mat[3].z,
                                    e.pxf ? e.pxf->matWorld[3].x : 0.0f, e.pxf ? e.pxf->matWorld[3].y : 0.0f, e.pxf ? e.pxf->matWorld[3].z : 0.0f, e.nStampObj, e.nStampWorld, e.pxf ? e.pxf->nStamp : 0);
                    if (std::getenv("SLY2N_SOEGEOM"))  // (debug: each entry's local hull bounds)
                        for (const SOE& e : static_cast<ALO*>(plo)->asoe)
                        {
                            glm::vec3 mn(1e30f), mx(-1e30f);
                            for (const glm::vec3& v : e.geomLocal.apos) mn = glm::min(mn, v), mx = glm::max(mx, v);
                            glm::vec3 wn(1e30f), wx(-1e30f);
                            for (const glm::vec3& v : e.geomWorld.apos) wn = glm::min(wn, v), wx = glm::max(wx, v);
                            std::printf(" {%d n %zu x %.1f %.1f y %.1f %.1f z %.1f %.1f r %.1f pin %d world %.0f %.0f %.0f .. %.0f %.0f %.0f}", e.tag, e.geomLocal.apos.size(), mn.x, mx.x, mn.y, mx.y, mn.z, mx.z, e.sRadius, int(e.pinned),
                                        wn.x, wn.y, wn.z, wx.x, wx.y, wx.z);
                        }
                    if (static_cast<ALO*>(plo)->psort)
                    {
                        std::printf(" xp");
                        for (int i = 0; i < 26; i++) std::printf(" %d", static_cast<ALO*>(plo)->psort->abXp[i]);
                    }
                    std::printf("\n");
                }
        // debug: SLY2N_SOEIDS=frame: every object with a contact entry (SOE id != 0: the touch hazards' filters and
        // damage states, the sensors), with its class, place and the entries
        if (static const char* s_szIds = std::getenv("SLY2N_SOEIDS"); s_szIds && frame == std::atoi(s_szIds))
            for (const auto& up : psw->aploAll)
                if (LO* plo = up.get(); FCidDerivedFrom(plo->Cid(), 0x30))
                {
                    ALO* palo = static_cast<ALO*>(plo);
                    bool f = false;
                    for (const SOE& e : palo->asoe) f = f || e.id != 0;
                    if (!f) continue;
                    const glm::vec3 pos = PosAloCenter(palo);
                    std::printf("soeids %d cid 0x%X inworld %d parent %d at %.0f %.0f %.0f:", plo->oid, plo->Cid(), plo->fInWorld,
                                plo->paloParent ? plo->paloParent->oid : -1, pos.x, pos.y, pos.z);
                    for (const SOE& e : palo->asoe)
                        if (e.id)
                        {
                            std::printf(" [tag %d id 0x%X st %d%s", e.tag, e.id, e.state, e.f50 ? " off" : "");
                            // (the entry's world bounds, from its world geometry, else its local one by the object's matrix)
                            const bool fW = !e.geomWorld.apos.empty();
                            const std::vector<glm::vec3>& apos = fW ? e.geomWorld.apos : e.geomLocal.apos;
                            if (!apos.empty())
                            {
                                const glm::mat4 m = fW ? glm::mat4(1.0f) : MatWorldOfAlo(palo);
                                glm::vec3 mn(1e30f), mx(-1e30f);
                                for (const glm::vec3& v : apos)
                                {
                                    const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f));
                                    mn = glm::min(mn, w);
                                    mx = glm::max(mx, w);
                                }
                                std::printf(" box %.0f %.0f %.0f .. %.0f %.0f %.0f", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
                            }
                            std::printf("]");
                        }
                    std::printf("\n");
                }
        // (debug: SLY2N_HOOKLIST=frame: the jump targets' point list (the hooks) with their classes and places)
        if (static const char* s_szHooks = std::getenv("SLY2N_HOOKLIST"); s_szHooks && frame == std::atoi(s_szHooks))
            for (LO* plo : AploJtList(psw.get(), JTL_Point))
                std::printf("hooklist %d cid 0x%X e8 %X at %.0f %.0f %.0f\n", plo->oid, int(plo->Cid()), plo->U32Field(0, 0xE8, 0),
                            MatJtLo(plo)[3].x, MatJtLo(plo)[3].y, MatJtLo(plo)[3].z);
        // (debug: SLY2N_JTLIST=kind@frame: that jump-target list (JTL: 3 spire, 5 ledge, 6 wall, ...) with their places)
        if (static const char* s_szJt = std::getenv("SLY2N_JTLIST"); s_szJt && std::strchr(s_szJt, '@') &&
                                                                       frame == std::atoi(std::strchr(s_szJt, '@') + 1))
            for (LO* plo : AploJtList(psw.get(), static_cast<JTL>(std::atoi(s_szJt))))
            {
                std::printf("jtlist %d cid 0x%X at %.0f %.0f %.0f", plo->oid, int(plo->Cid()), MatJtLo(plo)[3].x, MatJtLo(plo)[3].y,
                            MatJtLo(plo)[3].z);
                if (const CRV* pcrv = PcrvFromLo(plo); pcrv && !pcrv->apos.empty())  // (a curve's ends in the world)
                {
                    const glm::vec3 a = glm::vec3(MatJtLo(plo) * glm::vec4(pcrv->apos.front(), 1.0f));
                    const glm::vec3 b = glm::vec3(MatJtLo(plo) * glm::vec4(pcrv->apos.back(), 1.0f));
                    std::printf(" curve %zu points %.0f %.0f %.0f .. %.0f %.0f %.0f", pcrv->apos.size(), a.x, a.y, a.z, b.x, b.y, b.z);
                }
                std::printf("\n");
            }
        if (static const char* s_szCidPos = std::getenv("SLY2N_CIDPOS"); s_szCidPos && std::strchr(s_szCidPos, '@') &&
            frame == std::atoi(std::strchr(s_szCidPos, '@') + 1))
            for (const auto& up : psw->aploAll)
            {
                LO* plo = up.get();
                bool f = false;
                for (const char* q = s_szCidPos; q && *q && *q != '@'; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                    if (FCidDerivedFrom(plo->Cid(), static_cast<CID>(std::strtol(q, nullptr, 0)))) f = true;
                if (!f) continue;
                glm::vec3 pos(0.0f);
                if (FCidDerivedFrom(plo->Cid(), 0x30)) pos = PosAloCenter(static_cast<ALO*>(plo));
                else if (const LOX* px = dynamic_cast<const LOX*>(plo)) pos = glm::vec3((px->pxf ? px->pxf->matWorld * px->mat : px->mat)[3]);
                std::printf("cidpos 0x%X %d (alias %d) parent %d inworld %d state %d at %.0f %.0f %.0f r %.1f", plo->Cid(), plo->oid, plo->oidInstance,
                            plo->paloParent ? plo->paloParent->oid : -1, plo->fInWorld ? 1 : 0, plo->nState, pos.x, pos.y, pos.z, plo->sRadius);
                if (FCidDerivedFrom(plo->Cid(), 0x30))   // (its world x axis: which way the object faces)
                    std::printf(" xaxis %.3f %.3f", MatWorldOfAlo(static_cast<ALO*>(plo))[0].x, MatWorldOfAlo(static_cast<ALO*>(plo))[0].y);
                if (FCidDerivedFrom(plo->Cid(), 0x30))   // (its collision entries' world bounds: where a piece really is,
                    for (const SOE& e : static_cast<ALO*>(plo)->asoe)  // e.g. a bridge's cleats, all at the root)
                        if (!e.geomWorld.apos.empty())
                        {
                            glm::vec3 lo(3.4e38f), hi(-3.4e38f);
                            for (const glm::vec3& v : e.geomWorld.apos) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
                            std::printf(" soe (%.0f %.0f %.0f)..(%.0f %.0f %.0f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                        }
                if (const CMDLIST* pl = dynamic_cast<const CMDLIST*>(plo))
                    for (const auto& c : pl->acmd) std::printf(" [op %d oid %d n8 %d]", c->op, c->oid, c->n8);
                if (FCidDerivedFrom(plo->Cid(), 0x2E))  // (a recon subject: its task +0xEC, linked +0x100)
                    std::printf(" task %d linked %d", static_cast<int>(plo->U32Field(0, 0xEC, uint32_t(-1))), static_cast<int>(plo->U32Field(0, 0x100, uint32_t(-1))));
                if (FIsLoDerivedFrom(plo, 0x0A))   // (a path (rails, pipes ..): its points in the world)
                    if (const CRV* pcrv = PcrvFromLo(plo))
                        for (size_t i = 0; i < pcrv->ag.size(); ++i)
                        {
                            glm::vec3 p(0.0f);
                            EvalCrv(pcrv, pcrv->ag[i], &p, nullptr);
                            std::printf(" (%.0f %.0f %.0f)", p.x, p.y, p.z);
                        }
                std::printf("\n");
            }
        PrepareSlyFrame(frame);
        PrepareGangFrame(frame);
        g_iframeHarness = frame;
        UpdateSw(psw.get());
        // orig 0x1D9648 (the HUD's update, hudwidget.h; its wipe 0x22B198 here first)
        // (its start: the load's pad lock 8 lifted once the new world's clock passes 0.1 s at its rate)
        if ((g_grfjoyLock & 8) && g_clock.rDt != 0.0f && 0.1f < g_clock.t / g_clock.rDt) UnlockJoy(8);
        UpdateWipe(psw.get());
        UpdateHud(psw.get());
        // debug: SLY2N_F6AT=frame does F6 headless
        static const int s_iframeF6 = std::getenv("SLY2N_F6AT") ? std::atoi(std::getenv("SLY2N_F6AT")) : -1;
        if (frame == s_iframeF6) DebugUnlockAll(psw.get());
        // debug: SLY2N_F7=1 is F7 (infinite health and power) headless
        if (static const bool s_fF7Env = std::getenv("SLY2N_F7") != nullptr; s_fF7Env) CheatRefillPlayers(psw.get());
        // debug: SLY2N_COMPLETEJOBAT=frame[,frame...] does F9 (complete the current job) headless
        if (static const char* s_szF9 = std::getenv("SLY2N_COMPLETEJOBAT"); s_szF9)
            for (const char* q = s_szF9; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
                if (std::atoi(q) == frame) FDebugCompleteJob();
        // debug keys (only with the launcher's Settings > Advanced > Debug mode on, Debug Mode.ini; README.md):
        // F2 the Paris hub, F3 the next world record (with the wipe)
        if (g_pcinput.fDebugKeys && !g_transition.fPending && g_transition.wipes == WIPES_Idle)
        {
            static bool s_fF2 = false, s_fF3 = false;
            const bool fF2 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F2);
            const bool fF3 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F3);
            if (fF2 && !s_fF2) RequestWorldWarp(psw.get(), 2);
            if (fF3 && !s_fF3)
            {
                int i = g_transition.iwrecCur + 1;
                if (i == 18) ++i;  // (no file)
                if (i >= 44) i = 2;
                RequestWorldWarp(psw.get(), i);
            }
            s_fF2 = fF2;
            s_fF3 = fF3;
            // F5: save to <user-dir>/Saves/saveN (the slot played, else save1; the card slot's format)
            static bool s_fF5 = false;
            const bool fF5 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F5);
            if (fF5 && !s_fF5)
            {
                // (as the menu's Save Game into the slot played, else slot 1: FWriteSaveSlot)
                const int islot = g_islotChosen >= 0 ? g_islotChosen : 0;
                std::printf("F5: slot %d %s\n", islot + 1, FWriteSaveSlot(islot) ? "saved" : "not saved");
            }
            s_fF5 = fF5;
            // F6 (debug, temporary): every gadget / powerup acquired, 9999 coins, every character selectable
            static bool s_fF6 = false;
            const bool fF6 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F6);
            if (fF6 && !s_fF6) DebugUnlockAll(psw.get());
            s_fF6 = fF6;
            // F7 (debug, temporary): toggles infinite health and gadget power
            static bool s_fF7 = false;
            const bool fF7 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F7);
            if (fF7 && !s_fF7)
            {
                g_fDebugInfinite = !g_fDebugInfinite;   // (the debug menu's Player page too; applied by UpdateDebugTools)
                std::printf("F7: infinite health and power %s\n", g_fDebugInfinite ? "on" : "off");
            }
            s_fF7 = fF7;
            // F9 (debug): complete the current job (task.h FDebugCompleteJob: the job's own end, its log line names it)
            static bool s_fF9 = false;
            const bool fF9 = FDebugKeyHeld(g_gl.window, GLFW_KEY_F9);
            if (fF9 && !s_fF9) FDebugCompleteJob();
            s_fF9 = fF9;
        }
        // the debug menu's actions, noclip, the time scale, F7's refill, F11 (debugmenu.h; Debug keys only)
        UpdateDebugTools(psw.get());
        {
            // (the root ALOs in aploAll order, kept while no object is made and no parent changes: the scan of every
            // object was a tenth of the step; SLY2N_ROOTSCAN scans every step as before)
            static std::vector<ALO*> s_apaloRoot;
            static uint32_t s_serialRoot = 0, s_cParentRoot = 0;
            static size_t s_cploRoot = size_t(-1);
            static const bool s_fScan = std::getenv("SLY2N_ROOTSCAN") != nullptr;
            if (s_fScan || s_serialRoot != psw->serial || s_cploRoot != psw->aploAll.size() || s_cParentRoot != g_cLoParentChanges)
            {
                s_apaloRoot.clear();
                for (const auto& plo : psw->aploAll)
                    if (!plo->paloParent && FCidDerivedFrom(plo->Cid(), 0x30)) s_apaloRoot.push_back(static_cast<ALO*>(plo.get()));
                s_serialRoot = psw->serial, s_cploRoot = psw->aploAll.size(), s_cParentRoot = g_cLoParentChanges;
            }
#ifdef __SWITCH__
            // (the Switch: the hierarchies on the pass helpers, the walk being memory bound. A hierarchy whose records
            // refer outside it stops before reading there and is redone here after the others, in order: it then reads
            // every root's world of this frame, where the serial walk read the later roots' previous ones.)
            static std::vector<uint8_t> s_afRedo;
            s_afRedo.assign(s_apaloRoot.size(), 0);
            PassHelpers().ForBlocks(s_apaloRoot.size(), 8, [&](size_t iFirst, size_t iLim) {
                for (size_t i = iFirst; i < iLim; ++i)
                    if (!FUpdateAloHierarchyWorldsIsolated(s_apaloRoot[i], s_apaloRoot[i])) s_afRedo[i] = 1;
            });
            for (size_t i = 0; i < s_apaloRoot.size(); ++i)
                if (s_afRedo[i]) UpdateAloHierarchyWorlds(s_apaloRoot[i]);
#else
            for (ALO* palo : s_apaloRoot) UpdateAloHierarchyWorlds(palo);
#endif
        }
        DrawSlys(psw.get());
        DebugHitHideHolders(psw.get());   // (debug: hide.h)   // orig 0x18A238: the draw's own placement (state 3's cane on the hook), each step as drawn
        };
        {
            const bool fWant = s_szFrameInterp ? true : fWindow && !go.fFixedDt && g_display.frameRate > 60;
            if (fWant != accum.fActive)
            {
                accum = FRAMEACCUM{};
                accum.fActive = fWant;
                accum.fpsVirtual = s_szFrameInterp ? std::atof(s_szFrameInterp) : 0.0;
                ResetStepSnapshots();
            }
        }
        const int cstep = accum.fActive ? accum.Steps(now() / 1000.0) : 1;
        for (int k = 0; k < cstep; ++k)
        {
            if (k > 0) ++frame;
            if (!accum.fActive)
            {
                stepSim(frame);
                continue;
            }
            const float dtFixedSave = g_dtFixedStep;
            g_dtFixedStep = DT_FRAME_NTSC;   // (each step exactly 1001/60000 s, as the original's NTSC frames)
            stepSim(frame);
            g_dtFixedStep = dtFixedSave;
            // the step's placements and camera, the newer of the pair the frames are drawn between
            if (fDraws)
            {
                static const bool s_fDrawAllStep = std::getenv("SLY2N_DRAWALL") != nullptr;
                if (CAddNewAlosToScene(psw.get(), scene, &apalo) > 0) BuildSceneGpu(scene);
                UpdateSceneFromSw(scene, apalo, s_fDrawAllStep ? nullptr : psw.get());
                SnapshotStep(scene, CameraFromCm(g_cm));
            }
        }
        // (skipcut.h) a skipped cutscene: the game runs on to its natural end, many steps a frame (up to ~25 ms)
        {
            static double s_tPrevSkip = 0.0;
            const double tNowSkip = now();
            const float dtSkip = fWindow && !go.fFixedDt ? float(std::clamp((tNowSkip - s_tPrevSkip) / 1000.0, 0.0, 0.1)) : 1.0f / 60.0f;
            s_tPrevSkip = tNowSkip;
            // (the player's own input: a route's or a press list's buttons never skip, unless SLY2N_SKIPPRESS)
            extern uint16_t g_grfJoyWindow;
            static const bool s_fSkipPress = std::getenv("SLY2N_SKIPPRESS") != nullptr;
            static uint16_t s_grfSkipPrev = 0;
            const uint16_t grfSkip = s_fSkipPress ? g_joy.grfbtn : g_grfJoyWindow;
            UpdateSkipCut(dtSkip, grfSkip, uint16_t(grfSkip & ~s_grfSkipPrev));
            s_grfSkipPrev = grfSkip;
            // (the whole skip within this frame, up to 5 s of real time (a minute's scene takes about 1 s): the last
            // frame shown stays up meanwhile and the next one is the scene's end, with no blackout; the window's
            // events are polled meanwhile. Only a longer one (a stuck scene) goes on over more frames, behind
            // skipcut.cpp's fade. The drawn scene is brought up to date once, after it.)
            const double tEnd = tNowSkip + 5000.0;
            double tPoll = tNowSkip;
            bool fSkipped = false;
#ifdef __SWITCH__
            // (the fast-forward draws nothing: the CPU's loading profile while it runs)
            const bool fBoostSkip = FSkipCutFastForward();
            if (fBoostSkip) CaneSwitch::BeginLoadingBoost();
#endif
            while (FSkipCutFastForward() && (go.cframe < 0 || frame + 1 < go.cframe) && (!fWindow || now() < tEnd))
            {
                ++frame;
                fSkipped = true;
                const float dtFixedSave = g_dtFixedStep;
                g_dtFixedStep = DT_FRAME_NTSC;
                stepSim(frame);
                g_dtFixedStep = dtFixedSave;
                if (fWindow && now() - tPoll > 50.0) { glfwPollEvents(); tPoll = now(); }
                if (!FSkipCutStep()) break;
            }
#ifdef __SWITCH__
            if (fBoostSkip) CaneSwitch::EndLoadingBoost();
#endif
            if (fSkipped && std::getenv("SLY2N_SKIPPRINT")) std::printf("skipcut: %.1f ms of fast-forward this frame\n", now() - tNowSkip);
            if (fSkipped && accum.fActive && !accum.fpsVirtual)
                accum.tLast = now() / 1000.0;   // (the skip's real time is no gameplay time to catch up)
            if (fSkipped && accum.fActive && fDraws)
            {
                ResetStepSnapshots();
                if (CAddNewAlosToScene(psw.get(), scene, &apalo) > 0) BuildSceneGpu(scene);
                UpdateSceneFromSw(scene, apalo, psw.get());
                SnapshotStep(scene, CameraFromCm(g_cm));
            }
        }
        if (fWindow)
        {
            glfwPollEvents();
            GLFWwindow* w = g_gl.window;
            const bool fF1 = FDebugKeyHeld(w, GLFW_KEY_F1);   // (debug keys only)
            if (fF1 && !fF1Last) fFreeCam = !fFreeCam;
            fF1Last = fF1;
            // (Escape no longer closes the window: it is the pause / back key, pcinput.h; the pause menu has Exit to
            // Desktop)
            if (fFreeCam)
            {
                // F1: a free camera (WASD / Q E, right mouse looks)
                const float speed = (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) ? 4000.0f : 800.0f) / 60.0f;
                const glm::vec3 fwd = cam.Dir();
                const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
                if (glfwGetKey(w, GLFW_KEY_W)) cam.pos += fwd * speed;
                if (glfwGetKey(w, GLFW_KEY_S)) cam.pos -= fwd * speed;
                if (glfwGetKey(w, GLFW_KEY_D)) cam.pos += right * speed;
                if (glfwGetKey(w, GLFW_KEY_A)) cam.pos -= right * speed;
                if (glfwGetKey(w, GLFW_KEY_E)) cam.pos.z += speed;
                if (glfwGetKey(w, GLFW_KEY_Q)) cam.pos.z -= speed;
            }
            static double s_x = 0, s_y = 0;
            static bool s_fDrag = false;
            double x, y;
            glfwGetCursorPos(w, &x, &y);
            if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT))
            {
                if (s_fDrag)
                {
                    cam.yaw -= float(x - s_x) * 0.2f;
                    cam.pitch = std::clamp(cam.pitch - float(y - s_y) * 0.2f, -89.0f, 89.0f);
                }
                s_fDrag = true;
            }
            else s_fDrag = false;
            s_x = x;
            s_y = y;
            int fbw, fbh;
            glfwGetFramebufferSize(w, &fbw, &fbh);
            if (fbw > 0 && fbh > 0 && (fbw != g_display.outputWidth || fbh != g_display.outputHeight)) LayoutDisplay(fbw, fbh);
        }
        // (debugmenu.h) the free camera's pad controls (and SLY2N_FREECAMAT's test switch)
        if (fFreeCam && cam.fMat) cam.fMat = false;
        FUpdateDebugFreeCamPad(cam, frame);
        const double t1 = now();
        double t2 = t1;
        // (debug: SLY2N_SHOTEVERY=dir:N also writes every N-th frame to dir/frame_NNNNNN_tT.png, T the game clock)
        static const char* s_pchzShotEvery = std::getenv("SLY2N_SHOTEVERY");
        static const std::string s_strShotDir = s_pchzShotEvery ? std::string(s_pchzShotEvery, std::strcspn(s_pchzShotEvery, ":")) : "";
        static const int s_cShotEvery = s_pchzShotEvery && std::strchr(s_pchzShotEvery, ':') ? std::atoi(std::strchr(s_pchzShotEvery, ':') + 1) : 0;
        // (SLY2N_SHOTFROM=F: only from frame F on)
        static const int s_frameShotFrom = std::getenv("SLY2N_SHOTFROM") ? std::atoi(std::getenv("SLY2N_SHOTFROM")) : 0;
        const bool fShotEvery = s_cShotEvery > 0 && (frame + 1) % s_cShotEvery == 0 && frame >= s_frameShotFrom;
        const bool fDrawFrame = fWindow || (!go.screenshot.empty() && frame + 1 == go.cframe) || fShotEvery ||
                                (fDraws && (FPhotoCapturePending() || FAnimatedOverlayNeedsFrame() || std::getenv("SLY2N_RENDERALL")));
        if (fDrawFrame)
        {
            static const bool s_fDrawAll0 = std::getenv("SLY2N_DRAWALL") != nullptr;
            // the spawners' run-time clones (and their parts) into the scene (scenesw.h; AI code)
            CAMERA camStep = CameraFromCm(g_cm);
            // (debug: SLY2N_RAMXF=ram.bin[:,oid,oid,]: before each drawn frame the mapped objects' transform records (mat, world)
            // from a reference / PCSX2 RAM snapshot, all objects or only those oids: the same moment's moving parts, e.g. R51's
            // swinging searchlights, for a picture comparison at the snapshot's camera (--camera-ram))
            if (static const char* s_szRamXf = std::getenv("SLY2N_RAMXF"); s_szRamXf)
            {
                static RAM s_ram;
                static MAPPING s_map;
                static std::string s_strOids;
                static const bool s_fRam = [&] {
                    std::string path = s_szRamXf;
                    if (size_t i = path.find(":,"); i != std::string::npos) { s_strOids = path.substr(i + 1); path.resize(i); }
                    return s_ram.FLoad(path);
                }();
                static bool s_fMapped = false;
                if (s_fRam && !s_fMapped) { MapObjects(psw.get(), s_ram, s_map); s_fMapped = true; }
                for (const auto& [a, plo] : s_map.mpaplo)
                {
                    if (!FCidDerivedFrom(plo->Cid(), 0x30) || !plo->fInWorld) continue;
                    if (!s_strOids.empty() && s_strOids.find("," + std::to_string(plo->oid) + ",") == std::string::npos) continue;
                    ALO* palo = static_cast<ALO*>(plo);
                    const uint32_t c = s_ram.U32(a + 0x50), ar = s_ram.U32(a + 0x54);
                    if (c != palo->axf.size() || !s_ram.FValid(ar)) continue;
                    for (uint32_t i = 0; i < c; ++i)
                    {
                        palo->axf[i]->mat = s_ram.Mat4(ar + 0x100 * i);
                        palo->axf[i]->matWorld = s_ram.Mat4(ar + 0x100 * i + 0x40);
                    }
                }
            }
            if (!accum.fActive)
            {
                if (CAddNewAlosToScene(psw.get(), scene, &apalo) > 0) BuildSceneGpu(scene);
                UpdateSceneFromSw(scene, apalo, s_fDrawAll0 ? nullptr : psw.get());
            }
            else InterpolateSteps(scene, camStep, accum.U());   // (placed at the steps)
            // The display rates above 60 Hz relight (the lights, the CPU vertex lighting) on the frames that ran a step only:
            // the lights are the step's, and the colours of the frames drawn between two steps (a fraction of a 60 Hz
            // step's movement) are not told apart, while relighting every moving object at every drawn frame was a
            // third of an uncapped frame's time. (60 Hz and below: every frame is a step.)
            static const bool s_fRelightAll = std::getenv("SLY2N_RELIGHTALL") != nullptr;   // (debug: every drawn frame, as before)
            const bool fRelight = !accum.fActive || cstep > 0 || s_fRelightAll;
            // the light contexts' positions: each entry's root object (0x1F2500: the top without a parent)
            auto posLightOf = [&](size_t k) {
                const ALO* proot = apalo[k];
                if (!proot) return;
                while (proot->paloParent) proot = proot->paloParent;
                // (0x1F2500: pl +4 = the root's +0x60 record + 0x70, its world position: an actor's object record, which the
                // actors' slot-29 teleport moves while the root record stays at the origin)
                const XF* pxfObj = proot->apxfRef[2];
                const glm::vec3 pos = pxfObj ? glm::vec3(pxfObj->matWorld[3])
                                             : proot->axf.empty() ? glm::vec3(0.0f) : glm::vec3(proot->axf[0]->matWorld[3]);
                SCENEGLOBSET& sgs = scene.aglobset[k];
                static const int s_oidPosLightDbg = std::getenv("SLY2N_POSLIGHTDBG") ? std::atoi(std::getenv("SLY2N_POSLIGHTDBG")) : -1;
                if (s_oidPosLightDbg >= 0 && apalo[k]->oid == s_oidPosLightDbg)
                    std::printf("poslight %d frame %d: obj %d %.0f %.0f %.0f root %.0f %.0f %.0f\n", apalo[k]->oid, frame, pxfObj != nullptr, pos.x, pos.y, pos.z,
                                proot->axf.empty() ? 0.f : proot->axf[0]->matWorld[3][0], proot->axf.empty() ? 0.f : proot->axf[0]->matWorld[3][1],
                                proot->axf.empty() ? 0.f : proot->axf[0]->matWorld[3][2]);
                if (s_oidPosLightDbg >= 0 && apalo[k]->oid == s_oidPosLightDbg)
                    for (int j = 0; j < 3; ++j)
                        if (const XF* p = proot->apxfRef[j]) std::printf("   ref %d %p %.0f %.0f %.0f\n", j, (const void*)p, p->matWorld[3][0], p->matWorld[3][1], p->matWorld[3][2]);
                if (s_oidPosLightDbg >= 0 && apalo[k]->oid == s_oidPosLightDbg)
                    for (size_t j = 0; j < proot->axf.size() && j < 8; ++j)
                        std::printf("   xf %zu %p %.0f %.0f %.0f\n", j, (const void*)proot->axf[j].get(), proot->axf[j]->matWorld[3][0], proot->axf[j]->matWorld[3][1], proot->axf[j]->matWorld[3][2]);
                // (a frame without a relight (below) keeps the flag for the next one that relights)
                sgs.fPosLightMoved = (!fRelight && sgs.fPosLightMoved) || !sgs.fPosLight || pos != sgs.posLight;
                sgs.fPosLight = true;
                sgs.posLight = pos;
            };
            const size_t cposLight = std::min(scene.aglobset.size(), apalo.size());
#ifdef __SWITCH__
            // (the Switch: on the pass helpers, each entry writing only its own fields; the roots' records it reads are
            // scattered over the scene)
            PassHelpers().ForBlocks(cposLight, 64, [&](size_t kFirst, size_t kLim) {
                for (size_t k = kFirst; k < kLim; ++k) posLightOf(k);
            });
#else
            for (size_t k = 0; k < cposLight; ++k) posLightOf(k);
#endif
            static const bool s_fDrawAll = std::getenv("SLY2N_DRAWALL") != nullptr;  // debug: every glob set
            if (!s_fDrawAll) ApplyDrawPass(psw.get(), scene, apalo);  // orig RenderSw 0x1CDA80's selection
            ApplyRtgtOwners(scene, apalo);   // the binocucom's talking heads (0x1BEB28's lists)
            {
                // the foreground globs (0x168638: bit 34 and G_GAME +0x3BEC == 0)
                const bool fNoBig = CBigScreenObjsShown(psw.get()) == 0;
                const uint32_t serial = psw->serial;
                for (auto it = s_mpploForeground.begin(); it != s_mpploForeground.end();)
                    it = it->second != serial ? s_mpploForeground.erase(it) : std::next(it);
                for (size_t k = 0; k < scene.aglobset.size() && k < apalo.size(); ++k)
                    scene.aglobset[k].fForeground = fNoBig && !s_mpploForeground.empty() && s_mpploForeground.count(apalo[k]);
            }
            // the lights move with their objects; the forward-lit groups take their light context again (0x1A4478)
            if (fRelight) UpdateWorldLightsFromSw(psw.get());
            if (!fRelight) {}
            else if (static const char* const e = std::getenv("SLY2_RAMLIGHTS"); e)   // debugging: the lights of a RAM dump (a savestate's)
            {
                static LIGHTSET s_lights;
                static const bool s_f = FLightsFromRam(e, s_lights);
                RelightForward(scene, s_f ? s_lights : WorldLights(), true, s_alightsRtgt);
            }
            else RelightForward(scene, WorldLights(), true, s_alightsRtgt);
            UpdateShaderAnimations(g_clock.t, accum.fActive ? g_clock.dt * float(cstep) : g_clock.dt);
            msScene += now() - t1;
            // the native camera (F1: the free camera from where it is)
            if (!fFreeCam)
            {
                cam = accum.fActive ? camStep : CameraFromCm(g_cm);
                ApplyCmFog(g_cm, &opts);
            }
            else cam.fMat = false;
            if (!go.cameraRam.empty())
            {
                static RENDEROPTS s_optsRam;
                static const CAMERA s_camRam = CameraFromRam(go.cameraRam, s_optsRam);
                cam = s_camRam;
            }
            if (!go.camera.empty())
            {
                float a[5];
                if (std::sscanf(go.camera.c_str(), "%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4]) == 5)
                {
                    cam.fMat = false;
                    cam.pos = glm::vec3(a[0], a[1], a[2]);
                    cam.yaw = a[3];
                    cam.pitch = a[4];
                }
            }
            if (std::getenv("SLY2N_CAMPRINT"))   // (debug: each drawn frame's camera and the player's drawn root)
            {
                const glm::vec3 posSly = !apalo.empty() && PloPoCurrent(psw.get()) ? [&] {
                    for (size_t k = 0; k < apalo.size(); ++k)
                        if (apalo[k] == PloPoCurrent(psw.get()) && !scene.aglobset[k].amatGlob.empty()) return glm::vec3(scene.aglobset[k].amatGlob[0][3]);
                    return glm::vec3(0.0f);
                }() : glm::vec3(0.0f);
                std::printf("campr %d step %d u %.3f cam %.2f %.2f %.2f sly %.2f %.2f %.2f\n", cframeTimed, frame, accum.fActive ? accum.U() : 1.0f, cam.pos.x, cam.pos.y, cam.pos.z,
                            posSly.x, posSly.y, posSly.z);
            }
            // orig: 0x1A7518/0x1477B0: query before the overlay's state1 capture/transition.
            // (SLY2N_FPSLOG: the GPU's time per frame, a timer query read back a few frames later)
            static const bool s_fFpsLog = std::getenv("SLY2N_FPSLOG") != nullptr;
            static GLuint s_aqGpu[4] = {};
            static int s_iqGpu = 0;
            if (s_fFpsLog && fWindow)
            {
                if (!s_aqGpu[0]) glGenQueries(4, s_aqGpu);
                else if (s_iqGpu >= 4)
                {
                    GLuint64 ns = 0;
                    glGetQueryObjectui64v(s_aqGpu[s_iqGpu % 4], GL_QUERY_RESULT, &ns);
                    g_msGpuFpsLog += double(ns) / 1e6;
                }
                glBeginQuery(GL_TIME_ELAPSED, s_aqGpu[s_iqGpu % 4]);
            }
            if (FAnimatedOverlayDrawNormal()) {
                RenderScene(scene, cam, opts);
                const double tHud = s_fFpsLog ? now() : 0.0;
                DrawHud(psw.get(), g_gl.renderWidth, g_gl.renderHeight);
                if (s_fFpsLog) g_msHudFpsLog += now() - tHud;
                RenderSceneForeground(scene, cam, opts);   // kinds 22..25 over the HUD (group 10, 0x157CF0)
                DrawDebugOverlay(g_gl.renderWidth, g_gl.renderHeight);   // (debugmenu.h: the debug menu over all; Debug keys only)
                g_gl.RememberOverlaySourceFrame();
            }
            DrawAnimatedOverlayFrame();  // orig: 0x1477D8, capture then clear/draw the exclusive surfaces
            if (!go.screenshot.empty() && frame + 1 == go.cframe)
            {
                glFinish();
                g_gl.FWriteScreenshot(go.screenshot);
            }
            if (fShotEvery)
            {
                glFinish();
                char ach[64];
                if (accum.fActive) std::snprintf(ach, sizeof ach, "/frame_d%06d_%06d_t%.2f.png", cframeTimed, frame + 1, g_clock.t);   // (a frame per display frame)
                else std::snprintf(ach, sizeof ach, "/frame_%06d_t%.2f.png", frame + 1, g_clock.t);
                g_gl.FWriteScreenshot(s_strShotDir + ach);
            }
            t2 = now();
            if (fWindow)
            {
                g_gl.Present();
                if (s_fFpsLog && s_aqGpu[0]) glEndQuery(GL_TIME_ELAPSED), ++s_iqGpu;
#ifdef __SWITCH__
                // (SaltyNX's overlays: the scene's render size, which the internal resolution sets)
                CaneDeko::SetFrameResolution(unsigned(g_gl.renderWidth), unsigned(g_gl.renderHeight));
#endif
                cane::gl::SwapBuffers(g_gl.window);
            }
        }
        else PrepareAnimatedOverlayFrame();  // same logical boundary for frames without a GL target
        const double tSwapped = now();
        // (SLY2N_REALTIME: a headless run paced at 60 Hz too, so the audio thread's streams (dialog lines the
        // scripts wait on) keep up with the frames)
        static const bool s_fRealtime = std::getenv("SLY2N_REALTIME") != nullptr;
        if (fWindow && g_display.frameRate < DISP_FRAMERATE_UNCAPPED)   // the frame-rate cap (with or without VSync)
            pacer.Wait(g_display.frameRate, g_display.fSwapPacesFrames, g_display.swapGeneration);
        else if (s_fRealtime) pacer.Wait(60);
        msUpdate += t1 - t0;
        msRender += t2 - t1;
        msSwap += now() - t2;
        ++cframeTimed;
        // (SLY2N_FRAMETIMES=file: one CSV line a frame, for a tester's machine: where each frame's time went. update: the
        // steps (game logic); draw: the scene's CPU side and its GL calls; swap: the present and the swap; wait: the
        // frame-rate cap's pacer; frame: from this frame's start to the next's)
        if (static FILE* s_pfTimes = [] {
                const char* psz = std::getenv("SLY2N_FRAMETIMES");
                FILE* pf = psz && *psz ? std::fopen(psz, "w") : nullptr;
                if (pf) std::fprintf(pf, "frame,step,steps,update_ms,draw_ms,swap_ms,wait_ms,frame_ms,cap,vsync\n");
                return pf;
            }();
            s_pfTimes)
        {
            static double s_t0Prev = -1.0, s_aPrev[4] = {};
            static int s_iframe = 0, s_stepPrev = 0, s_cstepPrev = 0;
            if (s_t0Prev >= 0.0)
                std::fprintf(s_pfTimes, "%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n", s_iframe - 1, s_stepPrev, s_cstepPrev, s_aPrev[0], s_aPrev[1],
                             s_aPrev[2], s_aPrev[3], t0 - s_t0Prev, g_display.frameRate, int(g_display.fVsync));
            const double tEnd = now();
            s_t0Prev = t0, s_aPrev[0] = t1 - t0, s_aPrev[1] = t2 - t1, s_aPrev[2] = tSwapped - t2, s_aPrev[3] = tEnd - tSwapped;
            s_stepPrev = frame, s_cstepPrev = cstep;
            if (++s_iframe % 600 == 0) std::fflush(s_pfTimes);
        }
        // (debug: SLY2N_FPSLOG=S: every S seconds of wall time the display frames' rate and where their time went)
        if (static const double s_sFpsLog = std::getenv("SLY2N_FPSLOG") ? std::atof(std::getenv("SLY2N_FPSLOG")) : 0.0; s_sFpsLog > 0)
        {
            static double s_t0 = now(), s_aMs[4] = {};
            static int s_c = 0;
            const double tEnd = now();
            s_aMs[0] += t1 - t0, s_aMs[1] += t2 - t1, s_aMs[2] += msScene - s_aMs[3], s_aMs[3] = msScene;
            static double s_msGpu0 = 0, s_msHud0 = 0;
            auto msCpu = [] {
#ifdef _WIN32
                FILETIME created{}, exited{}, kernel{}, user{};
                if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
                ULARGE_INTEGER k{}, u{};
                k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
                return static_cast<double>(k.QuadPart + u.QuadPart) / 10000.0;
#else
                timespec ts{};
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
            };
            static double s_msCpu0 = msCpu();
            ++s_c;
            if (tEnd - s_t0 >= s_sFpsLog * 1000.0)
            {
                const double msFrame = (tEnd - s_t0) / s_c;
                std::printf("fps %.1f step %d t %.2f: %.3f ms/frame: update %.3f, draw %.3f (scene %.3f, hud %.3f), present+wait %.3f; gpu %.3f; main thread cpu %.3f\n",
                            1000.0 / msFrame, frame, g_clock.t, msFrame, s_aMs[0] / s_c, s_aMs[1] / s_c, s_aMs[2] / s_c,
                            (g_msHudFpsLog - s_msHud0) / s_c, msFrame - (s_aMs[0] + s_aMs[1]) / s_c, (g_msGpuFpsLog - s_msGpu0) / s_c,
                            (msCpu() - s_msCpu0) / s_c);
                s_msCpu0 = msCpu();
                s_msGpu0 = g_msGpuFpsLog, s_msHud0 = g_msHudFpsLog;
                s_t0 = tEnd, s_c = 0, s_aMs[0] = s_aMs[1] = s_aMs[2] = 0;
            }
        }
        if (cstep > 0) ++frame;
    }
    if (pfDump) std::fclose(pfDump);
    // debug: SLY2N_DUMPSAVE=file: the save block (orig 0x3D4A60) at the end (tools/savedump.py prints / diffs it)
    if (const char* sz = std::getenv("SLY2N_DUMPSAVE"))
    {
        std::string err;
        if (!FWriteGameSave(sz, err)) std::printf("save dump: %s\n", err.c_str());
    }
    if (const char* psz = std::getenv("SLY2N_POSECHECK"))
    {
        void PoseCheck(SW * psw, const char* pszSpec);  // animref.cpp
        PoseCheck(psw.get(), psz);
    }
    if (std::getenv("SLY2_PARTICLESTATS"))
        std::printf("particles: %d emitters (%d groups), %d systems, %d drawn in the last frame, %d RIP particles skipped, "
                    "%d emitter updates\n", g_particlestats.cemitter, g_particlestats.cemitterGroup, g_particlestats.cpsys,
                    g_particlestats.cdrawn, g_particlestats.cripSkipped, g_particlestats.cupdate);
    ReportSlyHarness();
    ReportGangHarness();
    if (cframeTimed)
        std::printf("%d frames: update %.2f ms, draw %.2f ms (scene %.2f), present+wait %.2f ms per frame\n", cframeTimed,
                    msUpdate / cframeTimed, msRender / cframeTimed, msScene / cframeTimed, msSwap / cframeTimed);
    // The game's end (native only: the PS2 game never exits): the world released in the transition's order, then the
    // sound thread joined and the audio device closed, all before the statics go. (Before, the world was freed by the
    // return without the transition's resets: the scene runners' table (ResetScpts) was destroyed among the statics
    // and wrote into an already destroyed one (scpt.cpp OrphanCmTrans): "corrupted double-linked list" at exit, PR5 /
    // the Ep7 boss run; and the sound thread was only joined by an atexit handler while the device's mixer ran on.)
    UnloadWorld(psw, false);
    ShutdownSound();
    if (fDraws) g_gl.Terminate();
    return 0;
}
