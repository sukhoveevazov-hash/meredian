// ============================================================================
// MEREDIAN Role Play — Input & UI Interceptor v2 (meredian_esc.asi)
// Target: GTA:SA US 1.0 + SA-MP 0.3.7-R1 / 0.3.7-R3-1 + samp-cef v1.4.1
//
// Функции:
//  1) Перехватывает ESC -> блокирует баганое меню паузы GTA и шлёт VK_F11
//     в CEF (открывает/закрывает CEF-меню паузы). Shift+ESC открывает родное меню.
//  2) Перехватывает T, F6 и '/' (когда не открыт чат/диалог SA-MP и не зажат
//     Shift/Ctrl/Alt) -> блокирует старую строку ввода SA-MP и шлёт VK_F12
//     (или VK_F10 для '/') в CEF-чат!
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace {

constexpr uintptr_t ADDR_GTA_HWND            = 0x00C97C1C;
constexpr uintptr_t ADDR_GTA_MENU_ACTIVE     = 0x00BA67A4;

constexpr uint32_t SAMP_EP_R1                = 0x0031DF13;
constexpr uint32_t SAMP_EP_R3_1              = 0x000CC4D0;

constexpr uintptr_t SAMP_R1_CINPUT           = 0x0021A0E8;
constexpr uintptr_t SAMP_R1_CDIALOG          = 0x0021A0B8;

constexpr uintptr_t SAMP_R3_CINPUT           = 0x0026E8CC;
constexpr uintptr_t SAMP_R3_CDIALOG          = 0x0026E898;

constexpr uintptr_t OFFSET_CINPUT_ENABLED    = 0x14E0;
constexpr uintptr_t OFFSET_CDIALOG_ACTIVE    = 0x0028;

constexpr WPARAM VK_CEF_PAUSE                = VK_F11; // ESC -> CEF Pause
constexpr WPARAM VK_CEF_CHAT                 = VK_F12; // T / F6 -> CEF Chat
constexpr WPARAM VK_CEF_CHAT_CMD             = VK_F10; // '/' -> CEF Chat с "/"

WNDPROC g_OrigWndProc      = nullptr;
HWND    g_GameHwnd         = nullptr;
bool    g_EscSwallowed     = false;
WPARAM  g_ChatSwallowedKey = 0;
bool    g_Running          = true;

uintptr_t g_SampBase       = 0;
uintptr_t g_OffInput       = 0;
uintptr_t g_OffDialog      = 0;

bool IsMemoryReadable(uintptr_t addr, size_t size) {
    if (addr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size) <= regionEnd;
}

template <typename T>
bool SafeRead(uintptr_t addr, T& out) {
    if (!IsMemoryReadable(addr, sizeof(T))) return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

void DetectSampVersion() {
    if (g_SampBase != 0) return;
    HMODULE hSamp = GetModuleHandleA("samp.dll");
    if (!hSamp) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(hSamp);
    IMAGE_DOS_HEADER dos{};
    if (!SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS32 nt{};
    if (!SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE) return;

    const uint32_t ep = nt.OptionalHeader.AddressOfEntryPoint;
    if (ep == SAMP_EP_R3_1) {
        g_SampBase  = base;
        g_OffInput  = SAMP_R3_CINPUT;
        g_OffDialog = SAMP_R3_CDIALOG;
    } else if (ep == SAMP_EP_R1) {
        g_SampBase  = base;
        g_OffInput  = SAMP_R1_CINPUT;
        g_OffDialog = SAMP_R1_CDIALOG;
    }
}

bool IsNativeGtaMenuOpen() {
    uint8_t active = 0;
    return SafeRead(ADDR_GTA_MENU_ACTIVE, active) && (active != 0);
}

bool IsSampUiOpen() {
    DetectSampVersion();
    if (g_SampBase == 0) return false;

    uintptr_t pInput = 0;
    if (SafeRead(g_SampBase + g_OffInput, pInput) && pInput != 0) {
        int32_t enabled = 0;
        if (SafeRead(pInput + OFFSET_CINPUT_ENABLED, enabled) && enabled != 0) return true;
    }

    uintptr_t pDialog = 0;
    if (SafeRead(g_SampBase + g_OffDialog, pDialog) && pDialog != 0) {
        int32_t active = 0;
        if (SafeRead(pDialog + OFFSET_CDIALOG_ACTIVE, active) && active != 0) return true;
    }
    return false;
}

void PostCefVirtualKey(HWND hwnd, WPARAM vk, bool keyDown) {
    const UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    const LPARAM lp = keyDown
        ? static_cast<LPARAM>((scan << 16) | 1u)
        : static_cast<LPARAM>((scan << 16) | 0xC0000001u);
    PostMessageA(hwnd, keyDown ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

LRESULT CALLBACK MeredianWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrlHeld  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altHeld   = (GetKeyState(VK_MENU) & 0x8000) != 0;

    // 1. Перехват ESC -> CEF Pause
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (!shiftHeld && !IsNativeGtaMenuOpen() && !IsSampUiOpen()) {
            const bool isRepeat = (lParam & 0x40000000) != 0;
            g_EscSwallowed = true;
            if (!isRepeat) PostCefVirtualKey(hwnd, VK_CEF_PAUSE, true);
            return 0;
        }
        g_EscSwallowed = false;
    } else if (msg == WM_KEYUP && wParam == VK_ESCAPE) {
        if (g_EscSwallowed) {
            g_EscSwallowed = false;
            PostCefVirtualKey(hwnd, VK_CEF_PAUSE, false);
            return 0;
        }
    } else if (msg == WM_CHAR && wParam == 0x1B && g_EscSwallowed) {
        return 0;
    }

    // 2. Перехват T / F6 / '/' -> CEF Chat (когда не в фокусе ввода и не открыты меню)
    if (msg == WM_KEYDOWN && !shiftHeld && !ctrlHeld && !altHeld && !IsNativeGtaMenuOpen() && !IsSampUiOpen()) {
        if (wParam == 'T' || wParam == VK_F6) {
            const bool isRepeat = (lParam & 0x40000000) != 0;
            g_ChatSwallowedKey = wParam;
            if (!isRepeat) PostCefVirtualKey(hwnd, VK_CEF_CHAT, true);
            return 0;
        } else if (wParam == VK_OEM_2) {
            const bool isRepeat = (lParam & 0x40000000) != 0;
            g_ChatSwallowedKey = wParam;
            if (!isRepeat) PostCefVirtualKey(hwnd, VK_CEF_CHAT_CMD, true);
            return 0;
        }
    } else if (msg == WM_KEYUP && g_ChatSwallowedKey != 0 && wParam == g_ChatSwallowedKey) {
        const WPARAM trigger = (wParam == VK_OEM_2) ? VK_CEF_CHAT_CMD : VK_CEF_CHAT;
        g_ChatSwallowedKey = 0;
        PostCefVirtualKey(hwnd, trigger, false);
        return 0;
    } else if (msg == WM_CHAR && g_ChatSwallowedKey != 0) {
        return 0;
    }

    return CallWindowProcA(g_OrigWndProc, hwnd, msg, wParam, lParam);
}

DWORD WINAPI InitThread(LPVOID) {
    HWND hwnd = nullptr;
    while (g_Running) {
        if (SafeRead(ADDR_GTA_HWND, hwnd) && hwnd != nullptr && IsWindow(hwnd)) break;
        Sleep(100);
    }
    if (!g_Running || !hwnd) return 0;

    Sleep(2500);
    if (!g_Running || !IsWindow(hwnd)) return 0;

    DetectSampVersion();
    g_GameHwnd = hwnd;
    g_OrigWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MeredianWndProc))
    );
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE hThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        g_Running = false;
        if (g_GameHwnd && g_OrigWndProc && IsWindow(g_GameHwnd)) {
            SetWindowLongPtrA(g_GameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OrigWndProc));
        }
    }
    return TRUE;
}
