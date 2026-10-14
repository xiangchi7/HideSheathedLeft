// ============================================================================
//  HideSheathedLeft  --  Dark Souls III 1.15.0 (Steam)
// ----------------------------------------------------------------------------
//  Effect: when the item held in the LEFT hand is in its "sheathed" state
//  (the classic case: the player two-hands the right-hand weapon, so the left
//  weapon/shield would normally be shown hanging on the back), its model is
//  hidden completely instead.
//
//  Mechanism (verified in game and against DSAnimStudio's reimplementation of
//  the engine's character assembler):
//
//    The param WepAbsorpPosParam ("WEP_ABSORP_POS_PARAM_ST") stores, for every
//    weapon and every model 0..3, the dummy-poly id the model is attached to in
//    each stance.  The engine picks the cluster purely from the weapon style:
//        OneHand   : left slot -> Left       right slot -> Right
//        RightBoth : left slot -> LeftHang   right slot -> RightBoth
//        LeftBoth  : left slot -> LeftBoth   right slot -> RightHang
//    If the chosen dummy poly id is invalid, the engine scales the model to 0
//    and skips drawing it.  So pointing the LeftHang fields at -1 makes "left
//    hand sheathed" render as nothing, while every other stance (weapon really
//    held in the left hand) is untouched.
//
//    Paired weapons: the spare blade is model 1 of the same weapon; while the
//    weapon is used one-handed in the RIGHT hand that model follows `right_1`
//    (`right_2`/`right_3` for models 2/3), so pointing those at -1 hides it.
//
//  Nothing is code-patched and no function is hooked -- only the param table
//  (heap data) is edited, so there is nothing for Arxan to trip over.
//
//  Every field the mod touches is snapshotted the first time it is seen, so an
//  option can be switched off again at runtime and the original values are put
//  back (no game restart needed).
//
//  Row layout of WEP_ABSORP_POS_PARAM_ST (row size 0x60):
//      0x04 right_0   0x0E right_1   0x18 right_2   0x22 right_3
//      0x0A leftHang_0 0x14 leftHang_1 0x1E leftHang_2 0x28 leftHang_3
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char     kParamName[]   = "WEP_ABSORP_POS_PARAM_ST";
constexpr uint64_t kLengthOff     = 0x0A;   // u16 row count inside ParamTable
constexpr uint64_t kNameOffsetOff = 0x10;   // u64 offset of the snake-case name
constexpr uint64_t kRowInfoOff    = 0x40;   // ParamRowInfo[] { u64 id, u64 off, u64 unk }
constexpr uint64_t kRowInfoSize   = 0x18;
constexpr uint32_t kMaxRows       = 40000;
constexpr uint32_t kRowSize       = 0x60;
constexpr uint16_t kInvalidPoly   = 0xFFFF; // -1 as s16: "no such dummy poly"
constexpr uint32_t kScanChunk     = 4u << 20;
constexpr uint64_t kResolveWindow = 0x100000;

// The fields the mod manages: 4 left-hang slots + 3 paired off-hand models.
constexpr int kFieldCount = 7;
const int  kFieldOff[kFieldCount] = {0x0A, 0x14, 0x1E, 0x28, 0x0E, 0x18, 0x22};
const bool kFieldIsLeftHang[kFieldCount] = {true, true, true, true,
                                            false, false, false};

struct Config {
    bool hideLeftHang      = true;
    bool hidePairedOffhand = true;
    bool log               = true;
    int  intervalMs        = 4000;
};

struct TableState {
    uint64_t addr       = 0;
    uint16_t len        = 0;
    uint64_t nameOffset = 0;
    bool     valid      = false;
    std::vector<uint8_t>  have;   // per row: snapshot taken?
    std::vector<uint16_t> orig;   // per row: kFieldCount original values
};

Config       g_cfg;
std::wstring g_iniPath;
std::wstring g_logPath;
CRITICAL_SECTION g_logLock;
bool         g_logLockReady = false;

// ---------------------------------------------------------------- utilities
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Log(const char* fmt, ...) {
    if (!g_cfg.log || !g_logLockReady) return;
    EnterCriticalSection(&g_logLock);
    if (FILE* f = _wfopen(g_logPath.c_str(), L"a, ccs=UTF-8")) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        fwprintf(f, L"%hs\n", buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

std::wstring ModuleDir(HMODULE mod) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
}

void LoadConfig() {
    Config c;
    c.hideLeftHang      = GetPrivateProfileIntW(L"general", L"HideLeftHang", 1, g_iniPath.c_str()) != 0;
    c.hidePairedOffhand = GetPrivateProfileIntW(L"general", L"HidePairedOffhand", 1, g_iniPath.c_str()) != 0;
    c.log               = GetPrivateProfileIntW(L"general", L"Log", 1, g_iniPath.c_str()) != 0;
    int sec = (int)GetPrivateProfileIntW(L"general", L"IntervalSeconds", 4, g_iniPath.c_str());
    if (sec < 1) sec = 1;
    if (sec > 600) sec = 600;
    c.intervalMs = sec * 1000;

    const bool changed = (c.hideLeftHang != g_cfg.hideLeftHang) ||
                         (c.hidePairedOffhand != g_cfg.hidePairedOffhand) ||
                         (c.intervalMs != g_cfg.intervalMs) ||
                         (c.log != g_cfg.log);
    g_cfg = c;
    if (changed) {
        Log("config: HideLeftHang=%d HidePairedOffhand=%d IntervalSeconds=%d Log=%d",
            (int)g_cfg.hideLeftHang, (int)g_cfg.hidePairedOffhand,
            g_cfg.intervalMs / 1000, (int)g_cfg.log);
    }
}

// ------------------------------------------------------- param table access
bool ResolveTableFromName(uint64_t nameAddr, uint64_t* outTable) {
    if (nameAddr <= kResolveWindow) return false;
    const uint64_t start  = nameAddr - kResolveWindow;
    const size_t   strIdx = (size_t)(nameAddr - start);
    const size_t   n      = strIdx + 16;
    std::vector<uint8_t> buf(n);
    if (!SafeCopy(buf.data(), (const void*)start, n)) return false;

    for (uint64_t delta = kRowInfoOff; delta + 0x18 <= n && delta <= strIdx; ++delta) {
        const size_t base = strIdx - (size_t)delta;
        uint64_t nameOffset = 0;
        memcpy(&nameOffset, buf.data() + base + kNameOffsetOff, 8);
        if (nameOffset != delta) continue;
        uint16_t len = 0;
        memcpy(&len, buf.data() + base + kLengthOff, 2);
        if (len == 0 || len > kMaxRows) continue;
        *outTable = start + base;
        return true;
    }
    return false;
}

bool ValidateTable(TableState& st, char* nameOut, size_t nameOutSize) {
    uint16_t len = 0;
    if (!SafeCopy(&len, (const void*)(st.addr + kLengthOff), 2)) return false;
    if (len == 0 || len > kMaxRows) return false;
    uint64_t nameOffset = 0;
    if (!SafeCopy(&nameOffset, (const void*)(st.addr + kNameOffsetOff), 8)) return false;
    if (nameOffset < kRowInfoOff || nameOffset > kResolveWindow) return false;
    char name[64] = {0};
    if (!SafeCopy(name, (const void*)(st.addr + nameOffset), sizeof(name) - 1)) return false;
    if (strcmp(name, kParamName) != 0) return false;
    st.len = len;
    st.nameOffset = nameOffset;
    st.valid = true;
    if (nameOut) {
        strncpy_s(nameOut, nameOutSize, name, _TRUNCATE);
    }
    return true;
}

// Apply the current config to one table, snapshotting originals on first sight
// so options can be turned off again at runtime.  Values are only written when
// they actually differ, so after the first pass the mod is completely passive.
int ApplyTable(TableState& st, int* writesOut) {
    if (!ValidateTable(st, nullptr, 0)) return -1;

    if (st.have.size() != st.len) {
        st.have.assign(st.len, 0);
        st.orig.assign((size_t)st.len * kFieldCount, 0);
    }

    const uint64_t dataStart = st.addr + kRowInfoOff + (uint64_t)st.len * kRowInfoSize;
    const uint64_t dataEnd   = st.addr + st.nameOffset;
    (void)dataStart;

    int rows = 0, writes = 0;
    for (uint16_t i = 0; i < st.len; ++i) {
        const uint64_t info = st.addr + kRowInfoOff + (uint64_t)i * kRowInfoSize;
        uint64_t rowId = 0, rowOff = 0;
        if (!SafeCopy(&rowId, (const void*)info, 8)) continue;
        if (!SafeCopy(&rowOff, (const void*)(info + 8), 8)) continue;
        const uint64_t row = st.addr + rowOff;
        if (rowOff < kRowInfoOff + (uint64_t)st.len * kRowInfoSize) continue;
        if (row + kRowSize > dataEnd) continue;

        uint8_t* p = (uint8_t*)row;
        if (!st.have[i]) {
            for (int f = 0; f < kFieldCount; ++f) {
                uint16_t v = 0;
                if (!SafeCopy(&v, p + kFieldOff[f], 2)) { v = 0; }
                st.orig[(size_t)i * kFieldCount + f] = v;
            }
            st.have[i] = 1;
        }
        for (int f = 0; f < kFieldCount; ++f) {
            const bool want = kFieldIsLeftHang[f] ? g_cfg.hideLeftHang
                                                  : g_cfg.hidePairedOffhand;
            const uint16_t target = want ? kInvalidPoly
                                         : st.orig[(size_t)i * kFieldCount + f];
            uint16_t cur = 0;
            if (SafeCopy(&cur, p + kFieldOff[f], 2) && cur == target) continue;
            *(uint16_t*)(p + kFieldOff[f]) = target;
            ++writes;
        }
        ++rows;
    }
    if (writesOut) *writesOut += writes;
    return rows;
}

std::vector<uint64_t> ScanForTables() {
    std::vector<uint64_t> found;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t nameLen = sizeof(kParamName) - 1;
    std::vector<uint8_t> buf(kScanChunk + nameLen);

    uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
    const uint8_t* maxAddr = (const uint8_t*)si.lpMaximumApplicationAddress;

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        const size_t size = mbi.RegionSize;

        const bool usable = mbi.State == MEM_COMMIT &&
                            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
                            !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS);
        if (usable && size) {
            for (size_t off = 0; off < size; off += kScanChunk) {
                size_t n = size - off;
                if (n > kScanChunk + nameLen) n = kScanChunk + nameLen;
                if (!SafeCopy(buf.data(), base + off, n)) continue;

                size_t i = 0;
                while (i + nameLen <= n) {
                    void* hit = memchr(buf.data() + i, kParamName[0], n - i - (nameLen - 1));
                    if (!hit) break;
                    const size_t idx = (size_t)((uint8_t*)hit - buf.data());
                    if (memcmp(buf.data() + idx, kParamName, nameLen) == 0) {
                        uint64_t table = 0;
                        if (ResolveTableFromName((uint64_t)(base + off + idx), &table)) {
                            bool dup = false;
                            for (uint64_t t : found) if (t == table) { dup = true; break; }
                            if (!dup) found.push_back(table);
                        }
                    }
                    i = idx + 1;
                }
            }
        }
        if (!size) break;
        addr = base + size;
    }
    return found;
}

// --------------------------------------------------------------- worker
DWORD WINAPI Worker(LPVOID) {
    std::vector<TableState> tables;

    for (int attempt = 0; attempt < 600; ++attempt) {
        std::vector<uint64_t> found = ScanForTables();
        if (!found.empty()) {
            tables.clear();
            for (uint64_t t : found) {
                TableState st;
                st.addr = t;
                tables.push_back(st);
            }
            break;
        }
        Sleep(1000);
    }
    Log("param tables located: %zu", tables.size());
    if (tables.empty()) Log("WARNING: %s not found (params not loaded?)", kParamName);

    for (;;) {
        LoadConfig();
        int live = 0, rowsTotal = 0, writes = 0;
        for (TableState& st : tables) {
            const int n = ApplyTable(st, &writes);
            if (n > 0) { ++live; rowsTotal += n; }
            else { st.valid = false; }
        }
        if (writes > 0) {
            Log("applied %d field write(s) over %d row(s) in %d table(s)",
                writes, rowsTotal, live);
        }
        if (!tables.empty() && live == 0) {
            Log("tables went stale, rescanning");
            std::vector<uint64_t> found = ScanForTables();
            tables.clear();
            for (uint64_t t : found) {
                TableState st;
                st.addr = t;
                tables.push_back(st);
            }
            Log("rescan found %zu", tables.size());
        }
        Sleep((DWORD)g_cfg.intervalMs);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;
        const std::wstring dir = ModuleDir(mod);
        g_iniPath = dir + L"\\HideSheathedLeft.ini";
        g_logPath = dir + L"\\HideSheathedLeft.log";
        LoadConfig();
        Log("--- HideSheathedLeft loaded (pid %lu) ---", GetCurrentProcessId());
        if (HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
            CloseHandle(th);
        }
    }
    return TRUE;
}
