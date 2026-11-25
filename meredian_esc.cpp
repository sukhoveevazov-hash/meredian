// ============================================================================
// MEREDIAN Role Play — ESC Pause Interceptor (meredian_esc.asi)
// Target: GTA:SA US 1.0 + SA-MP 0.3.7-R1 / 0.3.7-R3-1 + samp-cef v1.4.1
//
// Как работает:
//  - Перехватывает нажатие ESC в WndProc окна игры (gta_sa.exe).
//  - Пропускает ESC как обычно, если:
//      1) Зажат Shift (Shift + ESC -> вход в системное меню GTA:SA)
//      2) Системное меню GTA:SA уже открыто (0xBA67A4 != 0)
//      3) Открыт чат SA-MP (CInput::m_bEnabled) или диалог (CDialog::m_bIsActive)
//  - В обычной игре блокирует стандартное баганое меню паузы GTA и отправляет
//    клавишу VK_F11 в очередь сообщений окна -> samp-cef открывает/закрывает
//    наше CEF-меню паузы без конфликтов с движком.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace {

constexpr uintptr_t ADDR_GTA_HWND            = 0x00C97C1C; // HWND главного окна GTA:SA 1.0 US
constexpr uintptr_t ADDR_GTA_MENU_ACTIVE     = 0x00BA67A4; // FrontEndMenuManager + 0x5C (m_bMenuActive)

// SA-MP 0.3.7-R1 & 0.3.7-R3-1 offsets
constexpr uint32_t SAMP_EP_R1                = 0x0031DF13;
constexpr uint32_t SAMP_EP_R3_1              = 0x000CC4D0;

constexpr uintptr_t SAMP_R1_CINPUT           = 0x0021A0E8;
constexpr uintptr_t SAMP_R1_CDIALOG          = 0x0021A0B8;

constexpr uintptr_t SAMP_R3_CINPUT           = 0x0026E8CC;
constexpr uintptr_t SAMP_R3_CDIALOG          = 0x0026E898;

constexpr uintptr_t OFFSET_CINPUT_ENABLED    = 0x14E0;     // BOOL m_bEnabled
constexpr uintptr_t OFFSET_CDIALOG_ACTIVE    = 0x0028;     // BOOL m_bIsActive

constexpr WPARAM CEF_TRIGGER_VK              = VK_F11;     // Не используется GTA:SA и SA-MP

WNDPROC g_OrigWndProc      = nullptr;
HWND    g_GameHwnd         = nullptr;
bool    g_EscSwallowed     = false;
bool    g_Running          = true;

uintptr_t g_SampBase       = 0;
uintptr_t g_OffInput       = 0;
uintptr_t g_OffDialog      = 0;

bool IsMemoryReadable(uintptr_t addr, size_t size) {
    if (addr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
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
        if (SafeRead(pInput + OFFSET_CINPUT_ENABLED, enabled) && enabled != 0) {
            return true;
        }
    }

    uintptr_t pDialog = 0;
    if (SafeRead(g_SampBase + g_OffDialog, pDialog) && pDialog != 0) {
        int32_t active = 0;
        if (SafeRead(pDialog + OFFSET_CDIALOG_ACTIVE, active) && active != 0) {
            return true;
        }
    }
    return false;
}

void PostCefMenuKey(HWND hwnd, bool keyDown) {
    const UINT scan = MapVirtualKeyA(static_cast<UINT>(CEF_TRIGGER_VK), MAPVK_VK_TO_VSC);
    const LPARAM lp = keyDown
        ? static_cast<LPARAM>((scan << 16) | 1u)
        : static_cast<LPARAM>((scan << 16) | 0xC0000001u);
    PostMessageA(hwnd, keyDown ? WM_KEYDOWN : WM_KEYUP, CEF_TRIGGER_VK, lp);
}

LRESULT CALLBACK MeredianWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!shiftHeld && !IsNativeGtaMenuOpen() && !IsSampUiOpen()) {
            const bool isRepeat = (lParam & 0x40000000) != 0;
            g_EscSwallowed = true;
            if (!isRepeat) {
                PostCefMenuKey(hwnd, true);
            }
            return 0; // Полностью блокируем стандартный ESC для gta_sa.exe
        }
        g_EscSwallowed = false;
    } else if (msg == WM_KEYUP && wParam == VK_ESCAPE) {
        if (g_EscSwallowed) {
            g_EscSwallowed = false;
            PostCefMenuKey(hwnd, false);
            return 0;
        }
    } else if (msg == WM_CHAR && wParam == 0x1B) {
        if (g_EscSwallowed) {
            return 0;
        }
    }

    return CallWindowProcA(g_OrigWndProc, hwnd, msg, wParam, lParam);
}

DWORD WINAPI InitThread(LPVOID) {
    // Ждём создания главного окна GTA:SA
    HWND hwnd = nullptr;
    while (g_Running) {
        if (SafeRead(ADDR_GTA_HWND, hwnd) && hwnd != nullptr && IsWindow(hwnd)) {
            break;
        }
        Sleep(100);
    }
    if (!g_Running || !hwnd) return 0;

    // Даём samp.dll и samp-cef (cef.asi) завершить инициализацию и поставить свои хуки
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
