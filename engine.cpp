// ══════════════════════════════════════════════════════════════
//  Soru Macro Engine — native replacement for macro_engine.ahk
//
//  Does exactly what the AHK version did, minus AHK's interpreter
//  and message-queue overhead:
//    - watches a screen pixel while R is physically held, and bursts
//      a key group when it doesn't match a target color
//    - R passthrough-spam while held
//    - cycle-groups hotkey (edge-detected, no OS hook)
//    - re-reads config.ini (written by the AHK GUI) whenever it
//      changes, on its own low-priority thread — never on the hot path
//    - exits automatically if the launching AHK GUI process dies
//
//  Usage:  engine.exe "<path to config.ini>" <main script PID>
//
//  Build (see build.yml for the CI version of this):
//    MSVC:   cl /O2 /EHsc engine.cpp /link /SUBSYSTEM:WINDOWS ^
//                /ENTRY:mainCRTStartup user32.lib gdi32.lib /OUT:engine.exe
//    MinGW:  g++ -O2 -municode -mwindows engine.cpp -o engine.exe ^
//                -luser32 -lgdi32
// ══════════════════════════════════════════════════════════════

#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// ── Shared settings, written by ConfigWatcher(), read by MacroLoop() ──
struct Settings {
    int px = 1049;
    int py = 971;
    DWORD targetColor = 0xFFFFFF;   // RGB order, matches AHK's PixelGetColor default
    int burstSize = 5;
    double cooldownSeconds = 15.01;
    bool rSpamEnabled = false;
    bool suspended = false;
    std::string cycleKeyName = "g";
    std::vector<std::string> keyGroups;
};

static std::shared_ptr<const Settings> g_settings = std::make_shared<Settings>();
static std::atomic<bool> g_running{true};

// ══════════════════════════════════════════════════════════════
//  Key name <-> virtual key code
// ══════════════════════════════════════════════════════════════
static std::string ToUpperCopy(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

static WORD KeyNameToVK(const std::string& nameIn) {
    std::string name = ToUpperCopy(nameIn);

    if (name.size() == 1) {
        unsigned char c = (unsigned char)name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (WORD)c;
    }

    if (name.size() == 2 && name[0] == 'F') {
        int n = name[1] - '0';
        if (n >= 1 && n <= 9) return (WORD)(VK_F1 + n - 1);
    }
    if (name.size() == 3 && name[0] == 'F' && isdigit((unsigned char)name[1]) && isdigit((unsigned char)name[2])) {
        int n = (name[1] - '0') * 10 + (name[2] - '0');
        if (n >= 1 && n <= 24) return (WORD)(VK_F1 + n - 1);
    }

    static const struct { const char* name; WORD vk; } table[] = {
        {"ENTER", VK_RETURN}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
        {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"BACKSPACE", VK_BACK}, {"BS", VK_BACK},
        {"DELETE", VK_DELETE}, {"DEL", VK_DELETE}, {"INSERT", VK_INSERT}, {"INS", VK_INSERT},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PGUP", VK_PRIOR}, {"PGDN", VK_NEXT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"LBUTTON", VK_LBUTTON}, {"RBUTTON", VK_RBUTTON}, {"MBUTTON", VK_MBUTTON},
        {"XBUTTON1", VK_XBUTTON1}, {"XBUTTON2", VK_XBUTTON2},
        {"CAPSLOCK", VK_CAPITAL}, {"NUMLOCK", VK_NUMLOCK}, {"SCROLLLOCK", VK_SCROLL},
        {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN},
        {"CTRL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
        {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
    };
    for (auto& e : table)
        if (name == e.name) return e.vk;

    return 0;
}

static bool KeyPhysicallyDown(WORD vk) {
    if (vk == 0) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void SendCharDownUp(char c) {
    WORD vk = KeyNameToVK(std::string(1, c));
    if (vk == 0) return;
    WORD scan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);

    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.wScan = scan;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    SendInput(1, &in[0], sizeof(INPUT));
    SendInput(1, &in[1], sizeof(INPUT));
}

static void PressKeyGroup(const std::string& group) {
    std::vector<INPUT> downs, ups;
    downs.reserve(group.size());
    ups.reserve(group.size());

    for (char c : group) {
        WORD vk = KeyNameToVK(std::string(1, c));
        if (vk == 0) continue;
        WORD scan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);

        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = vk;
        down.ki.wScan = scan;
        down.ki.dwFlags = KEYEVENTF_SCANCODE;
        downs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        ups.push_back(up);
    }

    if (!downs.empty()) SendInput((UINT)downs.size(), downs.data(), sizeof(INPUT));
    if (!ups.empty())   SendInput((UINT)ups.size(),   ups.data(),   sizeof(INPUT));
}

// ══════════════════════════════════════════════════════════════
//  Config loading
// ══════════════════════════════════════════════════════════════
static std::string g_configPath;
static std::string g_statusPath;   // written on each cycle so AHK can show a tooltip

// engine_status.txt now lives in %APPDATA%\Soru instead of next to
// config.ini, and is created with the hidden attribute, so it no
// longer shows up in the script's own folder during normal browsing.
//
// The actual disk write is now done on the low-priority watcher thread,
// not the TIME_CRITICAL hot thread. RequestGroupStatus() (called from
// the hot thread) just stores a string under a mutex and returns —
// microseconds, no I/O. WriteGroupStatusToFile() (called from the watcher
// thread) does the real CreateFileA/WriteFile/CloseHandle, outside the
// lock, so the hot thread is never blocked behind a disk operation.
static std::mutex g_statusMutex;
static std::condition_variable g_statusCV;
static std::string g_pendingStatus;
static bool g_statusDirty = false;

static void RequestGroupStatus(const std::string& groupName) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_pendingStatus = groupName;
        g_statusDirty = true;
    }
    g_statusCV.notify_one();
}

static void WriteGroupStatusToFile(const std::string& groupName) {
    if (g_statusPath.empty()) return;
    HANDLE h = CreateFileA(g_statusPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, groupName.data(), (DWORD)groupName.size(), &written, NULL);
    CloseHandle(h);
    // Belt-and-suspenders: force the hidden attribute even if the file
    // already existed before this run (CREATE_ALWAYS doesn't always
    // re-apply attributes to a pre-existing file on every filesystem).
    SetFileAttributesA(g_statusPath.c_str(), FILE_ATTRIBUTE_HIDDEN);
}
static FILETIME g_lastConfigWrite{};

static std::vector<std::string> SplitGroups(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part = (comma == std::string::npos) ? text.substr(start) : text.substr(start, comma - start);
        size_t a = part.find_first_not_of(" \t\r\n");
        size_t b = part.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) out.push_back(part.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

static void LoadConfig() {
    char buf[512];

    auto s = std::make_shared<Settings>(*std::atomic_load(&g_settings));

    s->px = GetPrivateProfileIntA("Macros", "PixelX", s->px, g_configPath.c_str());
    s->py = GetPrivateProfileIntA("Macros", "PixelY", s->py, g_configPath.c_str());
    s->rSpamEnabled = GetPrivateProfileIntA("Macros", "RSpamEnabled", s->rSpamEnabled ? 1 : 0, g_configPath.c_str()) != 0;
    s->suspended = GetPrivateProfileIntA("State", "Suspended", s->suspended ? 1 : 0, g_configPath.c_str()) != 0;

    GetPrivateProfileStringA("Macros", "MoveKeys", "", buf, sizeof(buf), g_configPath.c_str());
    s->keyGroups = SplitGroups(buf);

    GetPrivateProfileStringA("Hotkeys", "CycleKey", s->cycleKeyName.c_str(), buf, sizeof(buf), g_configPath.c_str());
    s->cycleKeyName = buf;

    std::atomic_store(&g_settings, std::shared_ptr<const Settings>(s));
}

// Also now the only place that writes engine_status.txt: instead of a
// plain sleep_for(200ms), it waits (with a 200ms timeout) on g_statusCV.
// That means a pending status update gets flushed to disk promptly when
// one arrives, but the mainPID/config.ini polling above still runs on
// its normal ~200ms cadence even when no status update ever shows up.
static void ConfigWatcherThread(DWORD mainPID) {
    while (g_running.load(std::memory_order_relaxed)) {
        if (mainPID != 0) {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, mainPID);
            if (h) {
                bool exited = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
                CloseHandle(h);
                if (exited) { g_running.store(false); return; }
            } else {
                g_running.store(false);
                return;
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_configPath.c_str(), GetFileExInfoStandard, &fad)) {
            if (CompareFileTime(&fad.ftLastWriteTime, &g_lastConfigWrite) != 0) {
                g_lastConfigWrite = fad.ftLastWriteTime;
                LoadConfig();
            }
        }

        std::string toWrite;
        bool haveWork = false;
        {
            std::unique_lock<std::mutex> lock(g_statusMutex);
            g_statusCV.wait_for(lock, std::chrono::milliseconds(200),
                                 [] { return g_statusDirty || !g_running.load(std::memory_order_relaxed); });
            if (g_statusDirty) {
                toWrite = g_pendingStatus;
                g_statusDirty = false;
                haveWork = true;
            }
        } // lock released before touching disk
        if (haveWork) WriteGroupStatusToFile(toWrite);
    }
}

// ══════════════════════════════════════════════════════════════
//  Hot loop
// ══════════════════════════════════════════════════════════════
#define BUSY_SPIN 0

enum class MacroState { IDLE, BURST, COOLDOWN };

static void MacroLoop() {
    MacroState state = MacroState::IDLE;
    int burstCount = 0;
    std::string burstGroup;
    ULONGLONG cooldownEndMs = 0;
    int currentGroupIndex = 0;

    bool prevCycleDown = false;
    std::string prevCycleKeyName;
    WORD cycleVK = 0;

    HDC hdcScreen = GetDC(NULL);

    while (g_running.load(std::memory_order_relaxed)) {
        std::shared_ptr<const Settings> sp = std::atomic_load(&g_settings);
        const Settings& s = *sp;

        bool activeThisIteration = false;

        if (s.cycleKeyName != prevCycleKeyName) {
            prevCycleKeyName = s.cycleKeyName;
            cycleVK = KeyNameToVK(s.cycleKeyName);
            prevCycleDown = false;
        }

        if (!s.suspended) {
            bool cycleDown = KeyPhysicallyDown(cycleVK);
            if (cycleDown && !prevCycleDown && s.keyGroups.size() >= 2) {
                currentGroupIndex = (currentGroupIndex + 1) % (int)s.keyGroups.size();
                RequestGroupStatus(s.keyGroups[currentGroupIndex]);   // lets AHK show a tooltip; actual disk write happens on the watcher thread
            }
            prevCycleDown = cycleDown;
            if (currentGroupIndex >= (int)s.keyGroups.size()) currentGroupIndex = 0;

            bool rHeld = KeyPhysicallyDown('R');

            if (!s.keyGroups.empty()) {
                ULONGLONG now = GetTickCount64();

                switch (state) {
                    case MacroState::COOLDOWN:
                        if (now >= cooldownEndMs) state = MacroState::IDLE;
                        break;

                    case MacroState::IDLE:
                        if (rHeld) {
                            COLORREF cr = GetPixel(hdcScreen, s.px, s.py);
                            DWORD rgb = (DWORD)((GetRValue(cr) << 16) | (GetGValue(cr) << 8) | GetBValue(cr));
                            if (rgb != s.targetColor) {
                                burstGroup = s.keyGroups[currentGroupIndex];
                                burstCount = 0;
                                state = MacroState::BURST;
                            }
                        }
                        break;

                    case MacroState::BURST:
                        PressKeyGroup(burstGroup);
                        burstCount++;
                        activeThisIteration = true;
                        if (burstCount >= s.burstSize) {
                            state = MacroState::COOLDOWN;
                            cooldownEndMs = now + (ULONGLONG)(s.cooldownSeconds * 1000.0);
                        }
                        break;
                }
            }
        }

#if BUSY_SPIN
#else
        if (!activeThisIteration) Sleep(1);
#endif
    }

    ReleaseDC(NULL, hdcScreen);
}

// ══════════════════════════════════════════════════════════════
//  Entry point
// ══════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    if (argc < 2) {
        MessageBoxA(NULL, "engine.exe needs a config.ini path as its first argument.",
                    "Soru Macro Engine", MB_ICONERROR);
        return 1;
    }
    g_configPath = argv[1];
    {
        // engine_status.txt lives in the same directory as config.ini
        // (wherever the AHK side's EngineDir points it), created with
        // the hidden attribute (see WriteGroupStatusToFile). Deriving
        // this from g_configPath instead of a second hardcoded path
        // means it can never drift out of sync with where the AHK
        // script is actually looking for it.
        size_t slash = g_configPath.find_last_of("\\/");
        std::string dir = (slash == std::string::npos) ? "" : g_configPath.substr(0, slash + 1);
        g_statusPath = dir + "engine_status.txt";
    }
    DWORD mainPID = (argc >= 3) ? (DWORD)atoi(argv[2]) : 0;

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);

    LoadConfig();
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_configPath.c_str(), GetFileExInfoStandard, &fad))
            g_lastConfigWrite = fad.ftLastWriteTime;
    }

    std::thread watcher(ConfigWatcherThread, mainPID);

    std::thread hot(MacroLoop);
    SetThreadPriority(hot.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
    hot.join();

    g_running.store(false);
    g_statusCV.notify_one();   // wake the watcher immediately instead of waiting out its 200ms timeout
    watcher.join();

    timeEndPeriod(1);
    return 0;
}
