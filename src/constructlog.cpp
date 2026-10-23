// constructlog - construction office trucks deliver to several sites in one
// trip.
//
// Each office tick builds a list of jobs, one per site and resource
// (FUN_14014eb10: the site's need = required - delivered - in transit), picks
// a truck for each and hands it over in FUN_140150960 (the dispatch): the
// schedule becomes source -> site -> office and the load at the source is
// what that one site still needs (stop record 0, +4: fraction of the truck's
// capacity). A 20 t truck carrying 5 t, then 2 t, then 10 t makes three trips.
//
// The plugin lets the game dispatch as before, then adds up to
// max_extra_sites more sites of the same office that need the same resource,
// nearest first, between the site and the office, and raises the load to
// their sum. The game's unloading already stops at what a site needs (+1 t),
// so the rest stays on the truck for the next site.
//
// Cargo in transit is counted per truck by FUN_1401541a0: a site anywhere
// ahead in its schedule gets the truck's whole planned load. With several
// sites on board that would reserve the full load at each of them, so its
// one call is redirected and answered from the plugin's plan for the truck.
//
// What is left on board after the last site (the margin, or a site that
// took less) is thrown away with the game's own "discard cargo" once the truck
// is past that site, so it comes back empty and free for any job.
//
// Addresses, formulas and the reasoning: docs/findings.md.

#include "../include/tesmio_plugin.h"
#include <math.h>
#include <unordered_map>

#define CL_VERSION "0.2.4"
#define CL_INI     "plugins\\constructlog.ini"

// ---------------------------------------------------------------- signatures (1.1.1.9)
static const short SIG_DISPATCH[] = {       // FUN_140150960(world, job*, office) -> char
    0x48,0x8b,0xc4, 0x41,0x56, 0x48,0x81,0xec,0x80,0x00,0x00,0x00, 0x48,0xc7,0x40,0xa8,0xfe,0xff,0xff,0xff,
    0x48,0x89,0x58,0x08, 0x48,0x89,0x68,0x10, 0x48,0x89,0x70,0x18,
};
static const short SIG_TRANSIT[] = {        // FUN_1401541a0(world, resource, truck, site, char) -> float
    0x48,0x89,0x5c,0x24,0x10, 0x56, 0x48,0x83,0xec,0x20, 0x49,0x8b,0xd9, 0x4d,0x8b,0xd0, 0x4c,0x8b,0xda,
    0x48,0x8b,0xf1, 0x0f,0x57,0xc0, 0x48,0x85,0xd2,
};
static const short SIG_NEED[] = {           // FUN_1401544a0(world, site, resource, char, float*) -> float
    0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x6c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7c,0x24,0x20,
    0x41,0x56, 0x48,0x83,0xec,0x30, 0x48,0x8b,0x74,0x24,0x60,
};
static const short SIG_CAPACITY[] = {       // FUN_1406bddb0(truck, resource, char full, char) -> float
    0x48,0x89,0x5c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20, 0x57, 0x41,0x54, 0x41,0x56,
    0x48,0x3b,0x15,-1,-1,-1,-1, 0x4c,0x8b,0xd2, 0x4c,0x8b,0x99,0x08,0x17,0x00,0x00,
};
static const short SIG_INSERT_STOP[] = {    // FUN_14067d5d0(truck, building, after, raw) -> char
    0x48,0x89,0x54,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xec,0xe0,0x01,0x00,0x00, 0x48,0xc7,0x44,0x24,0x30,0xfe,0xff,0xff,0xff,
    0x48,0x89,0x9c,0x24,0x30,0x02,0x00,0x00,
};
static const short SIG_DISCARD[] = {        // FUN_1406beee0(vehicle, char) - the "discard cargo" button
    0x40,0x57, 0x41,0x57, 0x48,0x83,0xec,0x38, 0x48,0x8b,0x81,0xa8,0x03,0x00,0x00, 0x45,0x33,0xff,
    0x48,0x2b,0x81,0xa0,0x03,0x00,0x00,
};
#define DISPATCH_CALLS 3            // 2 in the office tick, 1 elsewhere
#define TRANSIT_CALLS  1            // FUN_140154090, the in-transit sum

#define SIG(x) x, sizeof(x) / sizeof(x[0])

// ---------------------------------------------------------------- game layout
// world
#define W_WORKERS    0xC2C8  // resource*: workers
// office job (0x40 bytes, FUN_14014eb10)
#define J_SITE       0x00
#define J_SOURCE     0x08
#define J_TRUCK      0x10
#define J_RESOURCE   0x20
#define J_NO_RETURN  0x28    // int: 0 = the office is the last stop
#define J_LOAD       0x30    // float: load fraction for stop 0
#define J_NEED       0x34    // float: what the site still needs
// vehicle
#define V_CARGO      0x3B8   // vector of 16-byte {resource*, float amount, float}
#define V_RECORDS    0x638   // vector of 0x198-byte stop settings, one per stop
#define V_STOPS      0x680   // vector of building*
#define V_STOP_INDEX 0x698   // int: the stop it is going to / at
#define V_IN         0x4F0   // building*: the one it is in
#define V_OFFICE     0x4F8   // building*: its workplace
#define V_TYPE       0x1708
// vehicle type
#define T_CLASS      0x8600  // int: cargo class (7 = passengers, 0x12 = machines)
// stop settings (0x198 bytes)
#define REC_SIZE     0x198
#define REC_LOAD_ON  0x00    // u8: load here
#define REC_LOAD     0x04    // float: load up to this fraction of the capacity
#define REC_UNLOAD_ON 0xC8   // u8: unload here
#define REC_UNLOAD   0xCC    // float
// building
#define B_POSITION   0x3A0   // float3: its C3D_NODE (+0x320) position (+0x80)
#define B_OFFICE_SITES 0xF90 // construction office: vector of site*

typedef char  (*FnDispatch)(BYTE* world, BYTE* job, BYTE* office);
typedef float (*FnTransit)(BYTE* world, BYTE* res, BYTE* truck, BYTE* site, char flag);
typedef float (*FnNeed)(BYTE* world, BYTE* site, BYTE* res, char lessTransit, float* transit);
typedef float (*FnCapacity)(BYTE* truck, BYTE* res, char full, char any);
typedef char  (*FnInsertStop)(BYTE* truck, BYTE* building, int after, char raw);
typedef void  (*FnDiscard)(BYTE* vehicle, char keep);

static FnDispatch   g_dispatch;
static FnTransit    g_transit;
static FnNeed       g_need;
static FnCapacity   g_capacity;
static FnInsertStop g_insertStop;
static FnDiscard    g_discard;

// ---------------------------------------------------------------- settings
static struct {
    int   maxExtra;         // sites added to one trip
    float maxDistance;      // m from the first site, 0 = any
    float minExtra;         // t: smaller rests are not worth a stop
    float margin;           // t loaded on top: the last site takes it (it accepts its need + 1 t)
    int   clearRest;        // throw away what is left after the last site
    int   log;
} cfg;

// ---------------------------------------------------------------- reading
template <class T> static T Get(const BYTE* p, size_t off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }
template <class T> static void Set(BYTE* p, size_t off, T v) { memcpy(p + off, &v, sizeof(T)); }

static BYTE* Ptr(const BYTE* p, size_t off) { return Get<BYTE*>(p, off); }

// A game vector at p + off: begin, end.
static size_t Count(const BYTE* p, size_t off, size_t stride)
{
    BYTE* b = Ptr(p, off);
    BYTE* e = Ptr(p, off + 8);
    return b && e > b ? (size_t)(e - b) / stride : 0;
}

static BYTE* Item(const BYTE* p, size_t off, size_t stride, size_t i) { return Ptr(p, off) + i * stride; }

static BYTE* Stop(const BYTE* truck, size_t i) { return Ptr(Item(truck, V_STOPS, 8, i), 0); }

static BYTE* Record(const BYTE* truck, size_t i) { return Item(truck, V_RECORDS, REC_SIZE, i); }

static float Distance(const BYTE* a, const BYTE* b)
{
    float dx = Get<float>(a, B_POSITION) - Get<float>(b, B_POSITION);
    float dz = Get<float>(a, B_POSITION + 8) - Get<float>(b, B_POSITION + 8);
    return sqrtf(dx * dx + dz * dz);
}

static float CargoTotal(const BYTE* truck)
{
    float t = 0.0f;
    for (size_t i = 0, n = Count(truck, V_CARGO, 16); i < n; ++i) t += Get<float>(Item(truck, V_CARGO, 16, i), 8);
    return t;
}

// ---------------------------------------------------------------- plans
// The plugin's own record of a multi-site trip: which stop gets how much.
#define MAX_EXTRA 8
struct PlanStop { BYTE* site; int index; float amount; };
struct Plan {
    BYTE*    res;
    BYTE*    source;
    int      n;
    PlanStop s[1 + MAX_EXTRA];
};

static std::unordered_map<BYTE*, Plan> g_plans;     // by truck, under g_lock

// The truck still runs the trip the plan describes.
static bool PlanValid(const BYTE* truck, const Plan& p)
{
    size_t n = Count(truck, V_STOPS, 8);
    if (n < 2 || Stop(truck, 0) != p.source) return false;
    for (int i = 0; i < p.n; ++i)
        if ((size_t)p.s[i].index >= n || Stop(truck, p.s[i].index) != p.s[i].site) return false;
    return true;
}

// ---------------------------------------------------------------- hooks
// The trip is over: past its last site, or the schedule was changed and the
// truck is on its way back to (or in) its office. What is left on board then
// goes - otherwise the truck waits in the office with it until a job for
// that resource comes up.
static bool Finished(BYTE* truck, const Plan& p)
{
    int at = Get<int>(truck, V_STOP_INDEX);
    bool valid = PlanValid(truck, p);
    bool over;
    if (valid) over = at > p.s[p.n - 1].index;
    else {
        BYTE* office = Ptr(truck, V_OFFICE);
        size_t n = Count(truck, V_STOPS, 8);
        over = office && (Ptr(truck, V_IN) == office || (at >= 0 && (size_t)at < n && Stop(truck, at) == office));
        if (!over && cfg.log) Logf("constructlog  truck %p: schedule changed, its plan is dropped", truck);
        if (!over) return true;                           // dropped, nothing thrown away
    }
    if (!over) return false;
    float left = CargoTotal(truck);
    if (cfg.clearRest && left > 0.0f) {
        g_discard(truck, 0);
        if (cfg.log) Logf("constructlog  truck %p: trip done, %.2f left on board thrown away", truck, left);
    }
    return true;
}

// FUN_1401541a0: what this truck brings to site. The game finds the site's
// first stop from the current one on: ahead -> the whole planned load, at it
// -> what is on board. With a plan: ahead -> the site's share, at it -> no
// more than the share.
static float HookTransit(BYTE* world, BYTE* res, BYTE* truck, BYTE* site, char flag)
{
    float game = g_transit(world, res, truck, site, flag);
    if (!truck || !site || flag) return game;
    EnterCriticalSection(&g_lock);
    float r = game;
    auto it = g_plans.find(truck);
    if (it != g_plans.end() && Finished(truck, it->second)) {
        g_plans.erase(it);
    }
    else if (it != g_plans.end()) {
        if (it->second.res == res) {
            const Plan& p = it->second;
            int at = Get<int>(truck, V_STOP_INDEX);
            if (at < 0) at = 0;
            int k = -1;
            for (size_t i = at, n = Count(truck, V_STOPS, 8); i < n; ++i)
                if (Stop(truck, i) == site) { k = (int)i; break; }
            for (int i = 0; i < p.n; ++i) {
                if (p.s[i].index != k) continue;
                if (at < k) r = p.s[i].amount;
                else if (game > p.s[i].amount) r = p.s[i].amount;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

struct Candidate { BYTE* site; float need; };

// A one-site trip is remembered too, so its rest goes after the site.
static void Remember(BYTE* truck, BYTE* res, BYTE* source, BYTE* site, float amount)
{
    Plan plan = {};
    plan.res = res; plan.source = source;
    plan.s[plan.n++] = { site, 1, amount };
    EnterCriticalSection(&g_lock);
    g_plans[truck] = plan;
    LeaveCriticalSection(&g_lock);
}

// Load amount t plus the margin, as a fraction of the capacity. Without the
// margin a few hundredths of a tonne go missing on the way and the office
// sends a truck again for 0.03 t.
static void SetLoad(BYTE* truck, float amount, float cap)
{
    float load = (amount + cfg.margin) / cap;
    if (load > 1.0f) load = 1.0f;
    Set<float>(Record(truck, 0), REC_LOAD, load);
}

// After the game gave the truck source -> site -> office: more sites on the way.
static const char* Extend(BYTE* world, BYTE* job, BYTE* office)
{
    BYTE* truck  = Ptr(job, J_TRUCK);
    BYTE* first  = Ptr(job, J_SITE);
    BYTE* source = Ptr(job, J_SOURCE);
    BYTE* res    = Ptr(job, J_RESOURCE);
    if (!truck || !first || !res) return "not a delivery";
    if (!source) return "no source";
    if (Get<int>(job, J_NO_RETURN) != 0) return "no return to the office";
    if (res == Ptr(world, W_WORKERS)) return "workers";
    BYTE* type = Ptr(truck, V_TYPE);
    if (!type) return "no vehicle type";
    int cls = Get<int>(type, T_CLASS);
    if (cls == 7 || cls == 0x12) return "not a cargo vehicle";
    if (Count(truck, V_STOPS, 8) != 3 || Count(truck, V_RECORDS, REC_SIZE) != 3) return "not 3 stops";
    if (Stop(truck, 0) != source || Stop(truck, 1) != first || Stop(truck, 2) != office) return "not source-site-office";

    float cap = g_capacity(truck, res, 1, 0);
    if (!(cap > 0.0f)) return "no capacity";
    float load = Get<float>(Record(truck, 0), REC_LOAD);
    if (load >= 0.999f) return "already full";
    float firstNeed = load * cap;
    // The margin stays free even when the sites could fill the truck: a full
    // truck delivers a few hundredths less than loaded, and a share planned
    // to the last tonne then leaves its site short by that much.
    float room = cap - firstNeed - cfg.margin;
    if (room < cfg.minExtra) { SetLoad(truck, firstNeed, cap); Remember(truck, res, source, first, firstNeed); return "no room"; }

    Candidate cand[64];
    int nc = 0;
    for (size_t i = 0, n = Count(office, B_OFFICE_SITES, 8); i < n && nc < 64; ++i) {
        BYTE* site = Ptr(Item(office, B_OFFICE_SITES, 8, i), 0);
        if (!site || site == first) continue;
        if (cfg.maxDistance > 0.0f && Distance(first, site) > cfg.maxDistance) continue;
        float transit = 0.0f;
        float need = g_need(world, site, res, 1, &transit);
        if (need >= cfg.minExtra) cand[nc++] = { site, need };
    }
    if (!nc) { SetLoad(truck, firstNeed, cap); Remember(truck, res, source, first, firstNeed); return "no other site needs it"; }

    Plan plan = {};
    plan.res = res; plan.source = source;
    plan.s[plan.n++] = { first, 1, firstNeed };
    BYTE* last = first;
    float extra = 0.0f;
    while (plan.n < 1 + cfg.maxExtra && room >= cfg.minExtra) {
        int best = -1;
        float bestD = 1e30f;
        for (int i = 0; i < nc; ++i) {
            if (!cand[i].site) continue;
            float d = Distance(last, cand[i].site);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) break;
        BYTE* site = cand[best].site;
        float amount = cand[best].need < room ? cand[best].need : room;
        cand[best].site = 0;

        int after = plan.s[plan.n - 1].index;
        size_t before = Count(truck, V_STOPS, 8);
        g_insertStop(truck, site, after, 1);
        if (Count(truck, V_STOPS, 8) != before + 1 || Stop(truck, after + 1) != site ||
            Count(truck, V_RECORDS, REC_SIZE) != before + 1) {
            Logf("constructlog  truck %p: stop not inserted as expected, trip left at %d sites", truck, plan.n);
            break;
        }
        // The new stop unloads, as the dispatch sets up the first site.
        BYTE* rec = Record(truck, after + 1);
        rec[REC_LOAD_ON]   = 0;
        rec[REC_UNLOAD_ON] = 1;
        Set<float>(rec, REC_UNLOAD, 1.0f);

        plan.s[plan.n++] = { site, after + 1, amount };
        room  -= amount;
        extra += amount;
        last   = site;
    }
    if (plan.n < 2) { SetLoad(truck, firstNeed, cap); Remember(truck, res, source, first, firstNeed); return "nothing inserted"; }

    SetLoad(truck, firstNeed + extra, cap);             // the inserts may have moved the records

    EnterCriticalSection(&g_lock);
    g_plans[truck] = plan;
    LeaveCriticalSection(&g_lock);

    if (cfg.log) {
        char line[512];
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "constructlog  truck %p: load %.1f of %.1f (was %.1f), sites",
                            truck, firstNeed + extra, cap, firstNeed);
        for (int i = 0; i < plan.n && k > 0; ++i)
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %p=%.1f", plan.s[i].site, plan.s[i].amount);
        Logf("%s", line);
    }
    return 0;
}

static char HookDispatch(BYTE* world, BYTE* job, BYTE* office)
{
    BYTE* truck = job ? Ptr(job, J_TRUCK) : 0;
    if (truck) {
        EnterCriticalSection(&g_lock);
        auto it = g_plans.find(truck);
        bool had = it != g_plans.end();
        if (had) g_plans.erase(it);
        LeaveCriticalSection(&g_lock);
        if (had && cfg.log) {
            float left = CargoTotal(truck);
            if (left > 0.05f) Logf("constructlog  truck %p: new job with %.2f left on board", truck, left);
        }
    }
    // The tick builds all its jobs first and hands them out one by one, so a
    // job may predate a trip extended earlier in the same tick. Its need is
    // taken again, in transit included: nothing left -> no trip, less ->
    // a smaller load.
    BYTE* site = job ? Ptr(job, J_SITE) : 0;
    BYTE* res  = job ? Ptr(job, J_RESOURCE) : 0;
    if (truck && site && res && Ptr(job, J_SOURCE) && res != Ptr(world, W_WORKERS)) {
        float transit = 0.0f;
        float fresh = g_need(world, site, res, 1, &transit);
        float had = Get<float>(job, J_NEED);
        if (fresh < 0.05f) {
            if (cfg.log) Logf("constructlog  truck %p: no trip, site %p already served (need was %.2f)", truck, site, had);
            return 0;
        }
        if (fresh < had - 0.05f) {
            float cap = g_capacity(truck, res, 1, 0);
            if (cap > 0.0f) {
                float load = fresh / cap;
                Set<float>(job, J_NEED, fresh);
                Set<float>(job, J_LOAD, load > 1.0f ? 1.0f : load);
                if (cfg.log) Logf("constructlog  truck %p: site %p needs %.2f now, not %.2f", truck, site, fresh, had);
            }
        }
    }
    char r = g_dispatch(world, job, office);
    if (r && job && office) {
        const char* why = Extend(world, job, office);
        if (why && cfg.log)
            Logf("constructlog  truck %p: one site %p (%s); need %.2f, load %.2f", Ptr(job, J_TRUCK), Ptr(job, J_SITE), why,
                 Get<float>(job, J_NEED),
                 truck && Count(truck, V_RECORDS, REC_SIZE) ? Get<float>(Record(truck, 0), REC_LOAD) : -1.0f);
    }
    return r;
}

// ---------------------------------------------------------------- patching
static BYTE* FindUnique(const short* sig, size_t n)
{
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    BYTE* found = 0;
    int hits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* p   = g_exeBase + s->VirtualAddress;
        BYTE* end = p + s->Misc.VirtualSize - n;
        for (; p <= end; ++p) {
            if (p[0] != (BYTE)sig[0]) continue;
            size_t k = 1;
            while (k < n && (sig[k] < 0 || p[k] == (BYTE)sig[k])) ++k;
            if (k == n) { found = p; ++hits; }
        }
    }
    return hits == 1 ? found : 0;
}

// Every `call rel32` in .text to target.
static int CallsTo(BYTE* target, BYTE** out, int cap)
{
    int n = 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* p   = g_exeBase + s->VirtualAddress;
        BYTE* end = p + s->Misc.VirtualSize - 5;
        for (; p <= end; ++p)
            if (p[0] == 0xE8 && p + 5 + *(INT32*)(p + 1) == target) { if (n < cap) out[n] = p; ++n; }
    }
    return n;
}

static INT32 Rel(BYTE* target, BYTE* next) { return (INT32)(target - next); }

// jmp [rip+0]; dq fn
static void WriteJump(BYTE* stub, void* fn)
{
    stub[0] = 0xFF; stub[1] = 0x25; *(INT32*)(stub + 2) = 0;
    *(void**)(stub + 6) = fn;
}

static void Redirect(BYTE* const* sites, int n, BYTE* stub)
{
    for (int i = 0; i < n; ++i) {
        DWORD old;
        VirtualProtect(sites[i], 5, PAGE_EXECUTE_READWRITE, &old);
        *(INT32*)(sites[i] + 1) = Rel(stub, sites[i] + 5);
        VirtualProtect(sites[i], 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
    }
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "constructlog";
    info->version = CL_VERSION;

    if (!H->configInt(CL_INI, "constructlog", "enabled", 1)) {
        Logf("constructlog  disabled in ini");
        return 1;
    }
    cfg.maxExtra = H->configInt(CL_INI, "constructlog", "max_extra_sites", 3);
    if (cfg.maxExtra < 0) cfg.maxExtra = 0;
    if (cfg.maxExtra > MAX_EXTRA) cfg.maxExtra = MAX_EXTRA;
    cfg.maxDistance = (float)H->configInt(CL_INI, "constructlog", "max_distance_m", 0);
    cfg.minExtra    = (float)H->configInt(CL_INI, "constructlog", "min_extra_t", 1);
    cfg.log         = H->configInt(CL_INI, "constructlog", "log", 1);
    cfg.clearRest   = H->configInt(CL_INI, "constructlog", "clear_rest", 1);
    {
        char m[32];
        H->configString(CL_INI, "constructlog", "load_margin_t", m, sizeof(m), "0.2");
        for (char* q = m; *q; ++q) if (*q == ',') *q = '.';
        cfg.margin = (float)atof(m);
        if (cfg.margin < 0.0f) cfg.margin = 0.0f;
        if (cfg.margin > 1.0f) cfg.margin = 1.0f;
    }
    Logf("constructlog  v%s init: up to %d extra sites, max distance %.0f m, min %.0f t, margin %.2f t",
         CL_VERSION, cfg.maxExtra, cfg.maxDistance, cfg.minExtra, cfg.margin);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    // Everything is found and counted before a byte is patched.
    BYTE* dispatch = FindUnique(SIG(SIG_DISPATCH));
    BYTE* transit  = FindUnique(SIG(SIG_TRANSIT));
    BYTE* need     = FindUnique(SIG(SIG_NEED));
    BYTE* capacity = FindUnique(SIG(SIG_CAPACITY));
    BYTE* insert   = FindUnique(SIG(SIG_INSERT_STOP));
    BYTE* discard  = FindUnique(SIG(SIG_DISCARD));
    if (!dispatch || !transit || !need || !capacity || !insert || !discard) {
        Logf("constructlog  not found:%s%s%s%s%s%s - nothing hooked", dispatch ? "" : " dispatch",
             transit ? "" : " transit", need ? "" : " need", capacity ? "" : " capacity", insert ? "" : " insert stop",
             discard ? "" : " discard");
        return 1;
    }
    BYTE* dispatchCalls[8];
    BYTE* transitCalls[8];
    int nd = CallsTo(dispatch, dispatchCalls, 8), nt = CallsTo(transit, transitCalls, 8);
    if (nd != DISPATCH_CALLS || nt != TRANSIT_CALLS) {
        Logf("constructlog  dispatch called %d times (%d expected), transit %d (%d expected) - nothing hooked",
             nd, DISPATCH_CALLS, nt, TRANSIT_CALLS);
        return 1;
    }
    BYTE* page = AllocNear(dispatch, 0x1000);
    if (!page) { Logf("constructlog  no memory next to the game - nothing hooked"); return 1; }

    g_dispatch   = (FnDispatch)dispatch;
    g_transit    = (FnTransit)transit;
    g_need       = (FnNeed)need;
    g_capacity   = (FnCapacity)capacity;
    g_insertStop = (FnInsertStop)insert;
    g_discard    = (FnDiscard)discard;
    WriteJump(page + 0x00, (void*)HookDispatch);
    WriteJump(page + 0x20, (void*)HookTransit);
    Redirect(dispatchCalls, nd, page + 0x00);
    Redirect(transitCalls, nt, page + 0x20);
    Logf("constructlog  dispatch +%06llX (%d calls), transit +%06llX (%d call), need +%06llX, capacity +%06llX, "
         "insert stop +%06llX: hooked",
         (unsigned long long)(dispatch - g_exeBase), nd, (unsigned long long)(transit - g_exeBase), nt,
         (unsigned long long)(need - g_exeBase), (unsigned long long)(capacity - g_exeBase),
         (unsigned long long)(insert - g_exeBase));
    return 0;
}
