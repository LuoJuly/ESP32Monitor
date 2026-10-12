/**
 * Windows side of the ESP32 Monitor Bluetooth link.
 * Finds the peripheral named ESP32Monitor, writes the same text lines the
 * serial port uses, and waits until the round screen is tapped.
 */

#define WIN32_LEAN_AND_MEAN
#include "BleClient.h"
#include <objbase.h>
#include <setupapi.h>
#include <bluetoothleapis.h>
#pragma comment(lib, "BluetoothApis.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

namespace {

constexpr uint32_t kChunk = 20;
const guid kService{0x6E400001, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};
const guid kRx{0x6E400002, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};
const guid kTx{0x6E400003, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};

std::mutex g_mu;
bool g_open = false;
volatile LONG g_cancel = 0;
std::string g_rx;
std::string g_token;
BluetoothLEDevice g_device{nullptr};
GattSession g_session{nullptr};
GattDeviceService g_service{nullptr};
GattCharacteristic g_rx_chr{nullptr};
GattCharacteristic g_tx_chr{nullptr};
event_token g_notify_token{};
HANDLE g_w32 = INVALID_HANDLE_VALUE;
BLUETOOTH_GATT_EVENT_HANDLE g_w32_event = nullptr;
BTH_LE_GATT_CHARACTERISTIC g_w32_rx{};
bool g_w32_have_rx = false;

void DllDir(wchar_t* out, DWORD cap)
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &mod);
    GetModuleFileNameW(mod, out, cap);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
}

void LoadToken()
{
    g_token.clear();
    wchar_t path[MAX_PATH];
    DllDir(path, MAX_PATH);
    wcscat_s(path, L"ble.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt") != 0 || !f) return;
    char buf[32] = {};
    if (fgets(buf, sizeof(buf), f)) {
        size_t n = strcspn(buf, "\r\n");
        if (n == 8) g_token.assign(buf, n);
    }
    fclose(f);
}

void SaveToken(const std::string& token)
{
    g_token = token;
    wchar_t path[MAX_PATH];
    DllDir(path, MAX_PATH);
    wcscat_s(path, L"ble.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt") != 0 || !f) return;
    fputs(token.c_str(), f);
    fclose(f);
}

void NoteRx(const uint8_t* data, uint32_t n)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_rx.append(reinterpret_cast<const char*>(data), n);
    if (g_rx.size() > 8192) g_rx.erase(0, g_rx.size() - 4096);
    const char key[] = "BLE_OK:";
    auto at = g_rx.find(key);
    if (at != std::string::npos && at + 15 <= g_rx.size()) {
        std::string token = g_rx.substr(at + 7, 8);
        bool hex = token.size() == 8;
        for (char c : token) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
        }
        if (hex) SaveToken(token);
    }
}

void BleLog(const wchar_t* text)
{
    wchar_t path[MAX_PATH];
    DllDir(path, MAX_PATH);
    wcscat_s(path, L"ble_log.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"at") == 0 && f) {
        fputws(text, f);
        fputws(L"\n", f);
        fclose(f);
    }
    if (_wfopen_s(&f, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && f) {
        fputws(text, f);
        fputws(L"\n", f);
        fclose(f);
    }
}

void EnsureApartment()
{
    static bool ready = false;
    if (ready) return;
    FILE* enter = nullptr;
    if (_wfopen_s(&enter, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && enter) {
        fputws(L"apt-enter\n", enter);
        fclose(enter);
    }
    /* CoInitializeEx lets the calls return, but service discovery stays
     * Unreachable until the Windows Runtime apartment is initialized. */
    HRESULT hr = S_OK;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        hr = e.code();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && f) {
        wchar_t note[32];
        swprintf_s(note, L"apt %08X\n", (unsigned)hr);
        fputws(note, f);
        fclose(f);
    }
    ready = true;
}

void CloseSession()
{
    /* Dropping this session is what makes Windows fall back to 已配对.
     * Keep it alive for the whole link. */
    GattSession session{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        session = g_session;
        g_session = nullptr;
    }
    if (!session) return;
    try { session.MaintainConnection(false); } catch (...) {}
    try { session.Close(); } catch (...) {}
}

void HoldConnection()
{
    CloseSession();
    if (!g_device) return;
    try {
        GattSession session = GattSession::FromDeviceIdAsync(g_device.BluetoothDeviceId()).get();
        session.MaintainConnection(true);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_session = session;
        }
        BleLog(L"hold");
    } catch (hresult_error const& e) {
        wchar_t note[48];
        swprintf_s(note, L"session %08X", (unsigned)e.code());
        BleLog(note);
    }
}

bool UuidIs(const BTH_LE_UUID& u, const guid& g)
{
    if (u.IsShortUuid) return false;
    const GUID& a = u.Value.LongUuid;
    return a.Data1 == g.Data1 && a.Data2 == g.Data2 && a.Data3 == g.Data3 &&
           memcmp(a.Data4, g.Data4, 8) == 0;
}

void LogHr(const wchar_t* tag, unsigned hr)
{
    wchar_t note[48];
    swprintf_s(note, L"%s %08X", tag, hr);
    BleLog(note);
}

HANDLE OpenMonitorHandle(bool service_node)
{
    const GUID classes[] = {
        {0x781aee18, 0x7733, 0x4ce4, {0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92}},
        {0x6e3bb679, 0x4372, 0x40c8, {0x9e, 0xaa, 0x45, 0x09, 0xdf, 0x26, 0x0c, 0xd8}},
    };
    for (const GUID& cls : classes) {
        HDEVINFO info = SetupDiGetClassDevsW(&cls, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA data{};
        data.cbSize = sizeof(data);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &cls, i, &data); ++i) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &data, nullptr, 0, &need, nullptr);
            if (!need) continue;
            auto* buf = static_cast<BYTE*>(malloc(need));
            if (!buf) continue;
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf);
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            HANDLE opened = INVALID_HANDLE_VALUE;
            if (SetupDiGetDeviceInterfaceDetailW(info, &data, detail, need, nullptr, nullptr)) {
                std::wstring path = detail->DevicePath;
                for (auto& c : path) c = towupper(c);
                bool uart = path.find(L"6E400001") != std::wstring::npos;
                if (path.find(L"7C0C5F423396") != std::wstring::npos && uart == service_node) {
                    opened = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (opened == INVALID_HANDLE_VALUE) {
                        wchar_t note[40];
                        swprintf_s(note, L"w32 open %lu", GetLastError());
                        BleLog(note);
                    }
                }
            }
            free(buf);
            if (opened != INVALID_HANDLE_VALUE) {
                SetupDiDestroyDeviceInfoList(info);
                return opened;
            }
        }
        SetupDiDestroyDeviceInfoList(info);
    }
    return INVALID_HANDLE_VALUE;
}

VOID CALLBACK OnW32Notify(BTH_LE_GATT_EVENT_TYPE type, PVOID param, PVOID)
{
    if (type != CharacteristicValueChangedEvent || !param) return;
    auto* ev = static_cast<PBLUETOOTH_GATT_VALUE_CHANGED_EVENT>(param);
    if (!ev->CharacteristicValue || !ev->CharacteristicValue->DataSize) return;
    NoteRx(ev->CharacteristicValue->Data, (uint32_t)ev->CharacteristicValue->DataSize);
}

bool Win32Subscribe()
{
    if (g_w32 != INVALID_HANDLE_VALUE) return true;
    HANDLE h = OpenMonitorHandle(false);
    HANDLE svc = OpenMonitorHandle(true);
    if (h == INVALID_HANDLE_VALUE) h = svc;
    if (h == INVALID_HANDLE_VALUE) {
        BleLog(L"w32 no dev");
        return false;
    }
    if (svc == INVALID_HANDLE_VALUE) BleLog(L"w32 no svc");
    USHORT n = 0;
    HRESULT hr = BluetoothGATTGetServices(h, 0, nullptr, &n, BLUETOOTH_GATT_FLAG_NONE);
    LogHr(L"w32 svc", (unsigned)hr);
    if (!n) {
        hr = BluetoothGATTGetServices(h, 0, nullptr, &n, BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);
        LogHr(L"w32 live", (unsigned)hr);
    }
    wchar_t count[24];
    swprintf_s(count, L"w32 n %u", n);
    BleLog(count);
    if (!n) {
        CloseHandle(h);
        return false;
    }
    auto* svcs = static_cast<PBTH_LE_GATT_SERVICE>(calloc(n, sizeof(BTH_LE_GATT_SERVICE)));
    USHORT got = 0;
    hr = BluetoothGATTGetServices(h, n, svcs, &got, BLUETOOTH_GATT_FLAG_NONE);
    LogHr(L"w32 svc2", (unsigned)hr);
    bool ok = false;
    for (USHORT i = 0; i < got && !ok; ++i) {
        if (!UuidIs(svcs[i].ServiceUuid, kService)) continue;
        USHORT cn = 0;
        BluetoothGATTGetCharacteristics(h, &svcs[i], 0, nullptr, &cn, BLUETOOTH_GATT_FLAG_NONE);
        if (!cn) break;
        auto* chrs = static_cast<PBTH_LE_GATT_CHARACTERISTIC>(calloc(cn, sizeof(BTH_LE_GATT_CHARACTERISTIC)));
        USHORT cgot = 0;
        hr = BluetoothGATTGetCharacteristics(h, &svcs[i], cn, chrs, &cgot, BLUETOOTH_GATT_FLAG_NONE);
        LogHr(L"w32 chr", (unsigned)hr);
        for (USHORT c = 0; c < cgot && !ok; ++c) {
            if (UuidIs(chrs[c].CharacteristicUuid, kRx)) {
                g_w32_rx = chrs[c];
                g_w32_have_rx = true;
            }
            if (!UuidIs(chrs[c].CharacteristicUuid, kTx)) continue;
            USHORT dn = 0;
            BluetoothGATTGetDescriptors(h, &chrs[c], 0, nullptr, &dn, BLUETOOTH_GATT_FLAG_NONE);
            if (!dn) break;
            auto* descs = static_cast<PBTH_LE_GATT_DESCRIPTOR>(calloc(dn, sizeof(BTH_LE_GATT_DESCRIPTOR)));
            USHORT dgot = 0;
            BluetoothGATTGetDescriptors(h, &chrs[c], dn, descs, &dgot, BLUETOOTH_GATT_FLAG_NONE);
            for (USHORT d = 0; d < dgot; ++d) {
                bool cccd = descs[d].DescriptorType == ClientCharacteristicConfiguration ||
                            (descs[d].DescriptorUuid.IsShortUuid &&
                             descs[d].DescriptorUuid.Value.ShortUuid == 0x2902);
                if (!cccd) continue;
                wchar_t note[40];
                swprintf_s(note, L"w32 desc %u %04X", (unsigned)descs[d].DescriptorType,
                           descs[d].DescriptorUuid.IsShortUuid ? descs[d].DescriptorUuid.Value.ShortUuid : 0);
                BleLog(note);
                BTH_LE_GATT_DESCRIPTOR_VALUE val{};
                val.DescriptorType = ClientCharacteristicConfiguration;
                val.ClientCharacteristicConfiguration.IsSubscribeToNotification = TRUE;
                HANDLE writer = svc != INVALID_HANDLE_VALUE ? svc : h;
                hr = BluetoothGATTSetDescriptorValue(writer, &descs[d], &val, BLUETOOTH_GATT_FLAG_NONE);
                LogHr(L"w32 cccd", (unsigned)hr);
                if (FAILED(hr)) continue;
                BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION reg{};
                reg.NumCharacteristics = 1;
                reg.Characteristics[0] = chrs[c];
                hr = BluetoothGATTRegisterEvent(writer, CharacteristicValueChangedEvent, &reg,
                                                 OnW32Notify, nullptr, &g_w32_event,
                                                 BLUETOOTH_GATT_FLAG_NONE);
                LogHr(L"w32 reg", (unsigned)hr);
                g_w32 = writer;
                if (writer == h) h = INVALID_HANDLE_VALUE;
                if (writer == svc) svc = INVALID_HANDLE_VALUE;
                ok = true;
                break;
            }
            free(descs);
        }
        free(chrs);
    }
    free(svcs);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (svc != INVALID_HANDLE_VALUE && svc != h) CloseHandle(svc);
    return ok;
}

bool Win32Write(const char* data, DWORD n)
{
    if (!data || !n || g_w32 == INVALID_HANDLE_VALUE || !g_w32_have_rx) return false;
    auto* val = static_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(
        malloc(sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE) + n));
    if (!val) return false;
    val->DataSize = n;
    memcpy(val->Data, data, n);
    HRESULT hr = BluetoothGATTSetCharacteristicValue(
        g_w32, &g_w32_rx, val, 0, BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE);
    free(val);
    LogHr(L"w32 wr", (unsigned)hr);
    return SUCCEEDED(hr);
}

void ReleaseDevice()
{
    if (g_w32_event) {
        BluetoothGATTUnregisterEvent(g_w32_event, BLUETOOTH_GATT_FLAG_NONE);
        g_w32_event = nullptr;
    }
    if (g_w32 != INVALID_HANDLE_VALUE) {
        CloseHandle(g_w32);
        g_w32 = INVALID_HANDLE_VALUE;
    }
    g_w32_have_rx = false;
    if (g_tx_chr && g_notify_token.value) {
        try { g_tx_chr.ValueChanged(g_notify_token); } catch (...) {}
        g_notify_token = {};
    }
    g_rx_chr = nullptr;
    g_tx_chr = nullptr;
    if (g_service) {
        try { g_service.Close(); } catch (...) {}
        g_service = nullptr;
    }
    CloseSession();
    if (g_device) {
        try { g_device.Close(); } catch (...) {}
        g_device = nullptr;
    }
    g_open = false;
}

struct PinState {
    wchar_t text[8];
    bool done;
    bool ok;
};

LRESULT CALLBACK PinWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<PinState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    if (msg == WM_CREATE && st) {
        CreateWindowExW(0, L"STATIC", L"输入屏幕上的 6 位数字",
                        WS_CHILD | WS_VISIBLE, 20, 14, 210, 22,
                        hwnd, nullptr, nullptr, nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
                                    20, 42, 210, 28, hwnd, reinterpret_cast<HMENU>(100),
                                    nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定",
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        75, 82, 100, 28, hwnd, reinterpret_cast<HMENU>(IDOK),
                        nullptr, nullptr);
        if (edit) SetFocus(edit);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK && st) {
        wchar_t buf[16] = {};
        GetWindowTextW(GetDlgItem(hwnd, 100), buf, 16);
        st->ok = wcslen(buf) == 6;
        if (st->ok) wcsncpy_s(st->text, buf, _TRUNCATE);
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    if ((msg == WM_CLOSE || msg == WM_DESTROY) && st) {
        st->done = true;
        if (msg == WM_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool AskPin(wchar_t* out, int cap)
{
    static bool ready = false;
    const wchar_t* cls = L"Esp32MonitorPin";
    if (!ready) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PinWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = cls;
        ready = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    PinState st{};
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, cls, L"ESP32Monitor",
                                WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 260, 168,
                                nullptr, nullptr, GetModuleHandleW(nullptr), &st);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    while (!st.done && IsWindow(hwnd)) {
        MSG msg;
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            Sleep(15);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!st.ok) return false;
    wcsncpy_s(out, cap, st.text, _TRUNCATE);
    return true;
}

/* The stored Windows bond is weaker than the passkey this firmware requires.
 * Drop it once and ask for the 6 digits on the round screen. */
bool UpgradeBond()
{
    try {
        auto pairing = g_device.DeviceInformation().Pairing();
        if (pairing.IsPaired()) {
            auto cleared = pairing.UnpairAsync().get();
            wchar_t note[32];
            swprintf_s(note, L"unpair %d", (int)cleared.Status());
            BleLog(note);
        }
        auto custom = g_device.DeviceInformation().Pairing().Custom();
        auto token = custom.PairingRequested([](auto const&, DevicePairingRequestedEventArgs const& args) {
            auto kind = args.PairingKind();
            wchar_t note[32];
            swprintf_s(note, L"pin kind %d", (int)kind);
            BleLog(note);
            if (kind == DevicePairingKinds::ProvidePin) {
                wchar_t pin[8] = {};
                if (AskPin(pin, 8)) args.Accept(pin);
            } else if (kind == DevicePairingKinds::ConfirmOnly ||
                       kind == DevicePairingKinds::ConfirmPinMatch) {
                int answer = MessageBoxW(nullptr, L"请确认屏幕上的数字", L"ESP32Monitor",
                                         MB_YESNO | MB_TOPMOST | MB_SETFOREGROUND);
                if (answer == IDYES) args.Accept();
            }
        });
        auto result = custom.PairAsync(
            DevicePairingKinds::ProvidePin | DevicePairingKinds::ConfirmPinMatch |
                DevicePairingKinds::ConfirmOnly,
            DevicePairingProtectionLevel::EncryptionAndAuthentication).get();
        custom.PairingRequested(token);
        wchar_t note[32];
        swprintf_s(note, L"repair %d", (int)result.Status());
        BleLog(note);
        auto st = result.Status();
        return st == DevicePairingResultStatus::Paired ||
               st == DevicePairingResultStatus::AlreadyPaired;
    } catch (hresult_error const& e) {
        wchar_t note[48];
        swprintf_s(note, L"repair hr %08X", (unsigned)e.code());
        BleLog(note);
        return false;
    }
}

bool WriteChunked(GattCharacteristic const& chr, const char* data, DWORD n)
{
    try {
        for (DWORD off = 0; off < n; off += kChunk) {
            DWORD nthis = n - off;
            if (nthis > kChunk) nthis = kChunk;
            auto const* begin = reinterpret_cast<uint8_t const*>(data + off);
            bool wrote = false;
            for (int attempt = 0; attempt < 4 && !wrote; ++attempt) {
                DataWriter writer;
                writer.WriteBytes(array_view<uint8_t const>(begin, begin + nthis));
                auto status = chr.WriteValueAsync(writer.DetachBuffer(),
                                                  GattWriteOption::WriteWithoutResponse).get();
                if (status == GattCommunicationStatus::Success) {
                    wrote = true;
                    break;
                }
                wchar_t note[32];
                swprintf_s(note, L"write st %d", (int)status);
                BleLog(note);
                Sleep(30);
            }
            if (!wrote || g_cancel) return false;
            if (off + nthis < n) Sleep(15);
        }
        return true;
    } catch (hresult_error const& e) {
        wchar_t note[48];
        swprintf_s(note, L"write hr %08X", (unsigned)e.code());
        BleLog(note);
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace

int BleOpen(DWORD scan_ms, DWORD auth_ms, void (*status)(const wchar_t*, void*), void* ctx,
            bool (*abort_fn)(void*), void* abort_ctx)
{
    FILE* mark = nullptr;
    if (_wfopen_s(&mark, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && mark) {
        fputws(L"ble-open\n", mark);
        fclose(mark);
    }
    if (_wfopen_s(&mark, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && mark) {
        fputws(L"before-close\n", mark);
        fclose(mark);
    }
    BleClose();
    InterlockedExchange(&g_cancel, 0);
    LoadToken();
    if (_wfopen_s(&mark, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && mark) {
        fputws(L"before-apt\n", mark);
        fclose(mark);
    }
    try {
        EnsureApartment();
    } catch (...) {
        return 0;
    }
    if (_wfopen_s(&mark, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") == 0 && mark) {
        fputws(L"after-apt\n", mark);
        fclose(mark);
    }

    int result = 0;
    auto give_up = [&]() {
        return g_cancel || (abort_fn && abort_fn(abort_ctx));
    };
    /* This board's public address is stable. An advertisement watcher on a
     * wedged radio never returns, and the saved service object cannot write
     * the notify switch. Open the live device and rediscover it. */
    BleLog(L"open");
    HANDLE hit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hit) {
        ReleaseDevice();
        return 0;
    }
    uint64_t address = 0;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Passive);
    auto received = watcher.Received([&](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& args) {
        if (args.Advertisement().LocalName() == L"ESP32Monitor") {
            address = args.BluetoothAddress();
            SetEvent(hit);
        }
    });
    try {
    if (!g_device) {
        /* Connect only after an advertisement, so this is the live radio
         * and not a stale paired object. */
        BleLog(L"scan");
        watcher.Start();
        DWORD scan_begin = GetTickCount();
        DWORD wait = WAIT_TIMEOUT;
        while (!give_up() && GetTickCount() - scan_begin < scan_ms) {
            DWORD slice = scan_ms - (GetTickCount() - scan_begin);
            if (slice > 200) slice = 200;
            wait = WaitForSingleObject(hit, slice);
            if (wait == WAIT_OBJECT_0) break;
        }
        watcher.Stop();
        watcher.Received(received);
        if (address == 0) {
            BleLog(L"scan miss");
            address = 0x7C0C5F423396ULL;
        } else {
            wchar_t note[40];
            swprintf_s(note, L"scan hit %012llX", address & 0xFFFFFFFFFFFFULL);
            BleLog(note);
        }
        g_device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
        if (!g_device) {
            BleLog(L"bt fail device");
            if (status) status(L"bt fail device", ctx);
            CloseHandle(hit);
            return 0;
        }
    }

        /* Keep the radio link up for the whole session. Closing it is what
         * makes Settings fall back to 已配对. */
        try {
            auto access = g_device.RequestAccessAsync().get();
            wchar_t note[24];
            swprintf_s(note, L"access %d", (int)access);
            BleLog(note);
        } catch (hresult_error const& e) {
            wchar_t note[48];
            swprintf_s(note, L"access hr %08X", (unsigned)e.code());
            BleLog(note);
        }
        /* FromBluetoothAddressAsync does not open the radio. The session
         * has to ask for the link, or ConnectionStatus stays disconnected
         * and the service read never runs. */
        HoldConnection();
        DWORD link_begin = GetTickCount();
        while (!give_up() &&
               g_device.ConnectionStatus() != BluetoothConnectionStatus::Connected &&
               GetTickCount() - link_begin < 8000) {
            Sleep(200);
        }
        {
            wchar_t note[32];
            swprintf_s(note, L"link %d", (int)g_device.ConnectionStatus());
            BleLog(note);
        }
        /* A live service scan on this paired device returns Unreachable and
         * then Windows drops the link. The service it saved at pairing time
         * can be opened without that scan. */
        int bond = -1;
        try {
            bond = (int)g_device.DeviceInformation().Pairing().ProtectionLevel();
            wchar_t note[40];
            swprintf_s(note, L"bond %d", bond);
            BleLog(note);
        } catch (...) {}
        auto open_cached = [&]() -> bool {
            if (g_rx_chr && g_tx_chr) return true;
            try {
                BleLog(L"known ask");
                auto infos = DeviceInformation::FindAllAsync(
                    GattDeviceService::GetDeviceSelectorFromUuid(kService)).get();
                wchar_t note[40];
                swprintf_s(note, L"known %u", (unsigned)infos.Size());
                BleLog(note);
                for (auto const& info : infos) {
                    std::wstring id = info.Id().c_str();
                    for (auto& c : id) c = towupper(c);
                    if (id.find(L"7C0C5F423396") == std::wstring::npos) continue;
                    g_service = GattDeviceService::FromIdAsync(
                        info.Id(), GattSharingMode::SharedReadAndWrite).get();
                    if (!g_service) {
                        BleLog(L"known null");
                        continue;
                    }
                    BleLog(L"known open");
                    auto chrs = g_service.GetCharacteristicsAsync(BluetoothCacheMode::Cached).get();
                    int n = 0;
                    if (chrs.Status() == GattCommunicationStatus::Success) {
                        for (auto const& chr : chrs.Characteristics()) {
                            n++;
                            if (chr.Uuid() == kRx) g_rx_chr = chr;
                            if (chr.Uuid() == kTx) g_tx_chr = chr;
                        }
                    }
                    swprintf_s(note, L"chr cached %d %d", (int)chrs.Status(), n);
                    BleLog(note);
                    if (g_rx_chr && g_tx_chr) return true;
                }
            } catch (hresult_error const& e) {
                wchar_t note[48];
                swprintf_s(note, L"known hr %08X", (unsigned)e.code());
                BleLog(note);
            }
            return false;
        };
        bool waited = false;
        int down = 0;
        int fails = 0;
        while (!(g_rx_chr && g_tx_chr) && !give_up() && fails < 4) {
            if (g_device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                if (++down > 20) break;
                Sleep(200);
                continue;
            }
            down = 0;
            (void)waited;
            if (open_cached()) break;
            fails++;
            Sleep(400);
        }
        if (!g_rx_chr || !g_tx_chr) {
            BleLog(L"bt fail svc");
            if (status) status(L"bt fail svc", ctx);
            ReleaseDevice();
            CloseHandle(hit);
            return 0;
        }
        g_notify_token = g_tx_chr.ValueChanged([](auto const&, GattValueChangedEventArgs const& args) {
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            uint32_t n = reader.UnconsumedBufferLength();
            if (!n) return;
            com_array<uint8_t> bytes(n);
            reader.ReadBytes(bytes);
            NoteRx(bytes.data(), n);
        });
        /* The screen asks for the saved bond after Windows updates the
         * connection. Writing the notify switch before that times out. */
        BleLog(L"wait enc");
        Sleep(2500);
        GattCommunicationStatus cccd = GattCommunicationStatus::Unreachable;
        DWORD cccd_begin = GetTickCount();
        while (!give_up() && GetTickCount() - cccd_begin < 15000) {
            if (g_device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                Sleep(200);
                continue;
            }
            try {
                g_tx_chr.ProtectionLevel(GattProtectionLevel::EncryptionAndAuthenticationRequired);
                cccd = g_tx_chr.WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
            } catch (hresult_error const& e) {
                wchar_t note[48];
                swprintf_s(note, L"cccd hr %08X", (unsigned)e.code());
                BleLog(note);
                cccd = GattCommunicationStatus::Unreachable;
            }
            if (cccd == GattCommunicationStatus::Success) break;
            wchar_t note[32];
            swprintf_s(note, L"cccd %d link %d", (int)cccd,
                       (int)g_device.ConnectionStatus());
            BleLog(note);
            if (Win32Subscribe()) {
                cccd = GattCommunicationStatus::Success;
                break;
            }
            break;
        }
        if (cccd != GattCommunicationStatus::Success) {
            BleLog(L"bt fail notify");
            if (status) status(L"bt fail notify", ctx);
            ReleaseDevice();
            CloseHandle(hit);
            return 0;
        }
        BleLog(L"tap screen");
        if (status) status(L"tap screen", ctx);
        /* Ask Windows to encrypt with the bond, then write. A throw here
         * used to close the device and flip Settings back to 已配对. */
        try {
            g_rx_chr.ProtectionLevel(GattProtectionLevel::EncryptionAndAuthenticationRequired);
        } catch (...) {}
        Sleep(1500);

        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_rx.clear();
            g_open = true;
        }

        DWORD start = GetTickCount();
        DWORD next = start;
        while (!give_up() && GetTickCount() - start < auth_ms) {
            if (GetTickCount() >= next) {
                /* The first write is what makes Windows encrypt the bond.
                 * It fails until that finishes, so keep the link and retry. */
                if (!g_token.empty()) {
                    std::string hello = "BLE_TOKEN:" + g_token + "\n";
                    WriteChunked(g_rx_chr, hello.data(), (DWORD)hello.size());
                }
                const char who[] = "WHO_ARE_YOU?\n";
                WriteChunked(g_rx_chr, who, sizeof(who) - 1);
                next = GetTickCount() + 400;
            }
            std::string snap;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                snap = g_rx;
            }
            if (snap.find("SCARAB_CLIENT_OK") != std::string::npos) {
                result = 1;
                break;
            }
            Sleep(50);
        }
        if (result != 1) {
            bool linked = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                linked = g_open;
            }
            ReleaseDevice();
            result = (linked && !give_up()) ? -1 : 0;
        }
    } catch (hresult_error const& e) {
        wchar_t note[64];
        swprintf_s(note, L"hr %08X", (unsigned)e.code());
        BleLog(note);
        try { watcher.Stop(); } catch (...) {}
        ReleaseDevice();
        result = 0;
    } catch (...) {
        BleLog(L"exception");
        try { watcher.Stop(); } catch (...) {}
        ReleaseDevice();
        result = 0;
    }
    CloseHandle(hit);
    BleLog(result == 1 ? L"open ok" : (result < 0 ? L"open declined" : L"open fail"));
    return result;
}

void BleClose()
{
    InterlockedExchange(&g_cancel, 1);
    GattCharacteristic tx{nullptr};
    event_token token{};
    BluetoothLEDevice dev{nullptr};
    GattDeviceService svc{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        tx = g_tx_chr;
        token = g_notify_token;
        dev = g_device;
        svc = g_service;
        g_tx_chr = nullptr;
        g_rx_chr = nullptr;
        g_service = nullptr;
        g_device = nullptr;
        g_notify_token = {};
        g_open = false;
        g_rx.clear();
    }
    if (tx && token.value) {
        try { tx.ValueChanged(token); } catch (...) {}
    }
    if (svc) {
        try { svc.Close(); } catch (...) {}
    }
    CloseSession();
    if (dev) {
        try { dev.Close(); } catch (...) {}
    }
}

bool BleIsOpen()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_open && g_device != nullptr;
}

bool BleLinkUp()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_open || !g_device) return false;
    try {
        return g_device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
    } catch (...) {
        return false;
    }
}

bool BleWrite(const char* data, DWORD n)
{
    if (!data || !n) return false;
    GattCharacteristic chr{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_open || !g_rx_chr) return false;
        chr = g_rx_chr;
    }
    try {
        return WriteChunked(chr, data, n);
    } catch (...) {
        return false;
    }
}

void BleTake(std::string& out)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_rx.empty()) {
        out.append(g_rx);
        g_rx.clear();
    }
}
