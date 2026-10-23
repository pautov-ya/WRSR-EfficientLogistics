// constructlog - construction office trucks deliver to several sites in one
// trip.
//
// The office hands a job to a truck in FUN_1401561c0 (the office job
// function): it picks the site that needs a resource most, builds the
// schedule source -> site -> office and sets the load at the source to what
// that one site still needs (record 0, +4: fraction of the truck's capacity).
// A 20 t truck carrying 5 t, then 2 t, then 10 t makes three trips.
//
// The plugin lets the game choose as before, then adds up to max_extra_sites
// more sites of the same office that need the same resource, nearest first,
// between the site and the office, and raises the load to their sum. The
// game's unloading already stops at what a site needs (+1 t), so the rest
// stays on the truck for the next site.
//
// The office reserves cargo in transit through FUN_14014cdc0 (a site's need
// for a resource): it counts capacity x load of every truck whose second stop
// is that site. With a fuller truck that would reserve too much at the first
// site and nothing at the others, so its calls are redirected and the answer
// corrected from the plugin's own plan for each truck.
//
// Addresses, formulas and the reasoning: docs/findings.md.

#include "../include/tesmio_plugin.h"
#include <math.h>
#include <unordered_map>

#define CL_VERSION "0.1.3"
#define CL_INI     "plugins\\constructlog.ini"

// ---------------------------------------------------------------- signatures (1.1.1.9)
static const short SIG_OFFICE_JOB[] = {     // FUN_1401561c0(world, truck, office, int, char) -> bool
    0x48,0x8b,0xc4, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8d,0xa8,0x08,0xfd,0xff,0xff, 0x48,0x81,0xec,0xc0,0x03,0x00,0x00,
    0x48,0xc7,0x45,0x68,0xfe,0xff,0xff,0xff, 0x48,0x89,0x58,0x20,
};
static const short SIG_SITE_NEED[] = {      // FUN_14014cdc0(world, site, resource, job, ...) -> bool
    0x48,0x89,0x4c,0x24,0x08, 0x53, 0x56, 0x57, 0x48,0x83,0xec,0x30, 0x4c,0x8b,0x9c,0x24,0x88,0x00,0x00,0x00,
    0x33,0xff, 0x48,0x89,0x6c,0x24,0x58, 0x49,0x8b,0xd8, 0x4c,0x89,0x6c,0x24,0x68, 0x45,0x8b,0xe9,
};
static const short SIG_INSERT_STOP[] = {    // FUN_14067d5d0(truck, building, after, raw) -> char
    0x48,0x89,0x54,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xec,0xe0,0x01,0x00,0x00, 0x48,0xc7,0x44,0x24,0x30,0xfe,0xff,0xff,0xff,
    0x48,0x89,0x9c,0x24,0x30,0x02,0x00,0x00,
};
#define OFFICE_JOB_CALLS 8          // 2 in the office tick, 6 in a window
#define SITE_NEED_CALLS  9          // all in the office job function, one per resource

#define SIG(x) x, sizeof(x) / sizeof(x[0])

// ---------------------------------------------------------------- game layout
// vehicle
#define V_CARGO      0x3B8   // vector of 16-byte {resource*, float amount, float}
#define V_JOB        0x50C   // int: the office job (resource kind) the truck is on
#define V_RECORDS    0x638   // vector of 0x198-byte stop settings, one per stop
#define V_STOPS      0x680   // vector of building*
#define V_STOP_INDEX 0x698   // int: the stop it is going to / at
#define V_TYPE       0x1708
// vehicle type
#define T_CLASS      0x8600  // int: cargo class, picks the resource's unit
#define T_CAPACITY   0x8604  // float
// resource: float unit per cargo class at +0xCC + class * 0x20
#define R_UNIT       0xCC
// stop settings (0x198 bytes)
#define REC_SIZE     0x198
#define REC_LOAD     0x04    // float: load up to this fraction of the capacity
#define REC_FILTER   0x08    // vector of 16-byte {resource*, ...}: what is loaded here
// building
#define B_POSITION   0x3A0   // float3: its C3D_NODE (+0x320) position (+0x80)
#define B_SITE_OFFICES 0x880 // construction site: vector of office*
#define B_VEHICLES   0xC70   // vector of vehicle*: those belonging to it
#define B_OFFICE_SITES 0xF90 // construction office: vector of site*

typedef bool (*FnOfficeJob)(BYTE* world, BYTE* truck, BYTE* office, int a4, char a5);
typedef bool (*FnSiteNeed)(BYTE* world, BYTE* site, BYTE* res, int job, float* need, float* have,
                           float* progress, float* loading, BYTE* source);
typedef char (*FnInsertStop)(BYTE* truck, BYTE* building, int after, char raw);

static FnOfficeJob  g_officeJob;
static FnSiteNeed   g_siteNeed;
static FnInsertStop g_insertStop;

// ---------------------------------------------------------------- settings
static struct {
    int   maxExtra;         // sites added to one trip
    float maxDistance;      // m from the first site, 0 = any
    float minExtra;         // t: smaller rests are not worth a stop
    DWORD jobs;             // bit n: office job n may take several sites
    int   log;
    int   trace;            // every office job call, the first 200
} cfg;

// ---------------------------------------------------------------- reading
template <class T> static T Get(const BYTE* p, size_t off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }

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

static float Distance(const BYTE* a, const BYTE* b)
{
    float dx = Get<float>(a, B_POSITION) - Get<float>(b, B_POSITION);
    float dz = Get<float>(a, B_POSITION + 8) - Get<float>(b, B_POSITION + 8);
    return sqrtf(dx * dx + dz * dz);
}

// What the truck holds of res when full.
static float Capacity(const BYTE* truck, const BYTE* res)
{
    BYTE* type = Ptr(truck, V_TYPE);
    if (!type || !res) return 0.0f;
    int cls = Get<int>(type, T_CLASS);
    if (cls < 0 || cls > 64) return 0.0f;
    return Get<float>(res, R_UNIT + cls * 0x20) * Get<float>(type, T_CAPACITY);
}

static bool CarriesSlot(const BYTE* truck, const BYTE* res)
{
    for (size_t i = 0, n = Count(truck, V_CARGO, 16); i < n; ++i)
        if (Ptr(Item(truck, V_CARGO, 16, i), 0) == res) return true;
    return false;
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
    int      job;
    BYTE*    res;
    BYTE*    source;
    int      n;
    PlanStop s[1 + MAX_EXTRA];
};

static std::unordered_map<BYTE*, Plan> g_plans;     // by truck, under g_lock

// The truck still runs the trip the plan describes.
static bool PlanValid(const BYTE* truck, const Plan& p)
{
    if (Get<int>(truck, V_JOB) != p.job) return false;
    size_t n = Count(truck, V_STOPS, 8);
    if (n < 2 || Stop(truck, 0) != p.source) return false;
    for (int i = 0; i < p.n; ++i)
        if ((size_t)p.s[i].index >= n || Stop(truck, p.s[i].index) != p.s[i].site) return false;
    return true;
}

// How much the game counts for this truck at site, and how much the plan does.
static void Reserved(const BYTE* truck, const Plan& p, const BYTE* site, const BYTE* res, int job,
                     float* game, float* plan)
{
    *game = *plan = 0.0f;
    if (Get<int>(truck, V_JOB) != job) return;
    int at = Get<int>(truck, V_STOP_INDEX);
    if (Count(truck, V_STOPS, 8) > 1 && Stop(truck, 1) == site && at < 2 && CarriesSlot(truck, res) &&
        Count(truck, V_RECORDS, REC_SIZE))
        *game = Capacity(truck, res) * Get<float>(Item(truck, V_RECORDS, REC_SIZE, 0), REC_LOAD);
    if (p.res != res) return;
    for (int i = 0; i < p.n; ++i)
        if (p.s[i].site == site && at <= p.s[i].index) *plan += p.s[i].amount;
}

// Plan minus game, over every truck of every office serving the site.
static float PlanCorrection(const BYTE* site, const BYTE* res, int job)
{
    float delta = 0.0f;
    EnterCriticalSection(&g_lock);
    if (!g_plans.empty()) {
        for (size_t i = 0, n = Count(site, B_SITE_OFFICES, 8); i < n; ++i) {
            BYTE* office = Ptr(Item(site, B_SITE_OFFICES, 8, i), 0);
            if (!office) continue;
            for (size_t j = 0, m = Count(office, B_VEHICLES, 8); j < m; ++j) {
                BYTE* truck = Ptr(Item(office, B_VEHICLES, 8, j), 0);
                auto it = truck ? g_plans.find(truck) : g_plans.end();
                if (it == g_plans.end()) continue;
                if (!PlanValid(truck, it->second)) { g_plans.erase(it); continue; }
                float game, plan;
                Reserved(truck, it->second, site, res, job, &game, &plan);
                delta += plan - game;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return delta;
}

// ---------------------------------------------------------------- hooks
#define NO_NEED -1.0e30f

// FUN_14014cdc0: need = what the current phase asks for, have = delivered +
// in transit; true while need > have.
static bool HookSiteNeed(BYTE* world, BYTE* site, BYTE* res, int job, float* need, float* have,
                         float* progress, float* loading, BYTE* source)
{
    if (need) *need = NO_NEED;
    bool r = g_siteNeed(world, site, res, job, need, have, progress, loading, source);
    static LONG traced;
    if (cfg.trace && InterlockedIncrement(&traced) <= 60)
        Logf("constructlog  [trace] need site %p res %p job %d: need %.2f have %.2f -> %d", site, res, job,
             need ? *need : -1.0f, have ? *have : -1.0f, (int)r);
    if (!site || !need || !have || *need == NO_NEED) return r;      // resource not in this phase
    float delta = PlanCorrection(site, res, job);
    if (delta == 0.0f) return r;
    *have += delta;
    return *need - *have > 0.0f;
}

struct Candidate { BYTE* site; float need; };

// After the game gave the truck source -> site -> office: more sites on the way.
static const char* Extend(BYTE* world, BYTE* truck, BYTE* office)
{
    int job = Get<int>(truck, V_JOB);
    if (job < 0 || job > 31 || !(cfg.jobs & (1u << job))) return "job not in jobs";
    if (Count(truck, V_STOPS, 8) != 3 || Count(truck, V_RECORDS, REC_SIZE) != 3) return "not 3 stops";
    BYTE* source = Stop(truck, 0);
    BYTE* first  = Stop(truck, 1);
    if (!source || !first || Stop(truck, 2) != office) return "not source-site-office";

    BYTE* rec0 = Item(truck, V_RECORDS, REC_SIZE, 0);
    float load = Get<float>(rec0, REC_LOAD);
    if (load >= 0.999f) return "already full";
    if (Count(rec0, REC_FILTER, 16) == 0) return "no cargo filter";
    BYTE* res = Ptr(Item(rec0, REC_FILTER, 16, 0), 0);
    float cap = Capacity(truck, res);
    if (!(cap > 0.0f)) return "no capacity";

    // The game's load: need / capacity + 0.01.
    float firstNeed = (load - 0.01f) * cap;
    if (firstNeed < 0.0f) firstNeed = 0.0f;
    float room = cap - firstNeed;
    if (room < cfg.minExtra) return "no room";

    Candidate cand[64];
    int nc = 0;
    for (size_t i = 0, n = Count(office, B_OFFICE_SITES, 8); i < n && nc < 64; ++i) {
        BYTE* site = Ptr(Item(office, B_OFFICE_SITES, 8, i), 0);
        if (!site || site == first) continue;
        if (cfg.maxDistance > 0.0f && Distance(first, site) > cfg.maxDistance) continue;
        float need = 0.0f, have = 0.0f, progress = 0.0f;
        if (!HookSiteNeed(world, site, res, job, &need, &have, &progress, 0, source)) continue;
        if (need - have >= cfg.minExtra) cand[nc++] = { site, need - have };
    }
    if (!nc) return "no other site needs it";

    Plan plan = {};
    plan.job = job; plan.res = res; plan.source = source;
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
        // The new stop unloads like the first site.
        BYTE* from = Item(truck, V_RECORDS, REC_SIZE, 1);
        BYTE* to   = Item(truck, V_RECORDS, REC_SIZE, after + 1);
        to[0] = from[0];
        memcpy(to + 0x04, from + 0x04, 4);
        memcpy(to + 0x20, from + 0x20, 9);
        to[0xC8] = from[0xC8];
        memcpy(to + 0xCC, from + 0xCC, 4);
        memcpy(to + 0xE8, from + 0xE8, 9);

        plan.s[plan.n++] = { site, after + 1, amount };
        room  -= amount;
        extra += amount;
        last   = site;
    }
    if (plan.n < 2) return "nothing inserted";

    float newLoad = (firstNeed + extra) / cap + 0.01f;
    if (newLoad > 1.0f) newLoad = 1.0f;
    rec0 = Item(truck, V_RECORDS, REC_SIZE, 0);                 // the inserts may have moved it
    memcpy(rec0 + REC_LOAD, &newLoad, 4);

    EnterCriticalSection(&g_lock);
    g_plans[truck] = plan;
    LeaveCriticalSection(&g_lock);

    if (cfg.log) {
        char line[512];
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "constructlog  truck %p job %d: load %.1f of %.1f (was %.1f), sites",
                            truck, job, (firstNeed + extra), cap, firstNeed);
        for (int i = 0; i < plan.n && k > 0; ++i)
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %p=%.1f", plan.s[i].site, plan.s[i].amount);
        Logf("%s", line);
    }
    return 0;
}

static bool HookOfficeJob(BYTE* world, BYTE* truck, BYTE* office, int a4, char a5)
{
    static bool first = true;
    if (first) { first = false; Logf("constructlog  office job runs (truck %p, office %p, %d, %d)", truck, office, a4, a5); }
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
    bool r = g_officeJob(world, truck, office, a4, a5);
    static LONG traced;
    if (cfg.trace && truck && InterlockedIncrement(&traced) <= 200) {
        char line[512];
        size_t n = Count(truck, V_STOPS, 8);
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "constructlog  [trace] job truck %p office %p (%d,%d) -> %d: job %d, at %d, stops",
                            truck, office, a4, (int)a5, (int)r, Get<int>(truck, V_JOB), Get<int>(truck, V_STOP_INDEX));
        for (size_t i = 0; i < n && i < 8 && k > 0; ++i)
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %p", Stop(truck, i));
        if (Count(truck, V_RECORDS, REC_SIZE) && k > 0)
            _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, ", load %.2f", Get<float>(Item(truck, V_RECORDS, REC_SIZE, 0), REC_LOAD));
        Logf("%s", line);
    }
    if (r && truck && office) {
        const char* why = Extend(world, truck, office);
        if (why && cfg.log) {
            size_t n = Count(truck, V_STOPS, 8), sites = Count(office, B_OFFICE_SITES, 8);
            float load = Count(truck, V_RECORDS, REC_SIZE) ? Get<float>(Item(truck, V_RECORDS, REC_SIZE, 0), REC_LOAD) : -1.0f;
            Logf("constructlog  truck %p job %d: one site (%s); stops %d, load %.2f, office sites %d",
                 truck, Get<int>(truck, V_JOB), why, (int)n, load, (int)sites);
        }
    }
    return r;
}


// Trace: who builds an office truck's schedule. FUN_14067d5d0 itself is
// hooked; for a truck of a construction office the caller goes to the log.
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
    if (truck && ReadablePtr(truck + 0x4F8, 8)) {
        int office = TypeId(Ptr(truck, 0x4F8));
        if ((office == 0xC || office == 0x1C || office == 0x6A) && InterlockedIncrement(&traced) <= 300)
            Logf("constructlog  [trace] insert stop from +%06llX: truck %p job %d, building %p type 0x%X, after %d, raw %d, now %d stops",
                 (unsigned long long)(ret - 5 - g_exeBase), truck, Get<int>(truck, V_JOB), building, TypeId(building),
                 after, (int)raw, (int)Count(truck, V_STOPS, 8));
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
    cfg.trace       = H->configInt(CL_INI, "constructlog", "trace", 1);
    {
        char list[128];
        H->configString(CL_INI, "constructlog", "jobs", list, sizeof(list), "1,2,3,4,5,6,9,10,11");
        cfg.jobs = 0;
        for (char* tok = strtok(list, ", "); tok; tok = strtok(0, ", ")) {
            int j = atoi(tok);
            if (j >= 0 && j < 32 && tok[0] >= '0' && tok[0] <= '9') cfg.jobs |= 1u << j;
        }
    }
    Logf("constructlog  v%s init: up to %d extra sites, max distance %.0f m, min %.0f t, jobs 0x%X",
         CL_VERSION, cfg.maxExtra, cfg.maxDistance, cfg.minExtra, (unsigned)cfg.jobs);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    // Everything is found and counted before a byte is patched.
    BYTE* job    = FindUnique(SIG(SIG_OFFICE_JOB));
    BYTE* need   = FindUnique(SIG(SIG_SITE_NEED));
    BYTE* insert = FindUnique(SIG(SIG_INSERT_STOP));
    if (!job || !need || !insert) {
        Logf("constructlog  not found:%s%s%s - nothing hooked", job ? "" : " office job", need ? "" : " site need",
             insert ? "" : " insert stop");
        return 1;
    }
    BYTE* jobCalls[16];
    BYTE* needCalls[16];
    int nj = CallsTo(job, jobCalls, 16), nn = CallsTo(need, needCalls, 16);
    if (nj != OFFICE_JOB_CALLS || nn != SITE_NEED_CALLS) {
        Logf("constructlog  office job called %d times (%d expected), site need %d (%d expected) - nothing hooked",
             nj, OFFICE_JOB_CALLS, nn, SITE_NEED_CALLS);
        return 1;
    }
    BYTE* page = AllocNear(job, 0x1000);
    if (!page) { Logf("constructlog  no memory next to the game - nothing hooked"); return 1; }

    g_officeJob  = (FnOfficeJob)job;
    g_siteNeed   = (FnSiteNeed)need;
    g_insertStop = (FnInsertStop)insert;
    WriteJump(page + 0x00, (void*)HookOfficeJob);
    WriteJump(page + 0x20, (void*)HookSiteNeed);
    Redirect(jobCalls, nj, page + 0x00);
    Redirect(needCalls, nn, page + 0x20);
    if (cfg.trace) {
        static const BYTE expect[16] = { 0x48,0x89,0x54,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57 };
        void* tramp = 0;
        if (InstallInlineHook(insert, (void*)HookInsertStop, &tramp, expect, 16, "constructlog insert stop")) {
            g_insertTramp = (FnInsertStop)tramp;
            g_insertStop  = g_insertTramp;
            Logf("constructlog  [trace] insert stop hooked");
        } else Logf("constructlog  [trace] insert stop hook refused");
    }
    Logf("constructlog  office job +%06llX (%d calls), site need +%06llX (%d calls), insert stop +%06llX: hooked",
         (unsigned long long)(job - g_exeBase), nj, (unsigned long long)(need - g_exeBase), nn,
         (unsigned long long)(insert - g_exeBase));
    return 0;
}
