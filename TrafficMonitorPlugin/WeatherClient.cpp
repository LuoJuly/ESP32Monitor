/**
 * Weather for the ESP32 page.
 *
 * Windows location (Wi-Fi) is the precise fix. ipwho.is supplies the city
 * and a second fix. TUN fake-ip DNS rewrites every port-53 answer, so the
 * real address comes from DNS-over-HTTPS and the TCP connection is bound
 * to the physical NIC, past the tunnel.
 */

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#define _CRT_SECURE_NO_WARNINGS
#include "WeatherClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <schannel.h>
#include <security.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Foundation.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "windowsapp.lib")

namespace {

struct Published {
    CRITICAL_SECTION cs{};
    bool ready = false;
    WeatherSnap snap{};
    volatile long stop = 0;
    HANDLE thread = nullptr;
    volatile uintptr_t sock = 0;
};

Published& published()
{
    static Published state;
    return state;
}

struct Nic {
    ULONG index = 0;
    IN_ADDR addr{};
    bool ok = false;
};

static bool virtual_name(const wchar_t* text)
{
    if (!text || !text[0]) return false;
    wchar_t buf[260];
    wcsncpy_s(buf, text, _TRUNCATE);
    _wcslwr_s(buf);
    const wchar_t* keys[] = {
        L"wintun", L"wireguard", L"tunnel", L"tap-windows", L"tap adapter",
        L"clash", L"sing-box", L"singbox", L"nordlynx", L"openvpn",
        L"vpn", L"tailscale", L"zerotier", L"hamachi", L"virtual",
        L"vethernet", L"hyper-v", L"bluetooth", L"loopback", L"pseudo",
        L"npcap", L"radmin", L"softether", L"outline", L"proton",
        L"mullvad", L"expressvpn", L"surfshark", L"windscribe", L"astrill",
        L"letsvpn", L"isatap", L"teredo", L"wan miniport", L"wi-fi direct",
        L"wifi direct", L"secure connect", L"ninja"
    };
    for (const wchar_t* key : keys) {
        if (wcsstr(buf, key)) return true;
    }
    return false;
}

static Nic physical_nic()
{
    Nic best;
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG err = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (err == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        err = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (err != NO_ERROR) return best;

    std::vector<ULONG> tun;
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    for (auto* a = addrs; a; a = a->Next) {
        bool virt = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL ||
                    a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_PROP_VIRTUAL ||
                    virtual_name(a->FriendlyName) || virtual_name(a->Description);
        if (virt) tun.push_back(a->IfIndex);
    }

    ULONG tsize = 0;
    if (GetIpForwardTable(nullptr, &tsize, FALSE) != ERROR_INSUFFICIENT_BUFFER) return best;
    auto* table = reinterpret_cast<MIB_IPFORWARDTABLE*>(malloc(tsize));
    DWORD best_metric = 0xffffffff;
    if (table && GetIpForwardTable(table, &tsize, FALSE) == NO_ERROR) {
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const auto& row = table->table[i];
            if (row.dwForwardDest != 0 || row.dwForwardMask != 0) continue;
            bool skip = false;
            for (ULONG id : tun) if (id == row.dwForwardIfIndex) skip = true;
            if (skip || row.dwForwardMetric1 >= best_metric) continue;
            best_metric = row.dwForwardMetric1;
            best.index = row.dwForwardIfIndex;
        }
    }
    free(table);
    if (!best.index) return best;

    for (auto* a = addrs; a; a = a->Next) {
        if (a->IfIndex != best.index) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            best.addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            best.ok = true;
            break;
        }
    }
    return best;
}

static void arm_socket(SOCKET s)
{
    published().sock = (uintptr_t)s;
}

static void disarm_socket(SOCKET s)
{
    uintptr_t cur = (uintptr_t)s;
    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&published().sock),
                                      nullptr, reinterpret_cast<void*>(cur));
}

static bool send_all(SOCKET s, const void* data, int len)
{
    const char* p = static_cast<const char*>(data);
    int sent = 0;
    while (sent < len) {
        if (published().stop) return false;
        int n = send(s, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool verify_server(CtxtHandle* ctx, const wchar_t* host)
{
    PCCERT_CONTEXT cert = nullptr;
    if (QueryContextAttributes(ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert)
        return false;
    CERT_CHAIN_PARA chain_para{};
    chain_para.cbSize = sizeof(chain_para);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    BOOL got = CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &chain_para, 0, nullptr, &chain);
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = const_cast<wchar_t*>(host);
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    bool ok = got && chain &&
              CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) &&
              status.dwError == 0;
    if (chain) CertFreeCertificateChain(chain);
    CertFreeCertificateContext(cert);
    return ok;
}

static bool https_via_nic(const Nic& nic, const IN_ADDR& ip, const char* host, const wchar_t* host_w,
                          const char* path, std::string* body)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    arm_socket(s);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr = nic.addr;
    bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    DWORD ifbe = htonl(nic.index);
    setsockopt(s, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char*>(&ifbe), sizeof(ifbe));
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(443);
    remote.sin_addr = ip;
    connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv{8, 0};
    int sel = select(0, nullptr, &wset, nullptr, &tv);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    int soerr = 0;
    int solen = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &solen);
    if (sel <= 0 || soerr != 0 || published().stop) {
        disarm_socket(s);
        closesocket(s);
        return false;
    }
    DWORD timeout = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    CredHandle ch{};
    TimeStamp ts{};
    if (AcquireCredentialsHandleA(nullptr, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
                                  nullptr, &cred, nullptr, nullptr, &ch, &ts) != SEC_E_OK) {
        disarm_socket(s);
        closesocket(s);
        return false;
    }

    CtxtHandle ctx{};
    bool have_ctx = false;
    std::vector<char> io;
    bool reading = false;
    SECURITY_STATUS st = SEC_I_CONTINUE_NEEDED;
    while (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE || st == SEC_I_INCOMPLETE_CREDENTIALS) {
        if (published().stop) {
            st = SEC_E_INTERNAL_ERROR;
            break;
        }
        if (io.empty() || st == SEC_E_INCOMPLETE_MESSAGE) {
            if (reading) {
                char tmp[4096];
                int n = recv(s, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    st = SEC_E_INTERNAL_ERROR;
                    break;
                }
                io.insert(io.end(), tmp, tmp + n);
            } else {
                reading = true;
            }
        }
        SecBuffer inb[2]{};
        inb[0].BufferType = SECBUFFER_TOKEN;
        inb[0].pvBuffer = io.empty() ? nullptr : io.data();
        inb[0].cbBuffer = (unsigned)io.size();
        inb[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc ind{SECBUFFER_VERSION, 2, inb};
        SecBuffer outb[1]{};
        outb[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outd{SECBUFFER_VERSION, 1, outb};
        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;
        DWORD outflags = 0;
        st = InitializeSecurityContextA(&ch, have_ctx ? &ctx : nullptr,
                                        have_ctx ? nullptr : const_cast<SEC_CHAR*>(host),
                                        flags, 0, SECURITY_NATIVE_DREP,
                                        (have_ctx || !io.empty()) ? &ind : nullptr,
                                        0, &ctx, &outd, &outflags, &ts);
        have_ctx = true;
        if ((st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED ||
             (FAILED(st) && st != SEC_E_INCOMPLETE_MESSAGE)) &&
            outb[0].cbBuffer && outb[0].pvBuffer) {
            if (!send_all(s, outb[0].pvBuffer, (int)outb[0].cbBuffer)) st = SEC_E_INTERNAL_ERROR;
            FreeContextBuffer(outb[0].pvBuffer);
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) {
            if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
                size_t extra = inb[1].cbBuffer;
                io.erase(io.begin(), io.end() - (ptrdiff_t)extra);
            } else {
                io.clear();
            }
        }
        if (FAILED(st) && st != SEC_E_INCOMPLETE_MESSAGE) break;
    }

    SecPkgContext_StreamSizes sizes{};
    bool ok = st == SEC_E_OK && have_ctx && verify_server(&ctx, host_w) &&
              QueryContextAttributes(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) == SEC_E_OK;
    if (ok) {
        char req[512];
        int reqn = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Esp32Monitor\r\nConnection: close\r\n\r\n",
                            path, host);
        std::vector<char> enc(sizes.cbHeader + reqn + sizes.cbTrailer);
        memcpy(enc.data() + sizes.cbHeader, req, reqn);
        SecBuffer eb[4]{};
        eb[0].cbBuffer = sizes.cbHeader;
        eb[0].BufferType = SECBUFFER_STREAM_HEADER;
        eb[0].pvBuffer = enc.data();
        eb[1].cbBuffer = (unsigned)reqn;
        eb[1].BufferType = SECBUFFER_DATA;
        eb[1].pvBuffer = enc.data() + sizes.cbHeader;
        eb[2].cbBuffer = sizes.cbTrailer;
        eb[2].BufferType = SECBUFFER_STREAM_TRAILER;
        eb[2].pvBuffer = enc.data() + sizes.cbHeader + reqn;
        eb[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc ed{SECBUFFER_VERSION, 4, eb};
        if (EncryptMessage(&ctx, 0, &ed, 0) != SEC_E_OK ||
            !send_all(s, enc.data(), (int)(eb[0].cbBuffer + eb[1].cbBuffer + eb[2].cbBuffer))) {
            ok = false;
        }
    }

    std::string raw;
    std::vector<char> pending = io;
    while (ok && raw.size() < 65536 && !published().stop) {
        if (pending.empty()) {
            char tmp[8192];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            pending.assign(tmp, tmp + n);
        }
        SecBuffer db[4]{};
        db[0].cbBuffer = (unsigned)pending.size();
        db[0].BufferType = SECBUFFER_DATA;
        db[0].pvBuffer = pending.data();
        db[1].BufferType = SECBUFFER_EMPTY;
        db[2].BufferType = SECBUFFER_EMPTY;
        db[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc dd{SECBUFFER_VERSION, 4, db};
        SECURITY_STATUS ds = DecryptMessage(&ctx, &dd, 0, nullptr);
        if (ds == SEC_E_INCOMPLETE_MESSAGE) {
            char tmp[8192];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            pending.insert(pending.end(), tmp, tmp + n);
            continue;
        }
        if (ds == SEC_I_CONTEXT_EXPIRED) break;
        if (ds != SEC_E_OK) break;
        for (int i = 1; i < 4; i++) {
            if (db[i].BufferType == SECBUFFER_DATA && db[i].pvBuffer && db[i].cbBuffer)
                raw.append(static_cast<char*>(db[i].pvBuffer), db[i].cbBuffer);
        }
        pending.clear();
        for (int i = 1; i < 4; i++) {
            if (db[i].BufferType == SECBUFFER_EXTRA && db[i].pvBuffer && db[i].cbBuffer)
                pending.assign(static_cast<char*>(db[i].pvBuffer),
                               static_cast<char*>(db[i].pvBuffer) + db[i].cbBuffer);
        }
    }
    if (have_ctx) DeleteSecurityContext(&ctx);
    FreeCredentialsHandle(&ch);
    disarm_socket(s);
    closesocket(s);

    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    std::string headers = raw.substr(0, hdr_end);
    std::string payload = raw.substr(hdr_end + 4);
    for (char& c : headers) c = (char)tolower((unsigned char)c);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t i = 0;
        while (i < payload.size()) {
            char* end = nullptr;
            long sz = strtol(payload.c_str() + i, &end, 16);
            if (end == payload.c_str() + i || sz < 0) break;
            const char* nl = strstr(end, "\r\n");
            if (!nl) break;
            if (sz == 0) break;
            i = (size_t)(nl - payload.c_str()) + 2;
            if (i + (size_t)sz > payload.size()) break;
            decoded.append(payload, i, (size_t)sz);
            i += (size_t)sz + 2;
        }
        payload.swap(decoded);
    }
    if (payload.find('{') == std::string::npos) return false;
    *body = std::move(payload);
    return true;
}

static bool systemtime_unix(const SYSTEMTIME& st, int64_t* unix_sec)
{
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const ULONGLONG epoch = 116444736000000000ULL;
    if (u.QuadPart < epoch) return false;
    *unix_sec = (int64_t)((u.QuadPart - epoch) / 10000000ULL);
    return true;
}

static bool winhttp_get(const wchar_t* host, const wchar_t* path, const wchar_t* extra,
                        std::string* body, SYSTEMTIME* date_utc = nullptr)
{
    HINTERNET ses = WinHttpOpen(L"Esp32Monitor", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 4000, 4000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, host, 443, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    bool ok = req &&
              WinHttpSendRequest(req, extra ? extra : WINHTTP_NO_ADDITIONAL_HEADERS,
                                 extra ? (DWORD)-1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok && date_utc) {
        DWORD size = sizeof(*date_utc);
        memset(date_utc, 0, sizeof(*date_utc));
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                                 WINHTTP_HEADER_NAME_BY_INDEX, date_utc, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            memset(date_utc, 0, sizeof(*date_utc));
        }
    }
    if (ok) {
        DWORD avail = 0;
        do {
            if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
            body->append(chunk.data(), read);
            if (body->size() > 65536) break;
        } while (avail && !published().stop);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok && body->find('{') != std::string::npos;
}

static bool json_number(const std::string& json, const char* key, double* out)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = 0;
    while ((p = json.find(pat, p)) != std::string::npos) {
        size_t vpos = p + pat.size();
        while (vpos < json.size() && (json[vpos] == ' ' || json[vpos] == '\n' || json[vpos] == '\r')) vpos++;
        char* end = nullptr;
        double v = strtod(json.c_str() + vpos, &end);
        if (end != json.c_str() + vpos) {
            *out = v;
            return true;
        }
        p = vpos + 1;
    }
    return false;
}

static bool json_string(const std::string& json, const char* key, char* out, size_t cap)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = json.find(pat);
    if (p == std::string::npos || cap == 0) return false;
    p += pat.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size() || json[p] != '"') return false;
    p++;
    size_t n = 0;
    while (p < json.size() && json[p] != '"' && n + 1 < cap) {
        unsigned char c = (unsigned char)json[p++];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '.') {
            out[n++] = (char)c;
        }
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n > 0;
}

static bool doh_a(const wchar_t* host, const wchar_t* path, IN_ADDR* ip)
{
    std::string body;
    if (!winhttp_get(host, path, L"Accept: application/dns-json\r\n", &body)) return false;
    size_t p = 0;
    while ((p = body.find("\"data\":\"", p)) != std::string::npos) {
        p += 8;
        char tmp[64]{};
        size_t n = 0;
        while (p < body.size() && body[p] != '"' && n + 1 < sizeof(tmp)) tmp[n++] = body[p++];
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(tmp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256 &&
            !(a == 198 && (b == 18 || b == 19)) && a != 0 && a != 127) {
            char literal[32];
            snprintf(literal, sizeof(literal), "%u.%u.%u.%u", a, b, c, d);
            return inet_pton(AF_INET, literal, ip) == 1;
        }
    }
    return false;
}

static bool resolve_ipwho(IN_ADDR* ip)
{
    if (doh_a(L"dns.alidns.com", L"/resolve?name=ipwho.is&type=A", ip)) return true;
    return doh_a(L"cloudflare-dns.com", L"/dns-query?name=ipwho.is&type=A", ip);
}

struct Fix {
    bool ok = false;
    double lat = 0;
    double lon = 0;
    double acc = 1e12;
    char city[24]{};
};

static bool system_fix(Fix* fix)
{
    try {
        winrt::Windows::Devices::Geolocation::Geolocator geo;
        geo.DesiredAccuracy(winrt::Windows::Devices::Geolocation::PositionAccuracy::High);
        winrt::Windows::Foundation::TimeSpan age{60LL * 10000000LL};
        winrt::Windows::Foundation::TimeSpan timeout{8LL * 10000000LL};
        auto pos = geo.GetGeopositionAsync(age, timeout).get();
        auto coord = pos.Coordinate();
        fix->lat = coord.Latitude();
        fix->lon = coord.Longitude();
        fix->acc = coord.Accuracy();
        fix->ok = fix->lat >= -90 && fix->lat <= 90 && fix->lon >= -180 && fix->lon <= 180;
        return fix->ok;
    } catch (...) {
        return false;
    }
}

static bool ip_fix(const Nic& nic, Fix* fix)
{
    if (!nic.ok) return false;
    IN_ADDR ip{};
    if (!resolve_ipwho(&ip)) return false;
    std::string body;
    if (!https_via_nic(nic, ip, "ipwho.is", L"ipwho.is", "/", &body)) return false;
    if (body.find("\"success\":true") == std::string::npos &&
        body.find("\"success\": true") == std::string::npos) {
        return false;
    }
    if (!json_number(body, "latitude", &fix->lat) || !json_number(body, "longitude", &fix->lon))
        return false;
    json_string(body, "city", fix->city, sizeof(fix->city));
    fix->acc = 50000;
    fix->ok = fix->lat >= -90 && fix->lat <= 90 && fix->lon >= -180 && fix->lon <= 180;
    return fix->ok;
}

static double distance_km(double lat1, double lon1, double lat2, double lon2)
{
    auto rad = [](double d) { return d * 3.141592653589793 / 180.0; };
    double dlat = rad(lat2 - lat1);
    double dlon = rad(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(rad(lat1)) * cos(rad(lat2)) * sin(dlon / 2) * sin(dlon / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static bool reverse_city(double lat, double lon, char* city, size_t cap)
{
    wchar_t path[160];
    swprintf_s(path, L"/v1/reverse?latitude=%.4f&longitude=%.4f&language=en&count=1", lat, lon);
    std::string body;
    if (!winhttp_get(L"geocoding-api.open-meteo.com", path, nullptr, &body)) return false;
    return json_string(body, "name", city, cap);
}

/* Open-Meteo daily time is local to the coordinates: 2026-10-10T06:12. */
static int sun_minutes(const std::string& json, const char* key)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = json.find(pat);
    if (p == std::string::npos) return -1;
    p += pat.size();
    /* The value is an array of one quoted local timestamp, or null. */
    size_t quote = json.find('"', p);
    if (quote == std::string::npos || quote > p + 16) return -1;
    size_t close = json.find('"', quote + 1);
    if (close == std::string::npos) return -1;
    std::string stamp = json.substr(quote + 1, close - quote - 1);
    size_t t = stamp.find('T');
    if (t == std::string::npos) return -1;
    int hour = -1, min = -1;
    if (sscanf(stamp.c_str() + t + 1, "%d:%d", &hour, &min) != 2) return -1;
    if (hour < 0 || hour > 23 || min < 0 || min > 59) return -1;
    return hour * 60 + min;
}

static bool forecast(double lat, double lon, WeatherSnap* snap)
{
    wchar_t path[320];
    swprintf_s(path,
               L"/v1/forecast?latitude=%.4f&longitude=%.4f"
               L"&current=temperature_2m,relative_humidity_2m,weather_code"
               L"&daily=sunrise,sunset&timezone=auto&forecast_days=1",
               lat, lon);
    std::string body;
    SYSTEMTIME date_utc{};
    if (!winhttp_get(L"api.open-meteo.com", path, nullptr, &body, &date_utc)) return false;
    double temp = 0, hum = 0, code = 0;
    if (!json_number(body, "temperature_2m", &temp)) return false;
    json_number(body, "relative_humidity_2m", &hum);
    json_number(body, "weather_code", &code);
    snap->temp = (int)(temp + (temp < 0 ? -0.5 : 0.5));
    snap->hum = (hum >= 0 && hum <= 100) ? (int)(hum + 0.5) : -1;
    snap->code = (code >= 0 && code < 200) ? (int)(code + 0.5) : -1;
    snap->rise_min = sun_minutes(body, "sunrise");
    snap->set_min = sun_minutes(body, "sunset");
    if (snap->rise_min < 0 || snap->set_min <= snap->rise_min) {
        snap->rise_min = -1;
        snap->set_min = -1;
    }
    /* Date is the server's UTC clock. utc_offset_seconds is local civil
     * time at these coordinates, including the current DST rule. */
    snap->time_ok = 0;
    double off = 0;
    int64_t utc = 0;
    if (date_utc.wYear >= 2020 &&
        json_number(body, "utc_offset_seconds", &off) &&
        systemtime_unix(date_utc, &utc)) {
        int offset = (int)(off >= 0 ? off + 0.5 : off - 0.5);
        if (offset >= -18 * 3600 && offset <= 18 * 3600) {
            snap->offset_sec = offset;
            snap->utc_unix = utc;
            snap->time_tick = GetTickCount64();
            snap->time_ok = 1;
        }
    }
    return snap->temp > -80 && snap->temp < 60;
}

static bool weather_fetch(WeatherSnap* snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->temp = -999;
    snap->code = -1;
    snap->hum = -1;
    snap->rise_min = -1;
    snap->set_min = -1;

    Fix sys;
    Fix ip;
    system_fix(&sys);
    Nic nic = physical_nic();
    ip_fix(nic, &ip);
    if (published().stop) return false;

    double lat = 0, lon = 0;
    bool have = false;
    bool sys_precise = sys.ok && sys.acc > 0 && sys.acc <= 20000;
    if (sys_precise) {
        lat = sys.lat;
        lon = sys.lon;
        have = true;
        if (ip.ok && ip.city[0] && distance_km(sys.lat, sys.lon, ip.lat, ip.lon) <= 80)
            strncpy_s(snap->city, ip.city, _TRUNCATE);
    } else if (ip.ok) {
        lat = ip.lat;
        lon = ip.lon;
        have = true;
        strncpy_s(snap->city, ip.city, _TRUNCATE);
    } else if (sys.ok) {
        lat = sys.lat;
        lon = sys.lon;
        have = true;
    }
    if (!have) return false;
    if (!snap->city[0]) reverse_city(lat, lon, snap->city, sizeof(snap->city));
    return forecast(lat, lon, snap);
}

static void publish(const WeatherSnap& snap)
{
    EnterCriticalSection(&published().cs);
    published().snap = snap;
    LeaveCriticalSection(&published().cs);
}

static DWORD WINAPI weather_thread(LPVOID)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    bool apartment = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
    } catch (...) {
    }
    while (!published().stop) {
        WeatherSnap snap{};
        bool ok = false;
        try {
            ok = weather_fetch(&snap);
        } catch (...) {
            ok = false;
        }
        if (ok) publish(snap);
        int seconds = ok ? 15 * 60 : 60;
        for (int i = 0; i < seconds && !published().stop; i++) Sleep(1000);
    }
    if (apartment) {
        try { winrt::uninit_apartment(); } catch (...) {}
    }
    WSACleanup();
    return 0;
}

}  // namespace

#ifdef WEATHER_PROBE
int main()
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    WeatherSnap snap{};
    bool ok = weather_fetch(&snap);
    printf("ok=%d temp=%d code=%d hum=%d city=%s\n", ok ? 1 : 0, snap.temp, snap.code, snap.hum, snap.city);
    WSACleanup();
    return ok ? 0 : 1;
}
#endif

void weather_start()
{
    auto& state = published();
    if (!state.ready) {
        InitializeCriticalSection(&state.cs);
        state.snap.temp = -999;
        state.snap.code = -1;
        state.snap.hum = -1;
        state.snap.rise_min = -1;
        state.snap.set_min = -1;
        state.snap.time_ok = 0;
        state.ready = true;
    }
    if (state.thread) return;
    state.stop = 0;
    state.thread = CreateThread(nullptr, 0, weather_thread, nullptr, 0, nullptr);
}

void weather_stop()
{
    auto& state = published();
    state.stop = 1;
    uintptr_t sock = (uintptr_t)InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&state.sock), nullptr);
    if (sock) closesocket((SOCKET)sock);
    if (state.thread) {
        WaitForSingleObject(state.thread, 8000);
        CloseHandle(state.thread);
        state.thread = nullptr;
    }
}

bool weather_network_time(const WeatherSnap* snap,
                          int* year, int* month, int* day,
                          int* hour, int* minute, int* second)
{
    if (!snap || !snap->time_ok || !year || !month || !day || !hour || !minute || !second)
        return false;
    uint64_t elapsed = (GetTickCount64() - snap->time_tick) / 1000;
    int64_t civil = snap->utc_unix + (int64_t)snap->offset_sec + (int64_t)elapsed;
    if (civil < 0) return false;
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)civil * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return false;
    *year = st.wYear;
    *month = st.wMonth;
    *day = st.wDay;
    *hour = st.wHour;
    *minute = st.wMinute;
    *second = st.wSecond;
    return true;
}

void weather_copy(WeatherSnap* out)
{
    auto& state = published();
    if (!state.ready) {
        memset(out, 0, sizeof(*out));
        out->temp = -999;
        out->code = -1;
        out->hum = -1;
        out->rise_min = -1;
        out->set_min = -1;
        out->time_ok = 0;
        return;
    }
    EnterCriticalSection(&state.cs);
    *out = state.snap;
    LeaveCriticalSection(&state.cs);
}
