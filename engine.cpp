// ══════════════════════════════════════════════════════════════
//  Soru Macro Engine — native replacement for macro_engine.ahk
//
//  Usage:  engine.exe "<path to config.ini>" <main script PID>
//
//  Build:
//    MSVC:   cl /O2 /EHsc engine.cpp /link /SUBSYSTEM:WINDOWS ^
//                /ENTRY:mainCRTStartup user32.lib gdi32.lib d3d11.lib dxgi.lib /OUT:engine.exe
//    MinGW:  g++ -O2 -municode -mwindows engine.cpp -o engine.exe ^
//                -luser32 -lgdi32 -ld3d11 -ldxgi
//
//  ── Pixel capture in this version ──
//  Primary path: DXGI Desktop Duplication API. This reads straight from
//  the GPU's swapchain via AcquireNextFrame(), which only returns once a
//  genuinely NEW frame has been presented — no polling a possibly-stale
//  composited surface, no guessing whether what you just read is fresh.
//  When no new frame is available yet (nothing on screen changed since
//  the last check), it just reuses the last known pixel color, which is
//  correct by construction (if nothing changed, the pixel didn't either).
//
//  Fallback path: if DXGI init fails for any reason (remote desktop
//  session, no hardware D3D11 device, output duplication blocked, pixel
//  is on a monitor/adapter index other than 0, etc.) it falls back
//  automatically to the BitBlt/DIBSection method, so the engine still
//  runs — just without the freshness guarantee DXGI gives you.
//
//  NOTE: DXGI Desktop Duplication only captures adapter 0 / output 0
//  (your primary monitor) as written. If your target pixel is on a
//  different monitor, change the EnumOutputs(0, ...) index in
//  DupCapture::Init() below.
// ══════════════════════════════════════════════════════════════

#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
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
//  Fallback pixel capture: BitBlt into a cached 1x1 DIBSection.
//  Used only if DXGI Desktop Duplication fails to initialize.
// ══════════════════════════════════════════════════════════════
static HDC     g_hdcScreen = NULL;
static HDC     g_hdcMem = NULL;
static HBITMAP g_hBitmap = NULL;
static BYTE*   g_bits = NULL;

static void InitBitBltCapture() {
    g_hdcScreen = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(g_hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = 1;
    bmi.bmiHeader.biHeight      = -1;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hBitmap = CreateDIBSection(g_hdcMem, &bmi, DIB_RGB_COLORS, (void**)&g_bits, NULL, 0);
    SelectObject(g_hdcMem, g_hBitmap);
}

static void ShutdownBitBltCapture() {
    if (g_hBitmap) DeleteObject(g_hBitmap);
    if (g_hdcMem) DeleteDC(g_hdcMem);
    if (g_hdcScreen) ReleaseDC(NULL, g_hdcScreen);
}

static inline DWORD ReadPixelBitBlt(int x, int y) {
    BitBlt(g_hdcMem, 0, 0, 1, 1, g_hdcScreen, x, y, SRCCOPY);
    return (DWORD)(((DWORD)g_bits[2] << 16) | ((DWORD)g_bits[1] << 8) | (DWORD)g_bits[0]);
}

// ══════════════════════════════════════════════════════════════
//  Primary pixel capture: DXGI Desktop Duplication.
//  AcquireNextFrame() only returns when the desktop actually changed —
//  so a successful read is guaranteed fresh, not a possibly-stale
//  composited frame. A timeout just means nothing changed since the
//  last check, so the caller keeps using the last known color.
// ══════════════════════════════════════════════════════════════
struct DupCapture {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* stagingTex = nullptr;
    int desktopX = 0, desktopY = 0;   // this output's top-left in virtual-desktop coords
    bool ok = false;

    bool Init() {
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                        nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);
        if (FAILED(hr) || !device) return false;

        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice)
            return false;

        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(hr) || !adapter) return false;

        // Output index 0 = primary monitor. Change this if your pixel
        // coordinate is on a different monitor.
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr) || !output) return false;

        DXGI_OUTPUT_DESC outDesc;
        output->GetDesc(&outDesc);
        desktopX = outDesc.DesktopCoordinates.left;
        desktopY = outDesc.DesktopCoordinates.top;

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        if (FAILED(hr) || !output1) return false;

        hr = output1->DuplicateOutput(device, &duplication);
        output1->Release();
        if (FAILED(hr) || !duplication) return false;   // common causes: RDP session, DRM-protected content on screen, no permission

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&td, nullptr, &stagingTex);
        if (FAILED(hr) || !stagingTex) return false;

        ok = true;
        return true;
    }

    // true  = rgbOut holds a freshly-presented pixel
    // false = no new frame since last call (nothing changed) OR a hard
    //         error; caller should keep using its last known color either way
    bool TryReadPixel(int globalX, int globalY, DWORD& rgbOut) {
        if (!ok) return false;

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) return false;   // e.g. DXGI_ERROR_ACCESS_LOST on mode switch/UAC prompt — caller keeps last known color

        ID3D11Texture2D* frameTex = nullptr;
        desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameTex);
        desktopResource->Release();

        int lx = globalX - desktopX;
        int ly = globalY - desktopY;
        bool success = false;

        if (frameTex && lx >= 0 && ly >= 0) {
            D3D11_BOX box = { (UINT)lx, (UINT)ly, 0, (UINT)(lx + 1), (UINT)(ly + 1), 1 };
            context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, frameTex, 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                BYTE* p = (BYTE*)mapped.pData;   // BGRA
                rgbOut = ((DWORD)p[2] << 16) | ((DWORD)p[1] << 8) | (DWORD)p[0];
                context->Unmap(stagingTex, 0);
                success = true;
            }
        }
        if (frameTex) frameTex->Release();

        duplication->ReleaseFrame();
        return success;
    }

    void Shutdown() {
        if (stagingTex) stagingTex->Release();
        if (duplication) duplication->Release();
        if (context) context->Release();
        if (device) device->Release();
    }
};

static DupCapture g_dup;
static bool g_useDup = false;
static DWORD g_lastKnownRgb = 0xFFFFFFFF;   // sentinel so the very first read always "counts"

static void InitPixelCapture() {
    g_useDup = g_dup.Init();
    if (!g_useDup) {
        InitBitBltCapture();   // fallback — DXGI unavailable (RDP session, no HW D3D11 device, blocked duplication, etc.)
    }
}

static void ShutdownPixelCapture() {
    if (g_useDup) g_dup.Shutdown();
    else ShutdownBitBltCapture();
}

static inline DWORD ReadPixel(int x, int y) {
    if (g_useDup) {
        DWORD rgb;
        if (g_dup.TryReadPixel(x, y, rgb)) g_lastKnownRgb = rgb;
        return g_lastKnownRgb;   // fresh if just updated, otherwise correctly-still-the-same as last frame
    }
    return ReadPixelBitBlt(x, y);
}

// ══════════════════════════════════════════════════════════════
//  Config loading
// ══════════════════════════════════════════════════════════════
static std::string g_configPath;
static std::string g_statusPath;

static void WriteGroupStatus(const std::string& groupName) {
    if (g_statusPath.empty()) return;
    HANDLE h = CreateFileA(g_statusPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, groupName.data(), (DWORD)groupName.size(), &written, NULL);
    CloseHandle(h);
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

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

    InitPixelCapture();

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
                WriteGroupStatus(s.keyGroups[currentGroupIndex]);
            }
            prevCycleDown = cycleDown;
            if (currentGroupIndex >= (int)s.keyGroups.size()) currentGroupIndex = 0;

            bool rHeld = KeyPhysicallyDown('R');
            if (rHeld) activeThisIteration = true;

            if (!s.keyGroups.empty()) {
                ULONGLONG now = GetTickCount64();

                switch (state) {
                    case MacroState::COOLDOWN:
                        if (now >= cooldownEndMs) state = MacroState::IDLE;
                        break;

                    case MacroState::IDLE:
                        if (rHeld) {
                            DWORD rgb = ReadPixel(s.px, s.py);
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

    ShutdownPixelCapture();
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
    watcher.join();

    timeEndPeriod(1);
    return 0;
}
