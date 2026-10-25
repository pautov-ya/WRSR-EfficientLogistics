// Efficient Logistics - construction office trucks deliver to several sites in
// one trip; road distribution office (supply base) trucks to several
// destinations (the logistics part below).
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

#define CL_VERSION "1.0.1"
#define CL_INI     "plugins\\efficientlogistics.ini"

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
// logistics
static const short SIG_LOG_DISPATCH[] = {   // FUN_1401e5480(world, truck, office) -> bool
    0x4c,0x8b,0xdc, 0x53, 0x55, 0x56, 0x57, 0x41,0x55, 0x48,0x83,0xec,0x70, 0x49,0x8d,0x98,0x58,0x0d,0x00,0x00,
    0x45,0x0f,0x29,0x43,0xa8, 0x48,0x8b,0x43,0x08, 0x49,0x8b,0xe8,
};
static const short SIG_ACCEPT[] = {         // FUN_1401de2b0(world, job, truck, office) -> bool
    0x40,0x53, 0x55, 0x57, 0x41,0x54, 0x48,0x83,0xec,0x48, 0x4d,0x8b,0x99,0x38,0x0d,0x00,0x00, 0x45,0x32,0xe4,
    0x48,0x63,0x42,0x18, 0x49,0x8b,0xd8,
};
static const short SIG_JOB_UPDATE[] = {     // FUN_1401e4b10(world, office, job)
    0x48,0x85,0xd2, 0x0f,0x84,0xa0,0x00,0x00,0x00, 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x57,
    0x48,0x83,0xec,0x20, 0x49,0x8b,0xf8, 0x48,0x8b,0xda,
};
// The two wrappers of FUN_140429a80 the per-tick rebuild calls (r8b = 1 / 0).
static const short SIG_REGISTER_A[] = {
    0x48,0x85,0xd2, 0x74,0x1d, 0x48,0x8b,0x82,0x08,0x17,0x00,0x00, 0x48,0x85,0xc0, 0x74,0x09,
    0x83,0xb8,0x94,0x02,0x00,0x00,0x04, 0x74,0x08, 0x41,0xb0,0x01, 0xe9,
};
static const short SIG_REGISTER_B[] = {
    0x48,0x85,0xd2, 0x74,0x1d, 0x48,0x8b,0x82,0x08,0x17,0x00,0x00, 0x48,0x85,0xc0, 0x74,0x09,
    0x83,0xb8,0x94,0x02,0x00,0x00,0x04, 0x74,0x08, 0x45,0x33,0xc0, 0xe9,
};
static const short SIG_COPY_SETTINGS[] = {  // FUN_1401e0fc0(half record, settings): a twin exists, the call makes it unique
    0x48,0x89,0x5c,0x24,0x08, 0x57, 0x48,0x83,0xec,0x20, 0x0f,0xb6,0x02, 0x48,0x8b,0xda, 0x88,0x01, 0x48,0x8b,0xf9,
    0x8b,0x42,0x04, 0x48,0x83,0xc2,0x08, 0x89,0x41,0x04, 0x48,0x83,0xc1,0x08, 0xe8,0x68,0x46,0xf0,0xff,
};
// FUN_1401e7940(_, destination, resource, truck) -> double: what a
// destination can still take, as a fraction of the truck; called once, by
// the top-up at the source FUN_1406b0240.
static const short SIG_FREE_ROOM[] = {
    0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x4c,0x24,0x08, 0x57, 0x48,0x83,0xec,0x50,
    0x49,0x8b,0xf1, 0x49,0x8b,0xf8, 0x48,0x8b,0xda, 0x48,0x85,0xd2, 0x0f,0x84,-1,-1,-1,-1,
};
#define FREE_ROOM_CALLS    1
#define LOG_DISPATCH_CALLS 2
#define REGISTER_CALLS     1        // each, in the rebuild FUN_140139a70

#define DISPATCH_CALLS 3            // 2 in the office tick, 1 elsewhere
#define TRANSIT_CALLS  1            // FUN_140154090, the in-transit sum

#define SIG(x) x, sizeof(x) / sizeof(x[0])

// ---------------------------------------------------------------- game layout
// world
#define W_WORKERS    0xC2C8  // resource*: workers
#define W_SPECIAL    0xC3B8  // resource* the reservations count as another one
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
// distribution office job (0x80 bytes, office+0xD58)
#define LJ_RESOURCE  0x00
#define LJ_SOURCE    0x10
#define LJ_DEST      0x48
#define LJ_FILLED    0x68    // double: what the destination holds + has coming
#define LJ_ROOM      0x70    // double: what it may hold
// vehicle type
#define T_CLASS      0x8600  // int: cargo class (7 = passengers, 0x11 drums, 0x12 = machines)
#define T_CAPACITY   0x8604  // float
// resource: float unit per cargo class at +0xCC + class * 0x20
#define R_UNIT       0xCC
// stop settings (0x198 bytes)
#define REC_SIZE     0x198
#define REC_LOAD_ON  0x00    // u8: load here
#define REC_LOAD     0x04    // float: load up to this fraction of the capacity
#define REC_FILTER   0x08    // vector of 16-byte {resource*, ...}: what is loaded here
#define REC_UNLOAD_ON 0xC8   // u8: unload here
#define REC_UNLOAD   0xCC    // float
// building
#define B_POSITION   0x3A0   // float3: its C3D_NODE (+0x320) position (+0x80)
#define B_OFFICE_SITES 0xF90 // construction office: vector of site*
#define B_JOBS       0xD58   // distribution office: vector of 0x80-byte jobs
#define B_LINKS      0xD38   // distribution office: vector of link*, link+0x10 -> stop settings (2 x 0xC8 + ...)
#define B_RESERVED_A 0x9B8   // vector of {resource*, float}: in transit, rebuilt every tick
#define B_RESERVED_B 0x9D0

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
    int   traceOthers;      // log the stops other vehicles get (research)
    int   logistics;        // distribution office trips too
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
        if (!over && cfg.log) Logf("EfficientLogistics  truck %p: schedule changed, its plan is dropped", truck);
        if (!over) return true;                           // dropped, nothing thrown away
    }
    if (!over) return false;
    float left = CargoTotal(truck);
    if (cfg.clearRest && left > 0.0f) {
        g_discard(truck, 0);
        if (cfg.log) Logf("EfficientLogistics  truck %p: trip done, %.2f left on board thrown away", truck, left);
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
            Logf("EfficientLogistics  truck %p: stop not inserted as expected, trip left at %d sites", truck, plan.n);
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
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "EfficientLogistics  truck %p: load %.1f of %.1f (was %.1f), sites",
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
            if (left > 0.05f) Logf("EfficientLogistics  truck %p: new job with %.2f left on board", truck, left);
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
            if (cfg.log) Logf("EfficientLogistics  truck %p: no trip, site %p already served (need was %.2f)", truck, site, had);
            return 0;
        }
        if (fresh < had - 0.05f) {
            float cap = g_capacity(truck, res, 1, 0);
            if (cap > 0.0f) {
                float load = fresh / cap;
                Set<float>(job, J_NEED, fresh);
                Set<float>(job, J_LOAD, load > 1.0f ? 1.0f : load);
                if (cfg.log) Logf("EfficientLogistics  truck %p: site %p needs %.2f now, not %.2f", truck, site, fresh, had);
            }
        }
    }
    char r = g_dispatch(world, job, office);
    if (r && job && office) {
        const char* why = Extend(world, job, office);
        if (why && cfg.log)
            Logf("EfficientLogistics  truck %p: one site %p (%s); need %.2f, load %.2f", Ptr(job, J_TRUCK), Ptr(job, J_SITE), why,
                 Get<float>(job, J_NEED),
                 truck && Count(truck, V_RECORDS, REC_SIZE) ? Get<float>(Record(truck, 0), REC_LOAD) : -1.0f);
    }
    return r;
}

// ---------------------------------------------------------------- trace (ini trace_others)
// Research for other vehicles (distribution offices ...): FUN_14067d5d0 is
// hooked inline and each stop given to a vehicle whose workplace is not a
// construction office goes to the log with the caller, the workplace's type
// id and what the stop loads / unloads. The first 400 only.
static FnInsertStop g_insertTramp;

static int TypeId(const BYTE* building)
{
    if (!building || !ReadablePtr(building + 0x318, 8)) return -1;
    BYTE* type = Ptr(building, 0x318);
    if (!type || !ReadablePtr(type + 0x360, 4)) return -1;
    return Get<int>(type, 0x360);
}

static char HookInsertStop(BYTE* truck, BYTE* building, int after, char raw)
{
    BYTE* ret = (BYTE*)__builtin_return_address(0);
    char r = g_insertTramp(truck, building, after, raw);
    static LONG traced;
    if (!truck || !ReadablePtr(truck + V_TYPE, 8)) return r;
    int work = TypeId(Ptr(truck, V_OFFICE));
    if (work == 0xC || work == 0x1C || work == 0x6A || work < 0) return r;
    BYTE* type = Ptr(truck, V_TYPE);
    int cls = type && ReadablePtr(type + T_CLASS, 4) ? Get<int>(type, T_CLASS) : -1;
    if (InterlockedIncrement(&traced) > 400) return r;
    size_t n = Count(truck, V_STOPS, 8);
    int at = -1;
    for (size_t i = 0; i < n; ++i) if (Stop(truck, i) == building) at = (int)i;
    char recs[160] = "";
    if (at >= 0 && (size_t)at < Count(truck, V_RECORDS, REC_SIZE)) {
        BYTE* rec = Record(truck, at);
        _snprintf_s(recs, sizeof(recs), _TRUNCATE, "load %d %.2f (filter %d), unload %d %.2f (filter %d)",
                    rec[REC_LOAD_ON], Get<float>(rec, REC_LOAD), (int)Count(rec, 0x08, 16),
                    rec[REC_UNLOAD_ON], Get<float>(rec, REC_UNLOAD), (int)Count(rec, 0xD0, 16));
    }
    Logf("EfficientLogistics  [trace] stop from +%06llX: vehicle %p class %d, workplace %p type 0x%X, building %p type 0x%X, "
         "after %d raw %d -> stop %d of %d; %s; cargo %.2f",
         (unsigned long long)(ret - 5 - g_exeBase), truck, cls, Ptr(truck, V_OFFICE), work, building, TypeId(building),
         after, (int)raw, at, (int)n, recs, CargoTotal(truck));
    return r;
}

// ---------------------------------------------------------------- logistics
// A road distribution office hands a truck the first job of its list that
// FUN_1401de2b0 accepts (FUN_1401e5480): source -> one destination ->
// office, the load at the source = that destination's free space. The plugin
// adds the office's other jobs with the same resource and source after the
// first destination and raises the load.
//
// Cargo in transit lives on the buildings (building+0x9D0 / +0x9B8: vector of
// {resource*, float}), rebuilt every tick from the trucks' schedules
// (FUN_140139a70 -> FUN_140429a80): an unload stop ahead counts the whole
// truck while it is empty, and a stop the plugin added counts nothing (no
// unload filter). The two calls of the rebuild are redirected and corrected
// from the plan: each destination counts its share.
typedef unsigned long long (*FnLogDispatch)(BYTE* world, BYTE* truck, BYTE* office);
typedef char (*FnAccept)(BYTE* world, BYTE* job, BYTE* truck, BYTE* office);
typedef void (*FnJobUpdate)(BYTE* world, BYTE* office, BYTE* job);
typedef void (*FnRegister)(BYTE* world, BYTE* truck);
typedef void (*FnCopySettings)(BYTE* half, BYTE* settings);

static FnLogDispatch g_logDispatch;
static FnAccept      g_accept;
static FnJobUpdate   g_jobUpdate;
static FnRegister    g_registerA;   // into building+0x9B8
static FnRegister    g_registerB;   // into building+0x9D0
static FnCopySettings g_copySettings;

static std::unordered_map<BYTE*, Plan> g_lplans;    // by truck, under g_lock

// The amount of res on board (the game takes the last entry).
static float CargoOf(const BYTE* truck, const BYTE* res)
{
    float a = 0.0f;
    for (size_t i = 0, n = Count(truck, V_CARGO, 16); i < n; ++i)
        if (Ptr(Item(truck, V_CARGO, 16, i), 0) == res) a = Get<float>(Item(truck, V_CARGO, 16, i), 8);
    return a;
}

static bool FilterHas(const BYTE* rec, size_t off, const BYTE* res)
{
    for (size_t i = 0, n = Count(rec, off, 16); i < n; ++i)
        if (Ptr(Item(rec, off, 16, i), 0) == res) return true;
    return false;
}

static void AddReserved(BYTE* building, size_t list, const BYTE* res, float delta)
{
    for (size_t i = 0, n = Count(building, list, 16); i < n; ++i) {
        BYTE* e = Item(building, list, 16, i);
        if (Ptr(e, 0) == res) { Set<float>(e, 8, Get<float>(e, 8) + delta); return; }
    }
}

static float UnitCapacity(const BYTE* truck, const BYTE* res)
{
    BYTE* type = Ptr(truck, V_TYPE);
    int cls = Get<int>(type, T_CLASS);
    if (cls < 0 || cls > 64) return 0.0f;
    return Get<float>(res, R_UNIT + cls * 0x20) * Get<float>(type, T_CAPACITY);
}

// What FUN_140429a80 puts on an unload stop for res (vehicles of class 0x11
// are not planned).
static float GameReserved(const BYTE* truck, const BYTE* rec, const BYTE* res, float unitCap)
{
    if (!rec[REC_UNLOAD_ON] || !FilterHas(rec, 0xD0, res)) return 0.0f;
    float cargo = CargoOf(truck, res);
    if (cargo <= 0.0f) return unitCap;
    if (Ptr(truck, 0x528) == 0) return cargo;
    BYTE* rec0 = Record(truck, 0);
    if (rec0[REC_LOAD_ON] == 0) return cargo;
    return unitCap * Get<float>(rec0, REC_LOAD);
}

// Plan minus game on every destination still ahead.
static void CorrectReserved(const BYTE* truck, const Plan& p, size_t list, float unitCap)
{
    int at = Get<int>(truck, V_STOP_INDEX);
    if (at < 0) return;
    float cargo = CargoOf(truck, p.res);
    for (int i = 0; i < p.n; ++i) {
        if (p.s[i].index < at) continue;
        float want = p.s[i].amount;
        if (p.s[i].index == at && cargo < want) want = cargo;
        float game = GameReserved(truck, Record(truck, p.s[i].index), p.res, unitCap);
        if (want != game) AddReserved(p.s[i].site, list, p.res, want - game);
    }
}

static void CorrectFor(BYTE* truck, size_t list)
{
    EnterCriticalSection(&g_lock);
    auto it = g_lplans.find(truck);
    if (it != g_lplans.end()) {
        const Plan& p = it->second;
        int at = Get<int>(truck, V_STOP_INDEX);
        if (!PlanValid(truck, p) || at > p.s[p.n - 1].index) g_lplans.erase(it);
        else CorrectReserved(truck, p, list, UnitCapacity(truck, p.res));
    }
    LeaveCriticalSection(&g_lock);
}


// A truck arriving at its source is topped up (FUN_1406b0240) to what its
// next destination can still take. For a storage the game takes its free
// room without the cargo already on the way (FUN_1401e7940). The office
// does not send a second truck while the first one is due, but a planned
// trip brings a share to a destination the office knows nothing of - so the
// second truck would load that share again. Taken off here; the top-up has
// already taken this truck itself off the destination's in-transit.
typedef double (*FnFreeRoom)(void* a, BYTE* dest, BYTE* res, BYTE* truck);
static FnFreeRoom g_freeRoom;

static double HookFreeRoom(void* a, BYTE* dest, BYTE* res, BYTE* truck)
{
    double r = g_freeRoom(a, dest, res, truck);
    if (!dest || !res || !truck || r <= 0.0) return r;
    int type = TypeId(dest);
    if (type == 0 || type == 0x14) return r;               // the game counts the cargo on the way there
    bool planned = false;
    EnterCriticalSection(&g_lock);
    for (auto& kv : g_lplans) {
        if (kv.first == truck || kv.second.res != res) continue;
        int at = Get<int>(kv.first, V_STOP_INDEX);
        for (int i = 0; i < kv.second.n; ++i)
            if (kv.second.s[i].site == dest && kv.second.s[i].index >= at) planned = true;
    }
    LeaveCriticalSection(&g_lock);
    if (!planned) return r;
    float coming = 0.0f;
    for (size_t i = 0, n = Count(dest, B_RESERVED_B, 16); i < n; ++i)
        if (Ptr(Item(dest, B_RESERVED_B, 16, i), 0) == res) coming = Get<float>(Item(dest, B_RESERVED_B, 16, i), 8);
    float cap = g_capacity(truck, res, 0, 0);
    if (coming <= 0.0f || !(cap > 0.0f)) return r;
    double fixed = r - coming / cap;
    if (fixed < 0.0) fixed = 0.0;
    if (cfg.log)
        Logf("EfficientLogistics  [logistics] truck %p tops up at the source: %p takes %.2f t, %.2f t of it already on the way",
             truck, dest, r * cap, coming);
    return fixed;
}

static void HookRegisterA(BYTE* world, BYTE* truck)
{
    g_registerA(world, truck);
    if (truck && !g_lplans.empty()) CorrectFor(truck, B_RESERVED_A);
}

static void HookRegisterB(BYTE* world, BYTE* truck)
{
    g_registerB(world, truck);
    if (truck && !g_lplans.empty()) CorrectFor(truck, B_RESERVED_B);
}

struct LogCandidate { BYTE* dest; BYTE* job; float free; };

#define TYPE_SUPPLY_BASE 0x2B   // road distribution office; the only kind tested

static const char* ExtendLogistics(BYTE* world, BYTE* truck, BYTE* office)
{
    if (TypeId(office) != TYPE_SUPPLY_BASE) return "not a road supply base";
    BYTE* type = Ptr(truck, V_TYPE);
    if (!type) return "no vehicle type";
    int cls = Get<int>(type, T_CLASS);
    if (cls == 7 || cls == 0x11 || cls == 0x12) return "not a plain cargo vehicle";
    if (Count(truck, V_STOPS, 8) != 3 || Count(truck, V_RECORDS, REC_SIZE) != 3) return "not 3 stops";
    BYTE* source = Stop(truck, 0);
    BYTE* first  = Stop(truck, 1);
    if (!source || !first || Stop(truck, 2) != office) return "not source-destination-office";
    BYTE* rec0 = Record(truck, 0);
    if (!rec0[REC_LOAD_ON] || Count(rec0, REC_FILTER, 16) != 1) return "not one resource loaded";
    BYTE* res = Ptr(Item(rec0, REC_FILTER, 16, 0), 0);
    if (!res || res == Ptr(world, W_SPECIAL)) return "special resource";
    if (TypeId(first) == 0x14 || TypeId(source) == 0x14) return "not a storage trip";

    float cap = g_capacity(truck, res, 0, 0);
    if (!(cap > 0.0f)) return "no capacity";
    float load = Get<float>(rec0, REC_LOAD);
    if (load >= 0.999f) return "already full";
    float firstShare = load * cap;
    float room = cap - firstShare;
    if (room < cfg.minExtra) return "no room";
    // The dispatch sets every stop up from the office's first link.
    BYTE* settings = 0;
    if (Count(office, B_LINKS, 8)) {
        BYTE* link = Ptr(Item(office, B_LINKS, 8, 0), 0);
        if (link) settings = Ptr(link, 0x10);
    }

    LogCandidate cand[64];
    int nc = 0;
    for (size_t i = 0, n = Count(office, B_JOBS, 0x80); i < n && nc < 64; ++i) {
        BYTE* job = Item(office, B_JOBS, 0x80, i);
        BYTE* dest = Ptr(job, LJ_DEST);
        if (Ptr(job, LJ_RESOURCE) != res || Ptr(job, LJ_SOURCE) != source || !dest || dest == first) continue;
        if (TypeId(dest) == 0x14) continue;
        bool known = false;
        for (int k = 0; k < nc; ++k) if (cand[k].dest == dest) known = true;
        if (known) continue;
        if (cfg.maxDistance > 0.0f && Distance(first, dest) > cfg.maxDistance) continue;
        if (!g_accept(world, job, truck, office)) continue;
        float free = (float)(Get<double>(job, LJ_ROOM) - Get<double>(job, LJ_FILLED)) - 0.05f;
        if (free >= cfg.minExtra) cand[nc++] = { dest, job, free };
    }
    if (!nc) return "no other destination wants it";

    Plan plan = {};
    plan.res = res; plan.source = source;
    plan.s[plan.n++] = { first, 1, firstShare };
    BYTE* jobs[1 + MAX_EXTRA];
    BYTE* last = first;
    float extra = 0.0f;
    while (plan.n < 1 + cfg.maxExtra && room >= cfg.minExtra) {
        int best = -1;
        float bestD = 1e30f;
        for (int i = 0; i < nc; ++i) {
            if (!cand[i].dest) continue;
            float d = Distance(last, cand[i].dest);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) break;
        BYTE* dest = cand[best].dest;
        float amount = cand[best].free < room ? cand[best].free : room;
        BYTE* job = cand[best].job;
        cand[best].dest = 0;

        int after = plan.s[plan.n - 1].index;
        size_t before = Count(truck, V_STOPS, 8);
        g_insertStop(truck, dest, after, 1);
        if (Count(truck, V_STOPS, 8) != before + 1 || Stop(truck, after + 1) != dest ||
            Count(truck, V_RECORDS, REC_SIZE) != before + 1) {
            Logf("EfficientLogistics  truck %p: stop not inserted as expected, trip left at %d destinations", truck, plan.n);
            break;
        }
        // Set up as the dispatch does its destination: the office's
        // settings (fill limits and the other conditions) copied in, the
        // unload filter left empty.
        BYTE* rec = Record(truck, after + 1);
        if (settings) {
            g_copySettings(rec, settings);
            g_copySettings(rec + REC_UNLOAD_ON, settings + 200);
            Set<BYTE*>(rec, 0xD8, Ptr(rec, 0xD0));
        }
        Set<BYTE*>(rec, 0x190, 0);
        rec[REC_LOAD_ON]   = 0;
        rec[REC_UNLOAD_ON] = 1;
        Set<float>(rec, REC_LOAD, 1.0f);
        Set<float>(rec, REC_UNLOAD, 1.0f);
        if (Get<BYTE>(job, 0x78)) rec[0xF0] = 0;

        jobs[plan.n] = job;
        plan.s[plan.n++] = { dest, after + 1, amount };
        room  -= amount;
        extra += amount;
        last   = dest;
    }
    if (plan.n < 2) return "nothing inserted";

    float newLoad = (firstShare + extra) / cap;
    if (newLoad > 1.0f) newLoad = 1.0f;
    float unitCap = UnitCapacity(truck, res);
    rec0 = Record(truck, 0);
    // The dispatch registered the trip already (FUN_140429a80 into +0x9D0):
    // the source gives out the bigger load, each destination its share.
    AddReserved(source, B_RESERVED_B, res, -unitCap * (newLoad - load));
    Set<float>(rec0, REC_LOAD, newLoad);

    EnterCriticalSection(&g_lock);
    g_lplans[truck] = plan;
    CorrectReserved(truck, plan, B_RESERVED_B, unitCap);
    LeaveCriticalSection(&g_lock);
    for (int i = 1; i < plan.n; ++i) g_jobUpdate(world, office, jobs[i]);

    if (cfg.log) {
        char line[512];
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "EfficientLogistics  [logistics] truck %p: load %.1f of %.1f (was %.1f), destinations",
                            truck, firstShare + extra, cap, firstShare);
        for (int i = 0; i < plan.n && k > 0; ++i)
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %p=%.1f", plan.s[i].site, plan.s[i].amount);
        Logf("%s", line);
    }
    return 0;
}

static unsigned long long HookLogDispatch(BYTE* world, BYTE* truck, BYTE* office)
{
    if (truck) {
        EnterCriticalSection(&g_lock);
        g_lplans.erase(truck);
        LeaveCriticalSection(&g_lock);
    }
    unsigned long long r = g_logDispatch(world, truck, office);
    if ((r & 0xFF) && truck && office) {
        const char* why = ExtendLogistics(world, truck, office);
        if (why && cfg.log) {
            float load = Count(truck, V_RECORDS, REC_SIZE) ? Get<float>(Record(truck, 0), REC_LOAD) : -1.0f;
            Logf("EfficientLogistics  [logistics] truck %p: one destination %p (%s); load %.2f", truck,
                 Count(truck, V_STOPS, 8) > 1 ? Stop(truck, 1) : (BYTE*)0, why, load);
        }
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

// "0,5" works too.
static float ConfigFloat(const char* key, float fallback, float lo, float hi)
{
    char s[32];
    if (!H->configString(CL_INI, "efficientlogistics", key, s, sizeof(s), "") || !s[0]) return fallback;
    for (char* q = s; *q; ++q) if (*q == ',') *q = '.';
    float v = (float)atof(s);
    return v < lo ? lo : v > hi ? hi : v;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "efficientlogistics";
    info->version = CL_VERSION;

    if (!H->configInt(CL_INI, "efficientlogistics", "enabled", 1)) {
        Logf("EfficientLogistics  disabled in ini");
        return 1;
    }
    cfg.maxExtra = H->configInt(CL_INI, "efficientlogistics", "max_extra_sites", 3);
    if (cfg.maxExtra < 0) cfg.maxExtra = 0;
    if (cfg.maxExtra > MAX_EXTRA) cfg.maxExtra = MAX_EXTRA;
    cfg.maxDistance = (float)H->configInt(CL_INI, "efficientlogistics", "max_distance_m", 0);
    cfg.minExtra    = ConfigFloat("min_extra_t", 0.1f, 0.01f, 100.0f);
    cfg.log         = H->configInt(CL_INI, "efficientlogistics", "log", 0);
    cfg.clearRest   = H->configInt(CL_INI, "efficientlogistics", "clear_rest", 1);
    cfg.traceOthers = H->configInt(CL_INI, "efficientlogistics", "trace_others", 0);
    cfg.logistics   = H->configInt(CL_INI, "efficientlogistics", "logistics", 1);
    cfg.margin      = ConfigFloat("load_margin_t", 0.2f, 0.0f, 1.0f);
    Logf("EfficientLogistics  v%s init: up to %d extra sites, max distance %.0f m, min %.2f t, margin %.2f t",
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
        Logf("EfficientLogistics  not found:%s%s%s%s%s%s - nothing hooked", dispatch ? "" : " dispatch",
             transit ? "" : " transit", need ? "" : " need", capacity ? "" : " capacity", insert ? "" : " insert stop",
             discard ? "" : " discard");
        return 1;
    }
    BYTE* dispatchCalls[8];
    BYTE* transitCalls[8];
    int nd = CallsTo(dispatch, dispatchCalls, 8), nt = CallsTo(transit, transitCalls, 8);
    if (nd != DISPATCH_CALLS || nt != TRANSIT_CALLS) {
        Logf("EfficientLogistics  dispatch called %d times (%d expected), transit %d (%d expected) - nothing hooked",
             nd, DISPATCH_CALLS, nt, TRANSIT_CALLS);
        return 1;
    }
    BYTE* page = AllocNear(dispatch, 0x1000);
    if (!page) { Logf("EfficientLogistics  no memory next to the game - nothing hooked"); return 1; }

    g_dispatch   = (FnDispatch)dispatch;
    g_transit    = (FnTransit)transit;
    g_need       = (FnNeed)need;
    g_capacity   = (FnCapacity)capacity;
    g_insertStop = (FnInsertStop)insert;
    g_discard    = (FnDiscard)discard;
    if (cfg.traceOthers) {
        static const BYTE expect[16] = { 0x48,0x89,0x54,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57 };
        void* tramp = 0;
        if (InstallInlineHook(insert, (void*)HookInsertStop, &tramp, expect, 16, "EfficientLogistics insert stop")) {
            g_insertTramp = (FnInsertStop)tramp;
            g_insertStop  = g_insertTramp;
            Logf("EfficientLogistics  [trace] other vehicles' stops are logged");
        } else Logf("EfficientLogistics  [trace] insert stop hook refused");
    }
    if (cfg.logistics) {
        BYTE* ld  = FindUnique(SIG(SIG_LOG_DISPATCH));
        BYTE* acc = FindUnique(SIG(SIG_ACCEPT));
        BYTE* upd = FindUnique(SIG(SIG_JOB_UPDATE));
        BYTE* ra  = FindUnique(SIG(SIG_REGISTER_A));
        BYTE* rb  = FindUnique(SIG(SIG_REGISTER_B));
        BYTE* cs  = FindUnique(SIG(SIG_COPY_SETTINGS));
        BYTE* ldCalls[8];
        BYTE* raCalls[8];
        BYTE* rbCalls[8];
        int nl = ld ? CallsTo(ld, ldCalls, 8) : 0;
        int na = ra ? CallsTo(ra, raCalls, 8) : 0;
        int nb = rb ? CallsTo(rb, rbCalls, 8) : 0;
        if (!ld || !acc || !upd || !ra || !rb || !cs || nl != LOG_DISPATCH_CALLS || na != REGISTER_CALLS || nb != REGISTER_CALLS) {
            Logf("EfficientLogistics  [logistics] not as expected (dispatch %d calls, register %d / %d) - logistics off", nl, na, nb);
        } else {
            g_logDispatch = (FnLogDispatch)ld;
            g_accept      = (FnAccept)acc;
            g_jobUpdate   = (FnJobUpdate)upd;
            g_registerA   = (FnRegister)ra;
            g_registerB   = (FnRegister)rb;
            g_copySettings = (FnCopySettings)cs;
            WriteJump(page + 0x40, (void*)HookLogDispatch);
            WriteJump(page + 0x60, (void*)HookRegisterA);
            WriteJump(page + 0x80, (void*)HookRegisterB);
            Redirect(ldCalls, nl, page + 0x40);
            Redirect(raCalls, na, page + 0x60);
            Redirect(rbCalls, nb, page + 0x80);
            BYTE* fr = FindUnique(SIG(SIG_FREE_ROOM));
            BYTE* frCalls[8];
            int nf = fr ? CallsTo(fr, frCalls, 8) : 0;
            if (nf == FREE_ROOM_CALLS) {
                g_freeRoom = (FnFreeRoom)fr;
                WriteJump(page + 0xA0, (void*)HookFreeRoom);
                Redirect(frCalls, nf, page + 0xA0);
            } else Logf("EfficientLogistics  [logistics] top-up at the source not found (%d calls) - left as it is", nf);
            Logf("EfficientLogistics  [logistics] dispatch +%06llX (%d calls), register +%06llX / +%06llX, top-up %s: hooked",
                 (unsigned long long)(ld - g_exeBase), nl, (unsigned long long)(ra - g_exeBase),
                 (unsigned long long)(rb - g_exeBase), g_freeRoom ? "yes" : "no");
        }
    }
    WriteJump(page + 0x00, (void*)HookDispatch);
    WriteJump(page + 0x20, (void*)HookTransit);
    Redirect(dispatchCalls, nd, page + 0x00);
    Redirect(transitCalls, nt, page + 0x20);
    Logf("EfficientLogistics  dispatch +%06llX (%d calls), transit +%06llX (%d call), need +%06llX, capacity +%06llX, "
         "insert stop +%06llX: hooked",
         (unsigned long long)(dispatch - g_exeBase), nd, (unsigned long long)(transit - g_exeBase), nt,
         (unsigned long long)(need - g_exeBase), (unsigned long long)(capacity - g_exeBase),
         (unsigned long long)(insert - g_exeBase));
    return 0;
}
