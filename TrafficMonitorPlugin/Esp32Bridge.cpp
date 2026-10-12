/**
 * TrafficMonitor plugin: forward host monitor data to the ESP32 screen.
 *
 * TrafficMonitor has no serial protocol of its own. This DLL implements
 * ITMPlugin, and on each update writes the line the firmware already parses:
 *   CPU:..,CPUT:..,GPU:..,GPUT:..,VRAM:-1/-1,RAM:used/total,NET:TM,SPEED:--,DOWN:..,UP:..
 *
 * up_speed / down_speed from MonitorInfo are bytes per second.
 * Memory size is read with GlobalMemoryStatusEx because MonitorInfo only
 * carries a memory percentage. VRAM is dedicated plus shared GPU memory.
 * CPU temperature comes from TrafficMonitor when hardware monitoring is on,
 * otherwise from the LibreHardwareMonitor DLL shipped with TrafficMonitor.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <powerbase.h>
#include <powersetting.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <iptypes.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <setupapi.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

static void PluginTrace(const wchar_t* text)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"C:\\Users\\luojuly\\AppData\\Local\\Temp\\esp32_plugin_load.txt", L"at") != 0 || !f)
        return;
    fputws(text, f);
    fputws(L"\n", f);
    fclose(f);
}

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "powrprof.lib")

/* 0 off, 1 on, 2 dimmed. */
static const GUID kConsoleDisplayState = {
    0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}
};
/* 0 off, 1 on. Fired when the panel itself loses power. */
static const GUID kMonitorPowerOn = {
    0x02731015, 0x4510, 0x4526, {0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea}
};
/* Session display: 0 off, 1 on, 2 dimmed. Idle timeout uses this one. */
static const GUID kSessionDisplayStatus = {
    0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}
};

#include "PluginInterface.h"
#include "WeatherClient.h"
#include "BleClient.h"

namespace {

constexpr DWORD kBaud = 115200;

double gpu_memory_used_gb();
wchar_t* gpu_description();

bool query_vram(double* used_gb, double* total_gb)
{
    *used_gb = -1.0;
    *total_gb = -1.0;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    unsigned long long best_dedicated = 0;
    unsigned long long best_shared = 0;
    unsigned long long best_used = 0;
    wchar_t best_name[128] = {};
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        IDXGIAdapter3* adapter3 = nullptr;
        if (!software && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            unsigned long long used = 0;
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO shared_use{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                used += local.CurrentUsage;
            }
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared_use))) {
                used += shared_use.CurrentUsage;
            }
            const bool better = desc.DedicatedVideoMemory > best_dedicated ||
                (desc.DedicatedVideoMemory == best_dedicated && desc.SharedSystemMemory > best_shared);
            if (better && (desc.DedicatedVideoMemory > 0 || desc.SharedSystemMemory > 0)) {
                best_dedicated = desc.DedicatedVideoMemory;
                best_shared = desc.SharedSystemMemory;
                best_used = used;
                wcsncpy_s(best_name, desc.Description, _TRUNCATE);
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    const unsigned long long total_bytes = best_dedicated + best_shared;
    if (total_bytes == 0) return false;
    *total_gb = static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0);
    const double counted = gpu_memory_used_gb();
    if (counted >= 0.0) best_used = static_cast<unsigned long long>(counted);
    *used_gb = static_cast<double>(best_used) / (1024.0 * 1024.0 * 1024.0);
    if (*used_gb > *total_gb) *used_gb = *total_gb;
    wcsncpy_s(gpu_description(), 128, best_name, _TRUNCATE);
    return true;
}

wchar_t* gpu_description()
{
    static wchar_t name[128] = {};
    return name;
}

const wchar_t* find_word(const wchar_t* text, const wchar_t* word)
{
    const size_t n = wcslen(word);
    for (const wchar_t* p = text; *p; ++p) {
        if ((p == text || p[-1] == L' ') && _wcsnicmp(p, word, n) == 0 &&
            (p[n] == 0 || p[n] == L' ')) {
            return p;
        }
    }
    return nullptr;
}

void copy_words(const wchar_t* text, int words, char* out, size_t cap, bool upper)
{
    size_t j = 0;
    int got = 0;
    while (*text && got < words && j + 1 < cap) {
        while (*text == L' ') text++;
        if (!*text) break;
        if (j) out[j++] = ' ';
        while (*text && *text != L' ' && j + 1 < cap) {
            wchar_t c = *text++;
            if (c <= 32 || c >= 127) continue;
            if (upper && c >= L'a' && c <= L'z') c = c - L'a' + L'A';
            out[j++] = static_cast<char>(c);
        }
        got++;
    }
    while (j && out[j - 1] == ' ') j--;
    out[j] = 0;
}

void format_cpu_name(const wchar_t* full, char* out, size_t cap)
{
    out[0] = 0;
    const wchar_t* intel[] = {L"i9-", L"i7-", L"i5-", L"i3-"};
    for (const wchar_t* key : intel) {
        const wchar_t* hit = wcsstr(full, key);
        if (!hit) continue;
        size_t n = 0;
        while (hit[n] && hit[n] != L' ' && n + 1 < cap) {
            out[n] = static_cast<char>(hit[n]);
            n++;
        }
        out[n] = 0;
        return;
    }
    const wchar_t* ultra = find_word(full, L"Ultra");
    if (ultra) {
        copy_words(ultra, 3, out, cap, false);
        return;
    }
    const wchar_t* ryzen = find_word(full, L"Ryzen");
    if (ryzen) {
        const wchar_t* p = ryzen + 5;
        while (*p == L' ') p++;
        if (*p >= L'3' && *p <= L'9') {
            wchar_t built[48];
            const wchar_t* model = p + 1;
            while (*model == L' ') model++;
            swprintf_s(built, L"R%c %s", *p, model);
            wchar_t* space = wcschr(built + 3, L' ');
            if (space) *space = 0;
            copy_words(built, 2, out, cap, false);
            return;
        }
    }
    copy_words(full, 3, out, cap, false);
}

void format_gpu_name(const wchar_t* full, char* out, size_t cap)
{
    out[0] = 0;
    const wchar_t* keys[] = {L"RTX", L"GTX", L"RX", L"Arc"};
    for (const wchar_t* key : keys) {
        const wchar_t* hit = find_word(full, key);
        if (!hit) continue;
        copy_words(hit, 4, out, cap, true);
        return;
    }
    copy_words(full, 3, out, cap, true);
}

void query_cpu_model(char* out, size_t cap)
{
    static char cached[32] = {};
    static bool done = false;
    if (!done) {
        done = true;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            wchar_t name[256] = {};
            DWORD bytes = sizeof(name);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                 reinterpret_cast<BYTE*>(name), &bytes) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                format_cpu_name(name, cached, sizeof(cached));
            }
            RegCloseKey(key);
        }
    }
    strncpy_s(out, cap, cached, _TRUNCATE);
}

void query_gpu_model(char* out, size_t cap)
{
    format_gpu_name(gpu_description(), out, cap);
}

double gpu_memory_used_gb()
{
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER dedicated = nullptr;
    static PDH_HCOUNTER shared = nullptr;
    static bool failed = false;
    if (failed) return -1.0;
    if (!query) {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &dedicated) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &shared) != ERROR_SUCCESS) {
            failed = true;
            return -1.0;
        }
        PdhCollectQueryData(query);
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) return -1.0;

    auto sum_counter = [](PDH_HCOUNTER counter) -> double {
        DWORD bytes = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr) != PDH_MORE_DATA || bytes == 0) {
            return 0.0;
        }
        std::string buffer(bytes, '\0');
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, items) != ERROR_SUCCESS) return 0.0;
        double total = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            if (items[i].szName && wcscmp(items[i].szName, L"_Total") == 0) continue;
            if (items[i].FmtValue.largeValue > 0) total += static_cast<double>(items[i].FmtValue.largeValue);
        }
        return total;
    };
    return sum_counter(dedicated) + sum_counter(shared);
}

void query_link(char* type, size_t type_cap, char* speed, size_t speed_cap)
{
    snprintf(type, type_cap, "LAN");
    snprintf(speed, speed_cap, "--");
    ULONG size = 0;
    GetAdaptersAddresses(0, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
    if (size == 0) return;
    std::string buffer(size, '\0');
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(0, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                     GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addresses, &size) != NO_ERROR) {
        return;
    }

    IP_ADAPTER_ADDRESSES* best = nullptr;
    bool best_wifi = false;
    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        const bool wifi = adapter->IfType == IF_TYPE_IEEE80211;
        const bool ethernet = adapter->IfType == IF_TYPE_ETHERNET_CSMACD;
        if (!wifi && !ethernet) continue;
        if (!best || (wifi && !best_wifi) ||
            (wifi == best_wifi && adapter->TransmitLinkSpeed > best->TransmitLinkSpeed)) {
            best = adapter;
            best_wifi = wifi;
        }
    }
    if (!best) return;
    snprintf(type, type_cap, "%s", best_wifi ? "WiFi" : "LAN");
    const unsigned mbps = static_cast<unsigned>(best->TransmitLinkSpeed / 1000000ull);
    if (mbps > 0) snprintf(speed, speed_cap, "%u Mbps", mbps);
}

namespace OpenHardwareMonitorApi {

class IOpenHardwareMonitor {
public:
    virtual void GetHardwareInfo() = 0;
    virtual float CpuTemperature() = 0;
    virtual float GpuTemperature() = 0;
    virtual float HDDTemperature() = 0;
    virtual float MainboardTemperature() = 0;
    virtual float GpuUsage() = 0;
    virtual float CpuFreq() = 0;
    virtual float CpuUsage() = 0;
    virtual const std::map<std::wstring, float>& AllHDDTemperature() = 0;
    virtual const std::map<std::wstring, float>& AllCpuTemperature() = 0;
    virtual const std::map<std::wstring, float>& AllHDDUsage() = 0;
    virtual void SetCpuEnable(bool enable) = 0;
    virtual void SetGpuEnable(bool enable) = 0;
    virtual void SetHddEnable(bool enable) = 0;
    virtual void SetMainboardEnable(bool enable) = 0;
};

}  // namespace OpenHardwareMonitorApi

int temperature_from_map(const std::map<std::wstring, float>& temps, const wchar_t* name)
{
    const auto it = temps.find(name);
    if (it == temps.end()) return -1;
    const int value = static_cast<int>(it->second + 0.5f);
    if (value <= 0 || value > 120) return -1;
    return value;
}

struct OhmReader {
    HMODULE dll = nullptr;
    std::shared_ptr<OpenHardwareMonitorApi::IOpenHardwareMonitor> monitor;
    bool failed = false;
};

OhmReader& ohm_reader()
{
    static OhmReader reader;
    return reader;
}

bool ohm_open()
{
    OhmReader& reader = ohm_reader();
    if (reader.monitor) return true;
    if (reader.failed) return false;

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        reader.failed = true;
        return false;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"OpenHardwareMonitorApi.dll");
    reader.dll = LoadLibraryW(path);
    if (!reader.dll) {
        reader.failed = true;
        return false;
    }
    using CreateFn = std::shared_ptr<OpenHardwareMonitorApi::IOpenHardwareMonitor> (*)();
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(
        reader.dll,
        "?CreateInstance@OpenHardwareMonitorApi@@YA?AV?$shared_ptr@VIOpenHardwareMonitor@OpenHardwareMonitorApi@@@std@@XZ"));
    if (!create) {
        reader.failed = true;
        return false;
    }
    reader.monitor = create();
    if (!reader.monitor) {
        reader.failed = true;
        return false;
    }
    reader.monitor->SetCpuEnable(true);
    reader.monitor->SetGpuEnable(true);
    reader.monitor->GetHardwareInfo();
    Sleep(150);
    reader.monitor->GetHardwareInfo();
    return true;
}

struct OhmSample {
    DWORD tick = 0;
    int cpu = -1;
    int gpu = -1;
};

struct OhmPublished {
    volatile long cpu = -1;
    volatile long gpu = -1;
    volatile long load = -1;
    volatile long stop = 0;
};

OhmPublished& ohm_published()
{
    static OhmPublished published;
    return published;
}

DWORD WINAPI ohm_thread(LPVOID)
{
    if (!ohm_open()) return 0;
    auto monitor = ohm_reader().monitor;
    while (monitor && !ohm_published().stop) {
        monitor->GetHardwareInfo();
        const int package = temperature_from_map(monitor->AllCpuTemperature(), L"CPU Package");
        const int average = temperature_from_map(monitor->AllCpuTemperature(), L"Core Average");
        int cpu = -1;
        if (package > 0) cpu = package;
        else if (average > 0) cpu = average;
        else {
            for (const auto& item : monitor->AllCpuTemperature()) {
                const int value = static_cast<int>(item.second + 0.5f);
                if (value <= 0 || value > 120) continue;
                if (item.first.find(L"Distance") != std::wstring::npos) continue;
                if (item.first.find(L"Tj") != std::wstring::npos) continue;
                if (value > cpu) cpu = value;
            }
        }
        if (cpu <= 0) {
            const int fallback = static_cast<int>(monitor->CpuTemperature() + 0.5f);
            if (fallback > 0 && fallback <= 120) cpu = fallback;
        }
        const int gpu_temp = static_cast<int>(monitor->GpuTemperature() + 0.5f);
        const float gpu_load = monitor->GpuUsage();
        const int load = (gpu_load >= 0.0f && gpu_load <= 100.0f)
                             ? static_cast<int>(gpu_load + 0.5f) : -1;
        InterlockedExchange(&ohm_published().cpu, cpu);
        InterlockedExchange(&ohm_published().gpu, (gpu_temp > 0 && gpu_temp <= 120) ? gpu_temp : -1);
        InterlockedExchange(&ohm_published().load, load);
        Sleep(1000);
    }
    return 0;
}

void ohm_start()
{
    static HANDLE thread = nullptr;
    if (thread) return;
    ohm_published().stop = 0;
    thread = CreateThread(nullptr, 0, ohm_thread, nullptr, 0, nullptr);
}

struct NvmlApi {
    HMODULE dll = nullptr;
    int (*init)() = nullptr;
    int (*get_handle)(unsigned, void**) = nullptr;
    int (*get_temp)(void*, int, unsigned*) = nullptr;
    int (*get_rates)(void*, unsigned*) = nullptr;
    void* device = nullptr;
    bool ready = false;
    bool failed = false;
    bool rates_failed = false;
};

NvmlApi& nvml_api()
{
    static NvmlApi nvml;
    return nvml;
}

bool nvml_open()
{
    NvmlApi& nvml = nvml_api();
    if (nvml.ready) return true;
    if (nvml.failed) return false;
    const wchar_t* paths[] = {
        L"nvml.dll",
        L"C:\\Windows\\System32\\nvml.dll",
        L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll",
    };
    for (const wchar_t* path : paths) {
        nvml.dll = LoadLibraryW(path);
        if (nvml.dll) break;
    }
    if (!nvml.dll) {
        nvml.failed = true;
        return false;
    }
    nvml.init = reinterpret_cast<int (*)()>(GetProcAddress(nvml.dll, "nvmlInit_v2"));
    if (!nvml.init) {
        nvml.init = reinterpret_cast<int (*)()>(GetProcAddress(nvml.dll, "nvmlInit"));
    }
    nvml.get_handle = reinterpret_cast<int (*)(unsigned, void**)>(
        GetProcAddress(nvml.dll, "nvmlDeviceGetHandleByIndex_v2"));
    if (!nvml.get_handle) {
        nvml.get_handle = reinterpret_cast<int (*)(unsigned, void**)>(
            GetProcAddress(nvml.dll, "nvmlDeviceGetHandleByIndex"));
    }
    nvml.get_temp = reinterpret_cast<int (*)(void*, int, unsigned*)>(
        GetProcAddress(nvml.dll, "nvmlDeviceGetTemperature"));
    if (!nvml.init || !nvml.get_handle || !nvml.get_temp ||
        nvml.init() != 0 || nvml.get_handle(0, &nvml.device) != 0) {
        nvml.failed = true;
        return false;
    }
    nvml.ready = true;
    return true;
}

int query_gpu_temperature()
{
    if (!nvml_open()) return -1;
    NvmlApi& nvml = nvml_api();
    unsigned temperature = 0;
    if (nvml.get_temp(nvml.device, 0, &temperature) != 0) return -1;
    if (temperature == 0 || temperature > 120) return -1;
    return static_cast<int>(temperature);
}

/* GPU percent. 0 is a real idle reading. TrafficMonitor leaves this at -1
 * unless its hardware-monitor mask includes GPU usage. */
int query_gpu_usage()
{
    if (!nvml_open()) return -1;
    NvmlApi& nvml = nvml_api();
    if (nvml.rates_failed) return -1;
    if (!nvml.get_rates) {
        nvml.get_rates = reinterpret_cast<int (*)(void*, unsigned*)>(
            GetProcAddress(nvml.dll, "nvmlDeviceGetUtilizationRates"));
        if (!nvml.get_rates) {
            nvml.rates_failed = true;
            return -1;
        }
    }
    /* nvmlUtilization_t is { unsigned gpu; unsigned memory; }. */
    unsigned rates[2] = {0, 0};
    if (nvml.get_rates(nvml.device, rates) != 0) return -1;
    if (rates[0] > 100) return -1;
    return static_cast<int>(rates[0]);
}

/* ACPI thermal zones arrive as Kelvin (~300) or tenths of Kelvin (~3000).
 * Treating every value above 200 as tenths of Kelvin turns 301 K into a
 * negative temperature and the reading is dropped. */
int thermal_celsius(int raw)
{
    int celsius = raw;
    if (raw >= 2000 && raw <= 4500) celsius = (raw - 2732) / 10;
    else if (raw >= 250 && raw < 2000) celsius = raw - 273;
    if (celsius < 10 || celsius > 115) return -1;
    return celsius;
}

int query_cpu_temperature()
{
    static IWbemServices* services = nullptr;
    static bool failed = false;
    if (failed) return -1;
    if (!services) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
            failed = true;
            return -1;
        }
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);
        IWbemLocator* locator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, reinterpret_cast<void**>(&locator)))) {
            failed = true;
            return -1;
        }
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        const HRESULT connected = locator->ConnectServer(
            ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        SysFreeString(ns);
        locator->Release();
        if (FAILED(connected) || !services) {
            failed = true;
            return -1;
        }
        CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                          nullptr, EOAC_NONE);
    }

    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT Temperature FROM Win32_PerfFormattedData_Counters_ThermalZoneInformation");
    IEnumWbemClassObject* enumerator = nullptr;
    const HRESULT queried = services->ExecQuery(
        language, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(queried) || !enumerator) return -1;

    int best = -1;
    for (;;) {
        IWbemClassObject* object = nullptr;
        ULONG got = 0;
        if (enumerator->Next(500, 1, &object, &got) != S_OK || got == 0) break;
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(object->Get(L"Temperature", 0, &value, nullptr, nullptr))) {
            int raw = 0;
            if (value.vt == VT_I4) raw = value.lVal;
            else if (value.vt == VT_UI4) raw = static_cast<int>(value.ulVal);
            const int celsius = thermal_celsius(raw);
            if (celsius > best) best = celsius;
        }
        VariantClear(&value);
        object->Release();
    }
    enumerator->Release();
    return best;
}

class Esp32Plugin : public ITMPlugin {
public:
    Esp32Plugin()
    {
        PluginTrace(L"ctor");
        InitializeCriticalSection(&cs_);
        wcscpy_s(status_[0], L"...");
        wcscpy_s(status_[1], L"...");
    }

    IPluginItem* GetItem(int index) override
    {
        return index == 0 ? &item_ : nullptr;
    }

    void DataRequired() override {}

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index) {
        case TMI_NAME: return L"ESP32 Monitor";
        case TMI_DESCRIPTION: return L"Send TrafficMonitor data to the ESP32 screen";
        case TMI_AUTHOR: return L"ESP32Monitor";
        case TMI_COPYRIGHT: return L"Copyright (C) 2026";
        case TMI_VERSION: return L"1.0";
        case TMI_URL: return L"https://github.com/zhongyang219/TrafficMonitor";
        default: return L"";
        }
    }

    void OnMonitorInfo(const MonitorInfo& info) override
    {
        EnterCriticalSection(&cs_);
        info_ = info;
        has_info_ = true;
        LeaveCriticalSection(&cs_);
    }

    void OnInitialize(ITrafficMonitor* app) override
    {
        PluginTrace(L"init");
        app_ = app;
        /* TrafficMonitor already polls LibreHardwareMonitor. A second
         * instance in this process throws NullReferenceException. */
        weather_start();
        if (!thread_) {
            stop_ = false;
            if (!screen_event_) screen_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            thread_ = CreateThread(nullptr, 0, &Esp32Plugin::ThreadProc, this, 0, nullptr);
        }
        if (!watch_thread_) {
            watch_thread_ = CreateThread(nullptr, 0, &Esp32Plugin::WatchProc, this, 0, nullptr);
        }
    }

    void Shutdown()
    {
        stop_ = true;
        BleClose();
        ohm_published().stop = 1;
        weather_stop();
        if (screen_event_) SetEvent(screen_event_);
        if (watch_thread_) {
            if (watch_tid_) PostThreadMessageW(watch_tid_, WM_QUIT, 0, 0);
            WaitForSingleObject(watch_thread_, 2000);
            CloseHandle(watch_thread_);
            watch_thread_ = nullptr;
        }
        if (thread_) {
            WaitForSingleObject(thread_, 3000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        ClosePort();
        BleClose();
        if (screen_event_) {
            CloseHandle(screen_event_);
            screen_event_ = nullptr;
        }
    }

private:
    class Item : public IPluginItem {
    public:
        explicit Item(Esp32Plugin* owner) : owner_(owner) {}
        const wchar_t* GetItemName() const override { return L"ESP32"; }
        const wchar_t* GetItemId() const override { return L"esp32_monitor"; }
        const wchar_t* GetItemLableText() const override { return L"ESP32"; }
        const wchar_t* GetItemValueText() const override { return owner_->Status(); }
        const wchar_t* GetItemValueSampleText() const override { return L"COM10"; }
    private:
        Esp32Plugin* owner_;
    };

    Item item_{this};
    ITrafficMonitor* app_ = nullptr;
    MonitorInfo info_{};
    bool has_info_ = false;
    CRITICAL_SECTION cs_{};
    HANDLE thread_ = nullptr;
    HANDLE watch_thread_ = nullptr;
    DWORD watch_tid_ = 0;
    HANDLE screen_event_ = nullptr;
    volatile long panel_wanted_ = 1;
    volatile long panel_sent_ = -1;
    volatile long session_locked_ = 0;
    volatile long display_off_ = 0;
    /* 0 none, 1 power notification, 2 idle-timeout poll. */
    volatile long display_off_reason_ = 0;
    volatile bool stop_ = false;
        HANDLE port_ = INVALID_HANDLE_VALUE;
        bool saw_ack_ = false;
        DWORD ble_hold_until_ = 0;

    static void OnBleStatus(const wchar_t* text, void* ctx)
    {
        auto* self = static_cast<Esp32Plugin*>(ctx);
        self->SetStatus(text);
        self->WriteLink(text);
    }

    bool WriteOut(const char* data, DWORD n)
    {
        if (!data || !n) return false;
        if (BleIsOpen()) return BleWrite(data, n);
        if (port_ == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        return WriteFile(port_, data, n, &wrote, nullptr) && wrote == n;
    }

    void CloseLink()
    {
        BleClose();
        ClosePort();
    }
    wchar_t status_[2][64]{};
    volatile long status_i_ = 0;

    const wchar_t* Status() const
    {
        return status_[status_i_];
    }

    void SetStatus(const wchar_t* text)
    {
        long next = 1 - status_i_;
        wcsncpy_s(status_[next], text, _TRUNCATE);
        InterlockedExchange(&status_i_, next);
    }

    static bool UsbIsPresent(void* self)
    {
        wchar_t com[16] = {};
        return static_cast<Esp32Plugin*>(self)->FindEsp32Com(com, 16);
    }

    static DWORD WINAPI ThreadProc(LPVOID param)
    {
        static_cast<Esp32Plugin*>(param)->Run();
        return 0;
    }

    static DWORD WINAPI WatchProc(LPVOID param)
    {
        static_cast<Esp32Plugin*>(param)->WatchScreen();
        return 0;
    }

    static LRESULT CALLBACK ScreenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Esp32Plugin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_WTSSESSION_CHANGE) {
            if (wp == WTS_SESSION_LOCK) self->NoteSession(true);
            else if (wp == WTS_SESSION_UNLOCK) self->NoteSession(false);
        } else if (msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE) {
            self->OnPowerSetting(reinterpret_cast<POWERBROADCAST_SETTING*>(lp));
        } else if (msg == WM_POWERBROADCAST && wp == PBT_APMSUSPEND) {
            self->NoteDisplay(false, 1);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void SetPanelWanted(bool on)
    {
        long next = on ? 1 : 0;
        if (InterlockedExchange(&panel_wanted_, next) != next && screen_event_) {
            SetEvent(screen_event_);
        }
    }

    void NoteSession(bool locked)
    {
        long prev = InterlockedExchange(&session_locked_, locked ? 1 : 0);
        if ((prev != 0) == locked) return;
        if (locked) SetPanelWanted(false);
        else if (InterlockedCompareExchange(&display_off_, 0, 0) == 0) SetPanelWanted(true);
    }

    /* reason 1: a power notification. reason 2: the idle-timeout poll. */
    void NoteDisplay(bool on, long reason)
    {
        long off = on ? 0 : 1;
        if (InterlockedExchange(&display_off_, off) == off) return;
        InterlockedExchange(&display_off_reason_, on ? 0 : reason);
        /* Turning the monitor on lights the panel, including the lock screen. */
        SetPanelWanted(on);
    }

    void OnPowerSetting(const POWERBROADCAST_SETTING* setting)
    {
        if (!setting || setting->DataLength < sizeof(DWORD)) return;
        const GUID& id = setting->PowerSetting;
        if (!IsEqualGUID(id, kConsoleDisplayState) &&
            !IsEqualGUID(id, kSessionDisplayStatus) &&
            !IsEqualGUID(id, kMonitorPowerOn)) {
            return;
        }
        DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
        /* 0 is off. Dimmed (2) still leaves the picture up. */
        NoteDisplay(state != 0, 1);
    }

    static ULONG CALLBACK PowerNotifyProc(PVOID context, ULONG type, PVOID setting)
    {
        auto* self = static_cast<Esp32Plugin*>(context);
        if (!self || type != PBT_POWERSETTINGCHANGE) return ERROR_SUCCESS;
        self->OnPowerSetting(static_cast<POWERBROADCAST_SETTING*>(setting));
        return ERROR_SUCCESS;
    }

    /* Display timeout does not lock the session, and Windows often never
     * delivers a power setting for that. The video-idle policy is the same
     * timer the monitor uses. An off raised by a real notification is left
     * alone, so a hotkey blank is not undone while the user is still idle. */
    void PollIdleDisplay()
    {
        SYSTEM_POWER_POLICY policy{};
        if (CallNtPowerInformation(SystemPowerPolicyCurrent, nullptr, 0,
                                   &policy, sizeof(policy)) != 0) {
            return;
        }
        DWORD timeout = policy.VideoTimeout;
        if (timeout == 0 || timeout > 24u * 60u * 60u) return;

        EXECUTION_STATE execution = 0;
        if (CallNtPowerInformation(SystemExecutionState, nullptr, 0,
                                   &execution, sizeof(execution)) == 0 &&
            (execution & ES_DISPLAY_REQUIRED) != 0) {
            if (InterlockedCompareExchange(&display_off_reason_, 0, 0) == 2)
                NoteDisplay(true, 0);
            return;
        }

        LASTINPUTINFO last{};
        last.cbSize = sizeof(last);
        if (!GetLastInputInfo(&last)) return;
        DWORD idle_ms = GetTickCount() - last.dwTime;
        if (idle_ms >= timeout * 1000u) {
            if (InterlockedCompareExchange(&display_off_, 0, 0) == 0)
                NoteDisplay(false, 2);
        } else if (InterlockedCompareExchange(&display_off_reason_, 0, 0) == 2) {
            NoteDisplay(true, 0);
        }
    }

    void WatchScreen()
    {
        watch_tid_ = GetCurrentThreadId();
        if (!screen_event_) screen_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        WNDCLASSW wc{};
        wc.lpfnWndProc = &Esp32Plugin::ScreenWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Esp32ScreenWatch";
        RegisterClassW(&wc);
        /* A message-only window never receives monitor power notifications. */
        HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    wc.lpszClassName, L"", WS_POPUP,
                                    0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (hwnd) WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);

        const GUID* settings[] = {
            &kConsoleDisplayState, &kMonitorPowerOn, &kSessionDisplayStatus
        };
        HPOWERNOTIFY window_regs[3]{};
        int nwindow = 0;
        if (hwnd) {
            for (const GUID* guid : settings) {
                HPOWERNOTIFY handle = RegisterPowerSettingNotification(
                    hwnd, guid, DEVICE_NOTIFY_WINDOW_HANDLE);
                if (handle) window_regs[nwindow++] = handle;
            }
        }

        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params{};
        params.Callback = &Esp32Plugin::PowerNotifyProc;
        params.Context = this;
        HPOWERNOTIFY callback_regs[3]{};
        int ncallback = 0;
        for (const GUID* guid : settings) {
            HPOWERNOTIFY handle = nullptr;
            DWORD status = PowerSettingRegisterNotification(
                guid, DEVICE_NOTIFY_CALLBACK, reinterpret_cast<HANDLE>(&params), &handle);
            if (status == ERROR_SUCCESS && handle) callback_regs[ncallback++] = handle;
        }

        DWORD last_poll = 0;
        while (!stop_) {
            DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
            if (stop_) break;
            if (wait == WAIT_OBJECT_0) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        stop_ = true;
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            DWORD now = GetTickCount();
            if (now - last_poll >= 500) {
                last_poll = now;
                PollIdleDisplay();
            }
        }

        for (int i = 0; i < ncallback; ++i)
            PowerSettingUnregisterNotification(callback_regs[i]);
        for (int i = 0; i < nwindow; ++i)
            UnregisterPowerSettingNotification(window_regs[i]);
        if (hwnd) {
            WTSUnRegisterSessionNotification(hwnd);
            DestroyWindow(hwnd);
        }
    }

    void SendPanelState()
    {
        if (!BleIsOpen() && port_ == INVALID_HANDLE_VALUE) return;
        long want = panel_wanted_;
        if (want != 0 && want != 1) return;
        if (want == panel_sent_) return;
        char line[16];
        int n = snprintf(line, sizeof(line), "PC_SCREEN:%ld\n", want);
        if (n > 0 && WriteOut(line, (DWORD)n)) panel_sent_ = want;
    }

    void Run()
    {
        PluginTrace(L"run");
        SetStatus(L"...");
        while (!stop_) {
            PluginTrace(L"loop");
            wchar_t usb_com[16] = {};
            bool usb_present = FindEsp32Com(usb_com, 16);
            PluginTrace(usb_present ? L"usb 1" : L"usb 0");
            /* The cable is gone. A write on the old handle can sit there
             * forever, so Bluetooth never starts and Windows keeps flipping
             * the paired device on its own. */
            if (!usb_present && port_ != INVALID_HANDLE_VALUE) {
                ClosePort();
                continue;
            }
            /* The cable enumerates even while Bluetooth is up. USB wins. */
            if (BleIsOpen() && usb_present) {
                CloseLink();
                continue;
            }
            if (!BleIsOpen() && port_ == INVALID_HANDLE_VALUE) {
                if (usb_present) {
                    if (!TryPort(usb_com, 8000)) {
                        SetStatus(L"no device");
                        WriteLink(L"no device");
                        for (int i = 0; i < 10 && !stop_; i++) Sleep(100);
                        continue;
                    }
                    SavePort(usb_com);
                    panel_sent_ = -1;
                } else {
                    WriteLink(L"ble");
                    bool hold = ble_hold_until_ != 0 && (LONG)(ble_hold_until_ - GetTickCount()) > 0;
                    int ble = 0;
                    DWORD ble_begin = GetTickCount();
                    while (!hold && ble == 0 && !stop_ &&
                           (LONG)(GetTickCount() - ble_begin) < 8000) {
                        if (FindEsp32Com(usb_com, 16)) break;
                        ble = BleOpen(3000, 35000, &Esp32Plugin::OnBleStatus, this,
                                      &Esp32Plugin::UsbIsPresent, this);
                        if (ble == 0 && !stop_) Sleep(2000);
                    }
                    if (ble == 1 && FindEsp32Com(usb_com, 16)) {
                        CloseLink();
                        continue;
                    }
                    if (ble == 1) {
                        panel_sent_ = -1;
                        SetStatus(L"Bluetooth");
                        WriteLink(L"bluetooth");
                    } else {
                        /* No tap rejects the link. Retry so the next
                         * connection brings the confirm screen back. */
                        SetStatus(L"no device");
                        WriteLink(L"no device");
                        for (int i = 0; i < 20 && !stop_; i++) Sleep(100);
                        continue;
                    }
                }
            }
            SendPanelState();
            /* One refused write used to close a link that had just finished
             * the handshake, and the next open flipped Windows back to 已配对. */
            auto deliver = [&]() -> bool {
                for (int attempt = 0; attempt < 8 && !stop_; ++attempt) {
                    if (SendSample()) return true;
                    if (!BleIsOpen()) return false;
                    Sleep(300);
                }
                return false;
            };
            auto keep_link = [&]() {
                if (!BleLinkUp()) return false;
                Sleep(500);
                return true;
            };
            if (ConsumeNeedData()) {
                if (!deliver()) {
                    if (keep_link()) continue;
                    CloseLink();
                    SetStatus(L"no device");
                    WriteLink(L"send failed");
                    Sleep(500);
                    continue;
                }
            }
            if (!deliver()) {
                if (keep_link()) continue;
                CloseLink();
                SetStatus(L"no device");
                WriteLink(L"send failed");
                Sleep(500);
                continue;
            }
            if (screen_event_) WaitForSingleObject(screen_event_, 1000);
            else Sleep(1000);
        }
    }

    /* USB-Serial/JTAG on this board. Present means the cable is plugged in. */
    bool FindEsp32Com(wchar_t* out, DWORD cap)
    {
        out[0] = 0;
        static const GUID kPorts = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO info = SetupDiGetClassDevsW(&kPorts, nullptr, nullptr, DIGCF_PRESENT);
        if (info == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &dev); ++i) {
            wchar_t id[512] = {};
            if (!SetupDiGetDeviceRegistryPropertyW(info, &dev, SPDRP_HARDWAREID, nullptr,
                                                   reinterpret_cast<PBYTE>(id), sizeof(id), nullptr)) {
                continue;
            }
            bool match = false;
            for (const wchar_t* p = id; *p; p += wcslen(p) + 1) {
                wchar_t up[512];
                wcsncpy_s(up, p, _TRUNCATE);
                _wcsupr_s(up);
                if (wcsstr(up, L"VID_303A&PID_1001")) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            HKEY key = SetupDiOpenDevRegKey(info, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            wchar_t port[32] = {};
            DWORD size = sizeof(port);
            if (RegQueryValueExW(key, L"PortName", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(port), &size) == ERROR_SUCCESS &&
                IsComName(port)) {
                wcsncpy_s(out, cap, port, _TRUNCATE);
                found = true;
            }
            RegCloseKey(key);
            if (found) break;
        }
        SetupDiDestroyDeviceInfoList(info);
        return found;
    }

    bool OpenEsp32()
    {
        wchar_t saved[16] = {};
        LoadSavedPort(saved, 16);
        if (saved[0] && TryPort(saved, 8000)) return true;

        wchar_t devices[32768];
        DWORD n = QueryDosDeviceW(nullptr, devices, 32768);
        if (n == 0) return false;
        for (const wchar_t* p = devices; *p; p += wcslen(p) + 1) {
            if (stop_) return false;
            if (!IsComName(p)) continue;
            if (saved[0] && _wcsicmp(p, saved) == 0) continue;
            if (TryPort(p, 700)) {
                SavePort(p);
                return true;
            }
        }
        return false;
    }

    static bool IsComName(const wchar_t* name)
    {
        if (_wcsnicmp(name, L"COM", 3) != 0) return false;
        if (!name[3]) return false;
        for (const wchar_t* c = name + 3; *c; ++c) {
            if (*c < L'0' || *c > L'9') return false;
        }
        return true;
    }

    bool TryPort(const wchar_t* com, DWORD handshake_ms)
    {
        wchar_t path[32];
        swprintf_s(path, L"\\\\.\\%s", com);
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) {
            CloseHandle(h);
            return false;
        }
        dcb.BaudRate = kBaud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        if (!SetCommState(h, &dcb)) {
            CloseHandle(h);
            return false;
        }
        EscapeCommFunction(h, CLRDTR);
        EscapeCommFunction(h, CLRRTS);
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = 50;
        to.ReadTotalTimeoutConstant = 80;
        to.ReadTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(h, &to);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        /* CreateFile resets the USB-JTAG chip. Keep this handle open and
         * repeat the handshake until the firmware's RX task is up. Closing
         * and reopening would reset it again before it can answer. */
        const char hello[] = "WHO_ARE_YOU?\n";
        std::string acc;
        DWORD start = GetTickCount();
        DWORD next_hello = start;
        while (!stop_ && GetTickCount() - start < handshake_ms) {
            if (GetTickCount() >= next_hello) {
                DWORD wrote = 0;
                if (!WriteFile(h, hello, sizeof(hello) - 1, &wrote, nullptr)) {
                    CloseHandle(h);
                    return false;
                }
                next_hello = GetTickCount() + 400;
            }
            char tmp[256];
            DWORD got = 0;
            if (!ReadFile(h, tmp, sizeof(tmp), &got, nullptr)) {
                CloseHandle(h);
                return false;
            }
            if (got) {
                acc.append(tmp, tmp + got);
                if (acc.size() > 4096) acc.erase(0, acc.size() - 2048);
            }
            if (acc.find("SCARAB_CLIENT_OK") != std::string::npos) {
                port_ = h;
                SetStatus(com);
                WriteLink(com);
                return true;
            }
        }
        CloseHandle(h);
        return false;
    }

    bool ConsumeNeedData()
    {
        if (BleIsOpen()) {
            std::string acc;
            DWORD start = GetTickCount();
            while (GetTickCount() - start < 150) {
                BleTake(acc);
                if (acc.find("NEED_DATA") != std::string::npos ||
                    acc.find("DATA_OK:") != std::string::npos) {
                    break;
                }
                Sleep(20);
            }
            if (acc.find("DATA_OK:") != std::string::npos) saw_ack_ = true;
            return acc.find("NEED_DATA") != std::string::npos;
        }
        if (port_ == INVALID_HANDLE_VALUE) return false;
        bool need = false;
        char tmp[512];
        std::string acc;
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 150) {
            DWORD got = 0;
            if (!ReadFile(port_, tmp, sizeof(tmp), &got, nullptr)) return false;
            if (!got) break;
            acc.append(tmp, tmp + got);
            if (acc.size() > 2048) acc.erase(0, acc.size() - 1024);
        }
        if (acc.find("NEED_DATA") != std::string::npos) need = true;
        if (acc.find("DATA_OK:") != std::string::npos) saw_ack_ = true;
        return need;
    }

    void WriteLink(const wchar_t* text)
    {
        wchar_t path[MAX_PATH];
        DllPath(path, MAX_PATH);
        wcscat_s(path, L"link.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wt") != 0 || !f) return;
        fputws(text, f);
        fputws(L"\n", f);
        fclose(f);
    }

    void ClosePort()
    {
        if (port_ != INVALID_HANDLE_VALUE) {
            CloseHandle(port_);
            port_ = INVALID_HANDLE_VALUE;
        }
        panel_sent_ = -1;
    }

    bool SendSample()
    {
        MonitorInfo info{};
        bool has = false;
        EnterCriticalSection(&cs_);
        info = info_;
        has = has_info_;
        LeaveCriticalSection(&cs_);
        if (!has && app_ != nullptr) {
            info.up_speed = static_cast<unsigned long long>(app_->GetMonitorValue(ITrafficMonitor::MI_UP));
            info.down_speed = static_cast<unsigned long long>(app_->GetMonitorValue(ITrafficMonitor::MI_DOWN));
            info.cpu_usage = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_CPU));
            info.memory_usage = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_MEMORY));
            info.gpu_usage = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_GPU_USAGE));
            info.cpu_temperature = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_CPU_TEMP));
            info.gpu_temperature = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_GPU_TEMP));
            has = true;
        }
        if (!has) return true;

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        double ram_used = -1.0;
        double ram_total = -1.0;
        if (GlobalMemoryStatusEx(&mem) && mem.ullTotalPhys > 0) {
            ram_total = mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
            unsigned long long used = mem.ullTotalPhys - mem.ullAvailPhys;
            ram_used = used / (1024.0 * 1024.0 * 1024.0);
        }

        if (app_ != nullptr) {
            const int from_app_cpu = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_CPU_TEMP));
            const int from_app_gpu = static_cast<int>(app_->GetMonitorValue(ITrafficMonitor::MI_GPU_TEMP));
            if (from_app_cpu > 0) info.cpu_temperature = from_app_cpu;
            if (from_app_gpu > 0) info.gpu_temperature = from_app_gpu;
        }
        const int ohm_cpu = static_cast<int>(ohm_published().cpu);
        const int ohm_gpu = static_cast<int>(ohm_published().gpu);
        int cpu_temp = info.cpu_temperature > 0 ? info.cpu_temperature : ohm_cpu;
        if (cpu_temp <= 0) cpu_temp = query_cpu_temperature();
        int gpu_temp = info.gpu_temperature > 0 ? info.gpu_temperature : ohm_gpu;
        if (gpu_temp <= 0) gpu_temp = query_gpu_temperature();
        int gpu_usage = info.gpu_usage;
        if (gpu_usage < 0 && app_ != nullptr) {
            const double from_app = app_->GetMonitorValue(ITrafficMonitor::MI_GPU_USAGE);
            if (from_app >= 0.0 && from_app <= 100.0) {
                gpu_usage = static_cast<int>(from_app + 0.5);
            }
        }
        if (gpu_usage < 0) {
            const int ohm_load = static_cast<int>(ohm_published().load);
            if (ohm_load >= 0 && ohm_load <= 100) gpu_usage = ohm_load;
        }
        if (gpu_usage < 0) gpu_usage = query_gpu_usage();
        double vram_used = -1.0;
        double vram_total = -1.0;
        query_vram(&vram_used, &vram_total);
        double down = info.down_speed / (1024.0 * 1024.0);
        double up = info.up_speed / (1024.0 * 1024.0);
        char net_type[16];
        char net_speed[16];
        query_link(net_type, sizeof(net_type), net_speed, sizeof(net_speed));

        WeatherSnap weather{};
        weather_copy(&weather);
        char city[24];
        snprintf(city, sizeof(city), "%s", weather.city[0] ? weather.city : "-");

        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (!weather_network_time(&weather, &year, &month, &day, &hour, &minute, &second)) {
            SYSTEMTIME clk;
            GetLocalTime(&clk);
            year = clk.wYear;
            month = clk.wMonth;
            day = clk.wDay;
            hour = clk.wHour;
            minute = clk.wMinute;
            second = clk.wSecond;
        }
        char line[512];
        snprintf(line, sizeof(line),
                 "CPU:%d,CPUT:%d,GPU:%d,GPUT:%d,VRAM:%.2f/%.2f,RAM:%.2f/%.2f,NET:%s,SPEED:%s,DOWN:%.3f,UP:%.3f,WTEMP:%d,WCODE:%d,WHUM:%d,WCITY:%s,SRISE:%d,SSET:%d,CLK:%04d-%02d-%02dT%02d:%02d:%02d\n",
                 info.cpu_usage, cpu_temp, gpu_usage, gpu_temp,
                 vram_used, vram_total, ram_used, ram_total, net_type, net_speed, down, up,
                 weather.temp, weather.code, weather.hum, city,
                 weather.rise_min, weather.set_min,
                 year, month, day, hour, minute, second);

        char cpu_model[32];
        char gpu_model[32];
        query_cpu_model(cpu_model, sizeof(cpu_model));
        query_gpu_model(gpu_model, sizeof(gpu_model));
        char name_line[80];
        if (cpu_model[0]) {
            snprintf(name_line, sizeof(name_line), "NAME_CPU=%s\n", cpu_model);
            WriteOut(name_line, (DWORD)strlen(name_line));
        }
        if (gpu_model[0]) {
            snprintf(name_line, sizeof(name_line), "NAME_GPU=%s\n", gpu_model);
            WriteOut(name_line, (DWORD)strlen(name_line));
        }

        if (!WriteOut(line, (DWORD)strlen(line))) return false;
        wchar_t note[200];
        swprintf_s(note, L"%s %s cpu=%d cput=%d gpu=%d gput=%d vram=%.2f/%.2f wx=%d %hs",
                   BleIsOpen() ? L"ble" : L"usb",
                   saw_ack_ ? L"ack" : L"noack",
                   info.cpu_usage, cpu_temp, gpu_usage, gpu_temp,
                   vram_used, vram_total, weather.temp, city);
        WriteLink(note);
        return true;
    }

    static void DllPath(wchar_t* out, DWORD cap)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Esp32Plugin::DllPath), &mod);
        GetModuleFileNameW(mod, out, cap);
        wchar_t* slash = wcsrchr(out, L'\\');
        if (slash) *(slash + 1) = 0;
    }

    void LoadSavedPort(wchar_t* out, DWORD cap)
    {
        out[0] = 0;
        wchar_t path[MAX_PATH];
        DllPath(path, MAX_PATH);
        wcscat_s(path, L"port.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rt") != 0 || !f) return;
        fgetws(out, (int)cap, f);
        fclose(f);
        size_t n = wcslen(out);
        while (n && (out[n - 1] == L'\n' || out[n - 1] == L'\r' || out[n - 1] == L' ')) {
            out[--n] = 0;
        }
    }

    void SavePort(const wchar_t* com)
    {
        wchar_t path[MAX_PATH];
        DllPath(path, MAX_PATH);
        wcscat_s(path, L"port.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wt") != 0 || !f) return;
        fputws(com, f);
        fclose(f);
    }
};

Esp32Plugin g_plugin;

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    PluginTrace(L"instance");
    return &g_plugin;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) PluginTrace(L"attach");
    if (reason == DLL_PROCESS_DETACH) {
        g_plugin.Shutdown();
    }
    return TRUE;
}

}  // namespace
