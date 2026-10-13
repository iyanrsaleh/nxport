// nxport - list listening TCP ports with the owning process and its working directory.
//
// Windows : GetExtendedTcpTable + process PEB (RTL_USER_PROCESS_PARAMETERS.CurrentDirectory)
// Linux   : /proc/net/tcp[6] + /proc/<pid>/fd (inode -> pid) + /proc/<pid>/cwd
//
// Equivalent of:
//   Get-NetTCPConnection -State Listen |
//     Select-Object LocalAddress, LocalPort, State, OwningProcess,
//       @{Name="Process";Expression={(Get-Process -Id $_.OwningProcess).ProcessName}}
//
// ...plus a "Dir" column: the current working directory of the owning process.

#define NXPORT_VERSION "1.0.0"

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <set>
#include <chrono>
#include <thread>
#include <ctime>

// ===========================================================================
// Common types
// ===========================================================================
struct Row {
    std::string proto = "TCP";
    std::string addr;
    unsigned port = 0;
    std::string state;
    unsigned long pid = 0;
};

struct ProcInfo {
    std::string name;
    std::string dir;
};

static void Out(const std::string& s) {
    if (!s.empty()) fwrite(s.data(), 1, s.size(), stdout);
}

static std::string Pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

static std::string EscapeJson(const std::string& s) {
    std::string res;
    for (char c : s) {
        if (c == '\\') res += "\\\\";
        else if (c == '"') res += "\\\"";
        else if (c == '\b') res += "\\b";
        else if (c == '\f') res += "\\f";
        else if (c == '\n') res += "\\n";
        else if (c == '\r') res += "\\r";
        else if (c == '\t') res += "\\t";
        else res += c;
    }
    return res;
}

static std::string EscapeCsv(const std::string& s) {
    std::string res;
    for (char c : s) {
        if (c == '"') res += "\"\"";
        else res += c;
    }
    return res;
}

static bool IsPublicDatabase(unsigned port, const std::string& addr) {    if (addr != "0.0.0.0" && addr != "::") return false;
    return port == 3306 || port == 5432 || port == 6379 || port == 27017 || port == 9200 ||
           port == 11211 || port == 2375 || port == 33060;
}

static std::string DetectTechStack(const std::string& dir) {
    if (dir.empty() || dir == "-" || dir.find("System32") != std::string::npos ||
        dir.find("system32") != std::string::npos || dir == "/etc/" || dir == "/") {
        return "";
    }

    std::string cleanDir = dir;
    if (cleanDir.back() != '\\' && cleanDir.back() != '/') {
#ifdef _WIN32
        cleanDir += '\\';
#else
        cleanDir += '/';
#endif
    }

    // 1. Check Node.js / package.json
    std::string pkgPath = cleanDir + "package.json";
    FILE* f = fopen(pkgPath.c_str(), "rb");
    if (f) {
        char buf[4096] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n > 0) {
            std::string content(buf, n);
            if (content.find("\"next\"") != std::string::npos) return "Next.js";
            if (content.find("\"nuxt\"") != std::string::npos) return "Nuxt";
            if (content.find("\"vite\"") != std::string::npos) return "Vite";
            if (content.find("\"express\"") != std::string::npos) return "Express";
            if (content.find("\"@nestjs/core\"") != std::string::npos || content.find("\"nest\"") != std::string::npos) return "NestJS";
            if (content.find("\"fastify\"") != std::string::npos) return "Fastify";
            if (content.find("\"astro\"") != std::string::npos) return "Astro";
            if (content.find("\"@remix-run/\"") != std::string::npos) return "Remix";
            if (content.find("\"@sveltejs/\"") != std::string::npos) return "SvelteKit";

            auto pos = content.find("\"name\"");
            if (pos != std::string::npos) {
                auto colon = content.find(':', pos);
                if (colon != std::string::npos) {
                    auto q1 = content.find('"', colon);
                    if (q1 != std::string::npos) {
                        auto q2 = content.find('"', q1 + 1);
                        if (q2 != std::string::npos && q2 > q1 + 1 && q2 - q1 < 32) {
                            return content.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }
            return "Node";
        }
    }

    // 2. Check PHP / Laravel / WordPress
    if (FILE* f2 = fopen((cleanDir + "artisan").c_str(), "rb")) { fclose(f2); return "Laravel"; }
    if (FILE* f2 = fopen((cleanDir + "wp-config.php").c_str(), "rb")) { fclose(f2); return "WordPress"; }

    // 3. Check Python
    if (FILE* f3 = fopen((cleanDir + "manage.py").c_str(), "rb")) { fclose(f3); return "Django"; }
    std::string reqPath = cleanDir + "requirements.txt";
    if (FILE* f3 = fopen(reqPath.c_str(), "rb")) {
        char buf[2048] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f3);
        fclose(f3);
        if (n > 0) {
            std::string content(buf, n);
            if (content.find("fastapi") != std::string::npos) return "FastAPI";
            if (content.find("flask") != std::string::npos) return "Flask";
            if (content.find("streamlit") != std::string::npos) return "Streamlit";
        }
    }

    // 4. Check Rust / Go
    if (FILE* f4 = fopen((cleanDir + "Cargo.toml").c_str(), "rb")) { fclose(f4); return "Rust"; }
    if (FILE* f4 = fopen((cleanDir + "go.mod").c_str(), "rb")) { fclose(f4); return "Go"; }

    return "";
}

static void PrintHelp() {
    Out(
        "nxport - modern real-time port inspection & process manager (Windows & Linux)\n"
        "\n"
        "Usage:\n"
        "  nxport                    live real-time dashboard (stays open in terminal)\n"
        "  nxport [port|range]       filter ports in live dashboard\n"
        "  nxport <text>             smart search: filter by process name or Dir\n"
        "  nxport kill <port>        kill the process(es) listening on <port>\n"
        "  nxport wait <port>        block until something listens on <port>\n"
        "  nxport find [start]       print the first free port (default from 3000)\n"
        "  nxport serve [port]       run internal API server (default port 7900)\n"
        "\n"
        "Dashboard Controls:\n"
        "  [↑/↓]                     navigate ports\n"
        "  [Enter / o]               open http://localhost:<port> in default browser\n"
        "  [k]                       kill process on selected port\n"
        "  [p]                       health check selected port (HTTP status / TCP)\n"
        "  [s]                       settings menu (saved folders, server history, UDP, refresh)\n"
        "  [r]                       refresh ports manually\n"
        "  [q / Esc]                 quit dashboard\n"
        "\n"
        "CLI Options:\n"
        "  --once, --static          print table once and exit (for scripts)\n"
        "  kill, -k <port>           kill process(es) on <port> (-f for force)\n"
        "  wait <port>               wait until a port is up (--timeout <sec>, 0 = forever)\n"
        "  find [start]              find free port(s) (--count <n>)\n"
        "  -u, --udp                 include UDP ports\n"
        "  -p, --ping                health check all ports\n"
        "  -a, --all                 show all TCP states (default: Listen only)\n"
        "  --json                    output in structured JSON format\n"
        "  --yaml                    output in YAML format\n"
        "  --csv                     output in CSV format\n"
        "  --color / --no-color      force or disable colored output\n"
        "  -h, --help                show this help\n"
        "  -V, --version             show version\n");
}

// ===========================================================================
// Windows
// ===========================================================================
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <conio.h>
#include <winsvc.h>
#include <winternl.h>
#include <netioapi.h>
#include <io.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static NtQueryInformationProcess_t g_NtQIP = nullptr;
static const ULONG kProcessBasicInformation = 0;
static const ULONG kProcessWow64Information = 26;

static const uintptr_t kPebParamsOffset64 = 0x20;
static const uintptr_t kParamsCurDirOffset64 = 0x38;
static const uintptr_t kPebParamsOffset32 = 0x10;
static const uintptr_t kParamsCurDirOffset32 = 0x24;

static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// PID -> image name for every process, readable without elevation.
// (NtQuerySystemInformation only exposes the base name here, not the full path.)
static std::map<DWORD, std::string> g_pidNames;

static void BuildPidNames() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    typedef NTSTATUS(NTAPI* pfnNtQSI)(ULONG, PVOID, ULONG, PULONG);
    auto Nt = reinterpret_cast<pfnNtQSI>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!Nt) return;

    ULONG size = 1u << 20;
    std::vector<BYTE> buf(size);
    NTSTATUS st;
    while ((st = Nt(5 /*SystemProcessInformation*/, buf.data(), size, nullptr)) ==
           (NTSTATUS)0xC0000004L /*STATUS_INFO_LENGTH_MISMATCH*/) {
        size *= 2;
        buf.resize(size);
    }
    if (st < 0) return;

    const BYTE* p = buf.data();
    const BYTE* end = buf.data() + size;
    for (;;) {
        if (p + 0x58 > end) break;
        ULONG next = *reinterpret_cast<const ULONG*>(p);
        auto name = reinterpret_cast<const UNICODE_STRING*>(p + 0x38);
        HANDLE pid = *reinterpret_cast<const HANDLE*>(p + 0x50);
        if (name->Buffer && name->Length > 0 && name->Length < 512) {
            g_pidNames[static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pid))] =
                W2U8(std::wstring(name->Buffer, name->Length / sizeof(wchar_t)));
        }
        if (!next) break;
        p += next;
    }
}

static std::wstring ExtractDirectoryFromPath(std::wstring p) {
    if (p.empty()) return {};
    while (!p.empty() && (p.front() == L' ' || p.front() == L'\t')) p.erase(0, 1);
    if (!p.empty() && p.front() == L'"') {
        p.erase(0, 1);
        auto q = p.find(L'"');
        if (q != std::wstring::npos) p = p.substr(0, q);
    } else {
        for (size_t i = 0; i + 4 <= p.size(); ++i) {
            if ((p[i] == L'.' || p[i] == L'.') &&
                (p[i + 1] == L'e' || p[i + 1] == L'E') &&
                (p[i + 2] == L'x' || p[i + 2] == L'X') &&
                (p[i + 3] == L'e' || p[i + 3] == L'E')) {
                p = p.substr(0, i + 4);
                break;
            }
        }
    }
    if (p.find(L'%') != std::wstring::npos) {
        wchar_t expanded[MAX_PATH * 2];
        DWORD expLen = ExpandEnvironmentStringsW(p.c_str(), expanded, MAX_PATH * 2);
        if (expLen > 0 && expLen < MAX_PATH * 2) {
            p.assign(expanded, expLen - 1);
        }
    }
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        return p.substr(0, slash + 1);
    }
    return {};
}

static std::map<DWORD, std::string> g_servicePidToDir;

static void BuildServiceMap() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    DWORD bytesNeeded = 0, servicesReturned = 0, resumeHandle = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);
    if (bytesNeeded == 0) { CloseServiceHandle(scm); return; }
    std::vector<BYTE> buf(bytesNeeded);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                               buf.data(), bytesNeeded, &bytesNeeded, &servicesReturned,
                               &resumeHandle, nullptr)) {
        CloseServiceHandle(scm);
        return;
    }

    auto* services = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
    for (DWORD i = 0; i < servicesReturned; ++i) {
        DWORD pid = services[i].ServiceStatusProcess.dwProcessId;
        if (pid == 0 || g_servicePidToDir.find(pid) != g_servicePidToDir.end()) continue;

        SC_HANDLE svc = OpenServiceW(scm, services[i].lpServiceName, SERVICE_QUERY_CONFIG);
        if (!svc) continue;
        DWORD cb = 0;
        QueryServiceConfigW(svc, nullptr, 0, &cb);
        if (cb > 0) {
            std::vector<BYTE> cbuf(cb);
            auto* cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cbuf.data());
            if (QueryServiceConfigW(svc, cfg, cb, &cb) && cfg->lpBinaryPathName) {
                std::wstring dir = ExtractDirectoryFromPath(cfg->lpBinaryPathName);
                if (!dir.empty()) {
                    g_servicePidToDir[pid] = W2U8(dir);
                }
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

template <class T>
static bool ReadMem(HANDLE h, uintptr_t addr, T& out) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &got) &&
           got == sizeof(T);
}

static std::wstring ReadRemoteUnicodeString(HANDLE h, uintptr_t addr, bool target64) {
    USHORT len = 0;
    if (!ReadMem(h, addr, len)) return {};
    if (len == 0 || len > 0x8000) return {};
    uintptr_t buffer = 0;
    if (target64) {
        if (!ReadMem(h, addr + 8, buffer)) return {};
    } else {
        uint32_t buf32 = 0;
        if (!ReadMem(h, addr + 4, buf32)) return {};
        buffer = buf32;
    }
    if (!buffer) return {};
    std::wstring s(len / sizeof(wchar_t), L'\0');
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(buffer), s.data(), len, &got) || got != len)
        return {};
    return s;
}

static std::wstring GetWorkingDirectory(HANDLE h) {
    if (!g_NtQIP) return {};
    BOOL isWow64 = FALSE;
    bool target64 = true;
    if (IsWow64Process(h, &isWow64) && isWow64) target64 = false;

    if (target64) {
        PROCESS_BASIC_INFORMATION pbi{};
        if (g_NtQIP(h, kProcessBasicInformation, &pbi, sizeof(pbi), nullptr) < 0) return {};
        uintptr_t pp = 0;
        if (!ReadMem(h, reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + kPebParamsOffset64, pp))
            return {};
        return ReadRemoteUnicodeString(h, pp + kParamsCurDirOffset64, true);
    }
    // WOW64 target: reach the 32-bit PEB / process parameters.
    uintptr_t peb32 = 0;
    if (g_NtQIP(h, kProcessWow64Information, &peb32, sizeof(peb32), nullptr) < 0) return {};
    uint32_t pp32 = 0;
    if (!ReadMem(h, peb32 + kPebParamsOffset32, pp32)) return {};
    return ReadRemoteUnicodeString(h, static_cast<uintptr_t>(pp32) + kParamsCurDirOffset32, false);
}

static ProcInfo GetProcInfo(unsigned long pid) {
    ProcInfo info;
    if (pid == 0) return info;

    // Special case for System kernel process
    if (pid == 4) {
        info.name = "System";
        wchar_t sysDir[MAX_PATH];
        if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
            info.dir = W2U8(std::wstring(sysDir) + L"\\");
        }
        return info;
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    bool canReadMemory = (h != nullptr);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);

    std::wstring exeFullPath;
    if (h) {
        wchar_t path[MAX_PATH * 8];
        DWORD n = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            exeFullPath.assign(path, n);
            auto slash = exeFullPath.find_last_of(L"\\/");
            info.name = W2U8(slash == std::wstring::npos ? exeFullPath : exeFullPath.substr(slash + 1));
        }
        if (canReadMemory) {
            info.dir = W2U8(GetWorkingDirectory(h));
        }
        CloseHandle(h);
    }

    // 1. Fallback for process name
    if (info.name.empty()) {
        auto it = g_pidNames.find((DWORD)pid);
        if (it != g_pidNames.end()) {
            info.name = it->second;
        }
    }

    // 2. Fallbacks for directory
    if (info.dir.empty()) {
        // 2a. Directory of the executable itself from QueryFullProcessImageName
        if (!exeFullPath.empty()) {
            info.dir = W2U8(ExtractDirectoryFromPath(exeFullPath));
        }
    }

    if (info.dir.empty()) {
        // 2b. Windows Service binary location (e.g. nvcontainer, svchost, etc.)
        auto it = g_servicePidToDir.find((DWORD)pid);
        if (it != g_servicePidToDir.end()) {
            info.dir = it->second;
        }
    }

    if (info.dir.empty() && !info.name.empty()) {
        // 2c. System32 or PATH lookup for standard system binaries (svchost.exe, lsass.exe, wininit.exe, etc.)
        std::wstring wname(info.name.begin(), info.name.end());
        wchar_t sysDir[MAX_PATH];
        if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
            std::wstring testPath = std::wstring(sysDir) + L"\\" + wname;
            if (GetFileAttributesW(testPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                info.dir = W2U8(std::wstring(sysDir) + L"\\");
            }
        }
        if (info.dir.empty()) {
            wchar_t found[MAX_PATH];
            DWORD len = SearchPathW(nullptr, wname.c_str(), L".exe", MAX_PATH, found, nullptr);
            if (len > 0 && len < MAX_PATH) {
                info.dir = W2U8(ExtractDirectoryFromPath(std::wstring(found, len)));
            }
        }
    }

    if (info.name == "wslhost.exe" || info.name == "wsl.exe") {
        info.name += " [WSL]";
    } else if (info.name.find("docker") != std::string::npos ||
               exeFullPath.find(L"Docker") != std::wstring::npos) {
        info.name += " [Docker]";
    }

    return info;
}

static const char* StateName(DWORD s) {
    switch (s) {
        case MIB_TCP_STATE_CLOSED:     return "Closed";
        case MIB_TCP_STATE_LISTEN:     return "Listen";
        case MIB_TCP_STATE_SYN_SENT:   return "SynSent";
        case MIB_TCP_STATE_SYN_RCVD:   return "SynReceived";
        case MIB_TCP_STATE_ESTAB:      return "Established";
        case MIB_TCP_STATE_FIN_WAIT1:  return "FinWait1";
        case MIB_TCP_STATE_FIN_WAIT2:  return "FinWait2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CloseWait";
        case MIB_TCP_STATE_CLOSING:    return "Closing";
        case MIB_TCP_STATE_LAST_ACK:   return "LastAck";
        case MIB_TCP_STATE_TIME_WAIT:  return "TimeWait";
        case MIB_TCP_STATE_DELETE_TCB: return "DeleteTcb";
        default:                       return "Unknown";
    }
}

static void CollectUdp(std::vector<Row>& rows) {
    {
        DWORD size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) ==
            ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            auto* t = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buf.data());
            if (GetExtendedUdpTable(t, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    const auto& e = t->table[i];
                    Row r;
                    r.proto = "UDP";
                    IN_ADDR a;
                    a.S_un.S_addr = e.dwLocalAddr;
                    char text[64] = {0};
                    InetNtopA(AF_INET, &a, text, 64);
                    r.addr = text;
                    r.port = ntohs(static_cast<u_short>(e.dwLocalPort));
                    r.state = "UDP";
                    r.pid = e.dwOwningPid;
                    rows.push_back(std::move(r));
                }
            }
        }
    }
    {
        DWORD size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) ==
            ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            auto* t = reinterpret_cast<PMIB_UDP6TABLE_OWNER_PID>(buf.data());
            if (GetExtendedUdpTable(t, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    const auto& e = t->table[i];
                    Row r;
                    r.proto = "UDP";
                    char text[64] = {0};
                    InetNtopA(AF_INET6, e.ucLocalAddr, text, 64);
                    r.addr = text;
                    if (r.addr == "::") r.addr = "0.0.0.0";
                    r.port = ntohs(static_cast<u_short>(e.dwLocalPort));
                    r.state = "UDP";
                    r.pid = e.dwOwningPid;
                    rows.push_back(std::move(r));
                }
            }
        }
    }
}

static void CollectRows(std::vector<Row>& rows, bool all, bool includeUdp) {
    {
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) ==
            ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            auto* t = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buf.data());
            if (GetExtendedTcpTable(t, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) ==
                NO_ERROR) {
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    const auto& e = t->table[i];
                    if (!all && e.dwState != MIB_TCP_STATE_LISTEN) continue;
                    Row r;
                    r.proto = "TCP";
                    IN_ADDR a;
                    a.S_un.S_addr = e.dwLocalAddr;
                    char text[64] = {0};
                    InetNtopA(AF_INET, &a, text, 64);
                    r.addr = text;
                    r.port = ntohs(static_cast<u_short>(e.dwLocalPort));
                    r.state = StateName(e.dwState);
                    r.pid = e.dwOwningPid;
                    rows.push_back(std::move(r));
                }
            }
        }
    }
    {
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) ==
            ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            auto* t = reinterpret_cast<PMIB_TCP6TABLE_OWNER_PID>(buf.data());
            if (GetExtendedTcpTable(t, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) ==
                NO_ERROR) {
                for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                    const auto& e = t->table[i];
                    if (!all && e.dwState != MIB_TCP_STATE_LISTEN) continue;
                    Row r;
                    r.proto = "TCP";
                    char text[64] = {0};
                    InetNtopA(AF_INET6, e.ucLocalAddr, text, 64);
                    r.addr = text;
                    if (r.addr == "::") r.addr = "0.0.0.0";
                    r.port = ntohs(static_cast<u_short>(e.dwLocalPort));
                    r.state = StateName(e.dwState);
                    r.pid = e.dwOwningPid;
                    rows.push_back(std::move(r));
                }
            }
        }
    }
    if (includeUdp) {
        CollectUdp(rows);
    }
}

static bool TerminateProc(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != FALSE;
}

static void PlatformInit() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        g_NtQIP = reinterpret_cast<NtQueryInformationProcess_t>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }
    BuildPidNames();
    BuildServiceMap();
}

static void PlatformShutdown() { WSACleanup(); }

static void GetTerminalSize(int& rows, int& cols) {
    rows = 30;
    cols = 100;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

// ===========================================================================
// Linux
// ===========================================================================
#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netdb.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <ifaddrs.h>

static std::string ReadLink(const std::string& path) {
    char buf[4096];
    ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return {};
    return std::string(buf, static_cast<size_t>(n));
}

static const char* StateName(unsigned s) {
    switch (s) {
        case 0x01: return "Established";
        case 0x02: return "SynSent";
        case 0x03: return "SynReceived";
        case 0x04: return "FinWait1";
        case 0x05: return "FinWait2";
        case 0x06: return "TimeWait";
        case 0x07: return "Closed";
        case 0x08: return "CloseWait";
        case 0x09: return "LastAck";
        case 0x0A: return "Listen";
        case 0x0B: return "Closing";
        case 0x0C: return "NewSynReceived";
        default:   return "Unknown";
    }
}

static ProcInfo GetProcInfo(unsigned long pid) {
    ProcInfo info;
    if (pid == 0) return info;
    std::string base = "/proc/" + std::to_string(pid);
    if (FILE* f = fopen((base + "/comm").c_str(), "r")) {
        char buf[512];
        if (fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            info.name = s;
        }
        fclose(f);
    }
    info.dir = ReadLink(base + "/cwd");
    if (info.dir.empty()) {
        std::string exe = ReadLink(base + "/exe");
        if (!exe.empty()) {
            auto slash = exe.find_last_of('/');
            if (slash != std::string::npos) {
                info.dir = exe.substr(0, slash + 1);
            }
        }
    }
    if (info.dir.empty() && !info.name.empty()) {
        const char* dirs[] = {"/usr/sbin/", "/usr/bin/", "/sbin/", "/bin/"};
        for (const char* d : dirs) {
            std::string test = std::string(d) + info.name;
            if (access(test.c_str(), X_OK) == 0) {
                info.dir = d;
                break;
            }
        }
    }
    return info;
}

// inode -> pid by scanning every process' open fds.
static void BuildInodeMap(std::map<unsigned long long, unsigned long>& map) {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    while (dirent* de = readdir(proc)) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        unsigned long pid = strtoul(de->d_name, nullptr, 10);
        std::string fddir = std::string("/proc/") + de->d_name + "/fd";
        DIR* fds = opendir(fddir.c_str());
        if (!fds) continue;
        while (dirent* fe = readdir(fds)) {
            if (fe->d_name[0] == '.') continue;
            std::string link = ReadLink(fddir + "/" + fe->d_name);
            if (link.rfind("socket:[", 0) == 0) {
                unsigned long long ino = strtoull(link.c_str() + 8, nullptr, 10);
                map.emplace(ino, pid);
            }
        }
        closedir(fds);
    }
    closedir(proc);
}

static void CollectRows(std::vector<Row>& rows, bool all, bool includeUdp) {
    std::map<unsigned long long, unsigned long> inodeToPid;
    BuildInodeMap(inodeToPid);

    std::vector<std::string> files;
    files.push_back("/proc/net/tcp");
    files.push_back("/proc/net/tcp6");
    if (includeUdp) {
        files.push_back("/proc/net/udp");
        files.push_back("/proc/net/udp6");
    }

    for (size_t k = 0; k < files.size(); ++k) {
        bool v6 = (files[k].find('6') != std::string::npos);
        bool isUdp = (files[k].find("udp") != std::string::npos);
        FILE* f = fopen(files[k].c_str(), "r");
        if (!f) continue;
        char line[512];
        if (!fgets(line, sizeof(line), f)) { fclose(f); continue; }  // header
        while (fgets(line, sizeof(line), f)) {
            char local[128], rem[128], st[16], txrx[64], trtm[64], retr[64];
            unsigned uid = 0, timeout = 0;
            unsigned long long inode = 0;
            if (sscanf(line, "%*s %127s %127s %15s %63s %63s %63s %u %u %llu", local, rem, st,
                       txrx, trtm, retr, &uid, &timeout, &inode) != 9)
                continue;

            unsigned state = static_cast<unsigned>(strtoul(st, nullptr, 16));
            if (!isUdp && !all && state != 0x0A) continue;

            char* colon = strrchr(local, ':');
            if (!colon) continue;
            *colon = '\0';
            unsigned port = static_cast<unsigned>(strtoul(colon + 1, nullptr, 16));

            Row r;
            r.proto = isUdp ? "UDP" : "TCP";
            r.port = port;
            r.state = isUdp ? "UDP" : StateName(state);

            char text[INET6_ADDRSTRLEN] = {0};
            if (!v6) {
                uint32_t v = static_cast<uint32_t>(strtoul(local, nullptr, 16));
                struct in_addr a;
                memcpy(&a, &v, 4);
                inet_ntop(AF_INET, &a, text, sizeof(text));
            } else {
                // /proc prints four little-endian 32-bit words; rebuild as native words.
                uint32_t words[4];
                for (int i = 0; i < 4; ++i) {
                    char tmp[9];
                    memcpy(tmp, local + i * 8, 8);
                    tmp[8] = '\0';
                    words[i] = static_cast<uint32_t>(strtoul(tmp, nullptr, 16));
                }
                struct in6_addr a;
                memcpy(&a, words, 16);
                inet_ntop(AF_INET6, &a, text, sizeof(text));
            }
            r.addr = text;
            if (r.addr == "::") r.addr = "0.0.0.0";

            auto it = inodeToPid.find(inode);
            r.pid = (it != inodeToPid.end()) ? it->second : 0;
            rows.push_back(std::move(r));
        }
        fclose(f);
    }
}

static bool TerminateProc(unsigned long pid) {
    if (kill(static_cast<pid_t>(pid), SIGTERM) == 0) return true;
    return kill(static_cast<pid_t>(pid), SIGKILL) == 0;
}

static void PlatformInit() {}
static void PlatformShutdown() {}


static void GetTerminalSize(int& rows, int& cols) {
    rows = 30;
    cols = 100;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
}

#endif  // _WIN32

// ===========================================================================
// System stats (CPU, RAM, Disk, Network) for the dashboard header.
// ===========================================================================
struct SysStats {
    int cpu = -1;     // percent, -1 = unknown
    int ram = -1;     // percent
    int disk = -1;    // percent
    long long netBps = -1;  // bytes/sec across interfaces, -1 = unknown
};

static unsigned long long g_prevSysTime = 0, g_prevIdleTime = 0;
static bool g_prevCpuValid = false;
static unsigned long long g_prevRx = 0, g_prevTx = 0;
static unsigned long long g_prevStampMs = 0;
static bool g_prevNetValid = false;

#ifdef _WIN32
static void FileTimeToUlong64(const FILETIME& ft, unsigned long long& out) {
    out = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
          static_cast<unsigned long long>(ft.dwLowDateTime);
}
#endif

// Sample CPU/RAM/Disk/Network. Call repeatedly; deltas need a previous sample.
static SysStats SampleSysStats() {
    SysStats st;
#ifdef _WIN32
    // CPU: GetSystemTimes delta
    FILETIME ftIdle, ftKernel, ftUser;
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        unsigned long long idle = 0, kernel = 0, user = 0;
        FileTimeToUlong64(ftIdle, idle);
        FileTimeToUlong64(ftKernel, kernel);
        FileTimeToUlong64(ftUser, user);
        unsigned long long sys = kernel + user;  // kernel includes idle
        if (g_prevCpuValid && sys > g_prevSysTime) {
            unsigned long long dSys = sys - g_prevSysTime;
            unsigned long long dIdle = idle - g_prevIdleTime;
            int pct = static_cast<int>((dSys - dIdle) * 100 / dSys);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            st.cpu = pct;
        }
        g_prevSysTime = sys;
        g_prevIdleTime = idle;
        g_prevCpuValid = true;
    }
    // RAM: GlobalMemoryStatusEx
    MEMORYSTATUSEX mse{};
    mse.dwLength = sizeof(mse);
    if (GlobalMemoryStatusEx(&mse)) {
        st.ram = static_cast<int>(mse.dwMemoryLoad);
    }
    // Disk: GetDiskFreeSpaceExA on the system drive
    ULARGE_INTEGER total{}, freeq{};
    char sysDir[MAX_PATH];
    if (GetSystemDirectoryA(sysDir, MAX_PATH) &&
        GetDiskFreeSpaceExA(std::string(sysDir).substr(0, 3).c_str(), nullptr, &total, &freeq) &&
        total.QuadPart > 0) {
        st.disk = static_cast<int>(100 - freeq.QuadPart * 100 / total.QuadPart);
    }
    // Network: GetIfTable2 delta (bytes across all non-loopback interfaces)
    unsigned long long stamp = static_cast<unsigned long long>(GetTickCount64());
    unsigned long long rx = 0, tx = 0;
    PMIB_IF_TABLE2 tbl = nullptr;
    if (GetIfTable2(&tbl) == NO_ERROR && tbl) {
        for (ULONG i = 0; i < tbl->NumEntries; ++i) {
            const MIB_IF_ROW2& r = tbl->Table[i];
            if (r.OperStatus != IfOperStatusUp) continue;
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            rx += r.InOctets;
            tx += r.OutOctets;
        }
        FreeMibTable(tbl);
    }
    if (g_prevNetValid && stamp > g_prevStampMs) {
        unsigned long long dms = stamp - g_prevStampMs;
        unsigned long long drx = (rx >= g_prevRx) ? rx - g_prevRx : 0;
        unsigned long long dtx = (tx >= g_prevTx) ? tx - g_prevTx : 0;
        st.netBps = static_cast<long long>((drx + dtx) * 1000 / dms);
    }
    g_prevRx = rx;
    g_prevTx = tx;
    g_prevStampMs = stamp;
    g_prevNetValid = true;
#else
    // CPU: /proc/stat delta
    {
        FILE* f = fopen("/proc/stat", "r");
        if (f) {
            unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0,
                               irq = 0, softirq = 0, steal = 0;
            if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
                unsigned long long all = user + nice + sys + idle + iowait + irq + softirq + steal;
                if (g_prevCpuValid && all > g_prevSysTime) {
                    unsigned long long dAll = all - g_prevSysTime;
                    unsigned long long dIdle = idle + iowait - g_prevIdleTime;
                    int pct = static_cast<int>((dAll - dIdle) * 100 / dAll);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    st.cpu = pct;
                }
                g_prevSysTime = all;
                g_prevIdleTime = idle + iowait;
                g_prevCpuValid = true;
            }
            fclose(f);
        }
    }
    // RAM: MemAvailable from /proc/meminfo
    {
        FILE* f = fopen("/proc/meminfo", "r");
        if (f) {
            char key[64];
            long long val = 0;
            unsigned long long total = 0, avail = 0;
            char unit[32];
            while (fscanf(f, "%63s %lld %31s", key, &val, unit) >= 2) {
                if (strcmp(key, "MemTotal:") == 0) total = static_cast<unsigned long long>(val);
                else if (strcmp(key, "MemAvailable:") == 0) avail = static_cast<unsigned long long>(val);
                if (total && avail) break;
            }
            if (total > 0) st.ram = static_cast<int>(100 - avail * 100 / total);
            fclose(f);
        }
    }
    // Disk: statvfs on "/"
    {
        struct statvfs vfs{};
        if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
            st.disk = static_cast<int>(100 - vfs.f_bavail * 100 / vfs.f_blocks);
        }
    }
    // Network: /proc/net/dev delta
    {
        FILE* f = fopen("/proc/net/dev", "r");
        if (f) {
            char line[512];
            unsigned long long rx = 0, tx = 0;
            if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f)) {
                while (fgets(line, sizeof(line), f)) {
                    char ifname[64];
                    unsigned long long rb = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0,
                                       d5 = 0, d6 = 0, d7 = 0, tb = 0;
                    if (sscanf(line, "%63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                               ifname, &rb, &d1, &d2, &d3, &d4, &d5, &d6, &d7, &tb) >= 10) {
                        if (strncmp(ifname, "lo", 2) == 0) continue;
                        rx += rb;
                        tx += tb;
                    }
                }
            }
            fclose(f);
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            unsigned long long stamp = static_cast<unsigned long long>(ts.tv_sec) * 1000 +
                                       static_cast<unsigned long long>(ts.tv_nsec) / 1000000;
            if (g_prevNetValid && stamp > g_prevStampMs) {
                unsigned long long dms = stamp - g_prevStampMs;
                unsigned long long drx = (rx >= g_prevRx) ? rx - g_prevRx : 0;
                unsigned long long dtx = (tx >= g_prevTx) ? tx - g_prevTx : 0;
                st.netBps = static_cast<long long>((drx + dtx) * 1000 / dms);
            }
            g_prevRx = rx;
            g_prevTx = tx;
            g_prevStampMs = stamp;
            g_prevNetValid = true;
        }
    }
#endif
    return st;
}

// Format bytes/sec into a compact human string.
static std::string FormatBps(long long bps) {
    if (bps < 0) return "n/a";
    char buf[32];
    if (bps >= 1000000) {
        snprintf(buf, sizeof(buf), "%.1fMB/s", bps / 1000000.0);
    } else if (bps >= 1000) {
        snprintf(buf, sizeof(buf), "%.1fkB/s", bps / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%lldB/s", bps);
    }
    return buf;
}

// Format a byte count into a human size (GB/MB).
static std::string FormatBytes(unsigned long long b) {
    char buf[32];
    if (b >= 1000000000ULL) snprintf(buf, sizeof(buf), "%.1f GB", b / 1000000000.0);
    else if (b >= 1000000ULL) snprintf(buf, sizeof(buf), "%.1f MB", b / 1000000.0);
    else snprintf(buf, sizeof(buf), "%.1f KB", b / 1000.0);
    return buf;
}

// Extended system info for the System Info screen.
struct SysInfo {
    int cpuCount = 0;
    double cpuPerCore[64] = {0};
    int cpuCoreSamples = 0;
    unsigned long long ramTotal = 0, ramUsed = 0;
    struct Disk { char letter[8]; unsigned long long total, free_; };
    std::vector<Disk> disks;
    std::vector<std::string> ips;
    std::string hostName, osName, cpuBrand, ramType, ramSpeed;
    int ramSlotsUsed = 0;
    unsigned long long smbiosRamMb = 0;
};

#ifdef _WIN32
struct PerCoreCpu {
    unsigned long long sys[64] = {0}, idle[64] = {0};
    bool valid = false;
};
static PerCoreCpu g_prevCore;
#endif

#ifdef _WIN32
// Parse raw SMBIOS (GetSystemFirmwareTable 'RSMB') Memory Device (type 17)
// structures: size (MB), configured speed (MT/s) and DDR generation.
// Type 17 offsets (SMBIOS 2.6+): 0Ch Speed, 12h Size, 15h ConfiguredMemorySpeed.
static void ParseSmbiosMemory(std::string& ramType, std::string& ramSpeed, int& slots,
                              unsigned long long& totalMb) {
    DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size < 12) return;
    std::vector<unsigned char> buf(size);
    if (GetSystemFirmwareTable('RSMB', 0, buf.data(), size) == 0) return;
    // RawSMBIOSData: BYTE Used; BYTE Major; BYTE Minor; BYTE DmiRev; DWORD Length; BYTE Data[].
    const unsigned char* tbl = buf.data() + 8;
    const unsigned char* end = tbl + *reinterpret_cast<const DWORD*>(buf.data() + 4);
    if (end > buf.data() + size) end = buf.data() + size;
    const unsigned char* q = tbl;
    while (q + 4 <= end) {
        unsigned char type = q[0];
        unsigned char len = q[1];
        if (len < 4) break;
        // Strings section starts at q+len; ends with a double NUL.
        const unsigned char* s = q + len;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) s++;
        s += 2;
        if (type == 17 && len >= 0x16) {
            unsigned short rawSize = q[0x12] | (q[0x13] << 8);
            unsigned short speed = q[0x15] | (q[0x16] << 8);  // configured speed
            if (rawSize != 0 && rawSize != 0xFFFF) {
                unsigned long long mb = (rawSize & 0x8000) ? (rawSize & 0x7FFF) * 1024ULL : rawSize;
                totalMb += mb;
                slots++;
                if (speed >= 100 && speed <= 10000 && ramSpeed.empty()) {
                    ramSpeed = std::to_string(speed) + " MT/s";
                }
                // MemoryDevice Type (offset 12h is Size; the DDR generation is best
                // inferred from speed, which is reliable in practice).
                if (ramType.empty()) {
                    if (speed >= 4800) ramType = "DDR5";
                    else if (speed >= 1600) ramType = "DDR4";
                    else if (speed > 0) ramType = "DDR3";
                }
            }
        }
        q = s;
    }
}
#endif  // _WIN32

static SysInfo GetSysInfo() {
    // Hardware identity (brand, RAM type/speed, disks, IPs) is sampled once;
    // per-core CPU is a delta computed on every call below.
    static SysInfo siCached;
    static bool siInit = false;
    if (!siInit) siInit = true;
    else return siCached;
    SysInfo si;
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD nsize = sizeof(name);
    if (GetComputerNameA(name, &nsize)) si.hostName = name;
    si.osName = "Windows";
    {
        std::string ramType, ramSpeed;
        int slots = 0;
        unsigned long long totalMb = 0;
        ParseSmbiosMemory(ramType, ramSpeed, slots, totalMb);
        si.ramType = ramType;
        si.ramSpeed = ramSpeed;
        si.ramSlotsUsed = slots;
    }
    // CPU brand from the registry (friendly name, e.g. "Intel(R) Core(TM) i7...").
    {
        HKEY hk;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ, &hk) == ERROR_SUCCESS) {
            char brand[256] = {0};
            DWORD bsize = sizeof(brand);
            if (RegQueryValueExA(hk, "ProcessorNameString", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(brand), &bsize) == ERROR_SUCCESS) {
                si.cpuBrand = brand;
            }
            RegCloseKey(hk);
        }
        if (si.cpuBrand.empty()) {
            SYSTEM_INFO sinfo{};
            GetSystemInfo(&sinfo);
            si.cpuBrand = (sinfo.dwProcessorType == PROCESSOR_INTEL_IA64) ? "Intel" : "x86 processor";
        }
    }
    ParseSmbiosMemory(si.ramType, si.ramSpeed, si.ramSlotsUsed, si.smbiosRamMb);
    SYSTEM_INFO sinfo{};
    GetSystemInfo(&sinfo);
    si.cpuCount = static_cast<int>(sinfo.dwNumberOfProcessors);
    if (si.cpuCount > 64) si.cpuCount = 64;

    // Per-core CPU via NtQuerySystemInformation (delta against the previous call).
    using NtQSI_t = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    static NtQSI_t ntqsi = reinterpret_cast<NtQSI_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (ntqsi) {
        // SystemProcessorPerformanceInformation = 8
        struct ProcPerfTime { LARGE_INTEGER Idle, Kernel, User; };
        ProcPerfTime times[64] = {};
        ULONG ret = 0;
        if (ntqsi(8, times, sizeof(times), &ret) == 0 && ret > 0) {
            int cores = static_cast<int>(ret / sizeof(ProcPerfTime));
            if (cores > 64) cores = 64;
            static ProcPerfTime prev[64] = {};
            static bool prevValid = false;
            for (int i = 0; i < cores; ++i) {
                unsigned long long idle = static_cast<unsigned long long>(times[i].Idle.QuadPart);
                unsigned long long sys = static_cast<unsigned long long>(times[i].Kernel.QuadPart) +
                                         static_cast<unsigned long long>(times[i].User.QuadPart);
                unsigned long long pSys = static_cast<unsigned long long>(prev[i].Kernel.QuadPart) +
                                          static_cast<unsigned long long>(prev[i].User.QuadPart);
                if (prevValid && sys > pSys) {
                    unsigned long long dSys = sys - pSys;
                    unsigned long long dIdle = idle - static_cast<unsigned long long>(prev[i].Idle.QuadPart);
                    if (dSys > 0) {
                        double pct = (dSys - dIdle) * 100.0 / dSys;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        if (i < 64) si.cpuPerCore[i] = pct;
                        si.cpuCoreSamples = cores;
                    }
                }
                prev[i] = times[i];
            }
            prevValid = true;
        }
    }
    MEMORYSTATUSEX mse{};
    mse.dwLength = sizeof(mse);
    if (GlobalMemoryStatusEx(&mse)) {
        si.ramTotal = mse.ullTotalPhys;
        si.ramUsed = mse.ullTotalPhys - mse.ullAvailPhys;
    }
    // All drive letters A-Z with free/total.
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        char root[4] = {static_cast<char>('A' + d), ':', '\\', 0};
        UINT type = GetDriveTypeA(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER total{}, freeq{};
        if (GetDiskFreeSpaceExA(root, nullptr, &total, &freeq)) {
            SysInfo::Disk dk{};
            dk.letter[0] = static_cast<char>('A' + d);
            dk.letter[1] = ':';
            dk.total = total.QuadPart;
            dk.free_ = freeq.QuadPart;
            si.disks.push_back(dk);
        }
    }
    // IPs from GetAdaptersAddresses.
    {
        ULONG bufLen = 16384;
        std::vector<char> buf(bufLen);
        auto addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                 nullptr, addrs, &bufLen) == NO_ERROR) {
            for (auto a = addrs; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) continue;
                for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
                    char ip[64] = {0};
                    getnameinfo(u->Address.lpSockaddr, u->Address.iSockaddrLength,
                                ip, sizeof(ip), nullptr, 0, NI_NUMERICHOST);
                    si.ips.push_back(ip);
                }
            }
        }
    }
#else
    struct utsname un{};
    if (uname(&un) == 0) {
        si.hostName = un.nodename;
        si.osName = std::string(un.sysname) + " " + un.release;
    }
    si.cpuCount = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (si.cpuCount > 64) si.cpuCount = 64;
    // CPU brand from /proc/cpuinfo ("model name").
    {
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "model name", 10) == 0) {
                    char* colon = strchr(line, ':');
                    if (colon) {
                        si.cpuBrand = colon + 1;
                        while (!si.cpuBrand.empty() &&
                               (si.cpuBrand.front() == ' ' || si.cpuBrand.front() == '\t'))
                            si.cpuBrand.erase(si.cpuBrand.begin());
                        while (!si.cpuBrand.empty() &&
                               (si.cpuBrand.back() == '\n' || si.cpuBrand.back() == '\r' ||
                                si.cpuBrand.back() == ' '))
                            si.cpuBrand.pop_back();
                    }
                    break;
                }
            }
            fclose(f);
        }
    }
    // RAM type/speed from DMI (/sys/devices/virtual/dmi/id/meminfo).
    {
        FILE* f = fopen("/sys/devices/virtual/dmi/id/meminfo", "r");
        if (f) {
            char line[512];
            int count = 0;
            while (fgets(line, sizeof(line), f)) {
                // Format: "Handle 0x0028, DMI type 17, 40 bytes" not here; actual lines:
                // "System Memory: Size: 16384 MB" style varies; parse "Speed:" / "Type:".
                char key[64] = {0}, val[128] = {0};
                if (sscanf(line, "%63[^:]: %127[^\n]", key, val) >= 1) {
                    char* kp = key;
                    while (*kp == ' ') kp++;
                    std::string k = kp;
                    std::string v = val;
                    // trim leading space of value
                    size_t vs = v.find_first_not_of(" \t");
                    if (vs != std::string::npos) v = v.substr(vs);
                    else v.clear();
                    if (k.find("Speed") != std::string::npos && si.ramSpeed.empty()) {
                        if (v.find("MT/s") != std::string::npos || v.find("MHz") != std::string::npos)
                            si.ramSpeed = v;
                    } else if (k.find("Type") != std::string::npos && k.find("Type Detail") == std::string::npos &&
                               si.ramType.empty()) {
                        if (v.find("DDR") != std::string::npos) si.ramType = v;
                    } else if (k == "Size" && !v.empty() && v != "No Module Installed" &&
                               v.find("Not") == std::string::npos) {
                        count++;
                    }
                }
            }
            si.ramSlotsUsed = count;
            fclose(f);
        }
    }
    // Per-core CPU from /proc/stat lines cpu0..cpuN.
    {
        FILE* f = fopen("/proc/stat", "r");
        if (f) {
            char line[256];
            struct CoreTime { unsigned long long all, idle; };
            static CoreTime prev[64] = {};
            static bool prevValid = false;
            int idx = 0;
            while (idx < 64 && fgets(line, sizeof(line), f)) {
                unsigned long long u = 0, ni = 0, s = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
                if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                           &idx, &u, &ni, &s, &id, &io, &irq, &sirq, &st) >= 5 && idx < 64) {
                    unsigned long long all = u + ni + s + id + io + irq + sirq + st;
                    unsigned long long idleT = id + io;
                    if (prevValid && all > prev[idx].all) {
                        unsigned long long dAll = all - prev[idx].all;
                        unsigned long long dIdle = idleT - prev[idx].idle;
                        double pct = (dAll - dIdle) * 100.0 / dAll;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        si.cpuPerCore[idx] = pct;
                        si.cpuCoreSamples = idx + 1;
                    }
                    prev[idx] = {all, idleT};
                }
            }
            prevValid = true;
            fclose(f);
        }
    }
    // RAM.
    {
        FILE* f = fopen("/proc/meminfo", "r");
        if (f) {
            char key[64], unit[16];
            long long val = 0;
            unsigned long long total = 0, avail = 0;
            while (fscanf(f, "%63s %lld %15s", key, &val, unit) >= 2) {
                if (strcmp(key, "MemTotal:") == 0) total = static_cast<unsigned long long>(val) * 1024;
                else if (strcmp(key, "MemAvailable:") == 0) avail = static_cast<unsigned long long>(val) * 1024;
                if (total && avail) break;
            }
            si.ramTotal = total;
            si.ramUsed = total - avail;
            fclose(f);
        }
    }
    // Disks: statvfs on common mount points.
    {
        const char* mounts[] = {"/", "/home", "/tmp", "/var"};
        for (const char* m : mounts) {
            struct statvfs vfs{};
            if (statvfs(m, &vfs) == 0 && vfs.f_blocks > 0) {
                bool dup = false;
                for (auto& dk : si.disks)
                    if (dk.total == static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize) dup = true;
                if (dup) continue;
                SysInfo::Disk dk{};
                snprintf(dk.letter, sizeof(dk.letter), "%s", m);
                dk.total = static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize;
                dk.free_ = static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
                si.disks.push_back(dk);
            }
        }
    }
    // IPs via getifaddrs.
    {
        struct ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) == 0) {
            for (auto a = ifs; a; a = a->ifa_next) {
                if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
                char ip[64] = {0};
                getnameinfo(a->ifa_addr, sizeof(sockaddr), ip, sizeof(ip), nullptr, 0, NI_NUMERICHOST);
                si.ips.push_back(std::string(a->ifa_name) + ": " + ip);
            }
            freeifaddrs(ifs);
        }
    }
#endif
    siCached = si;
    return siCached;
}

// ===========================================================================
// Color & Terminal Detection
// ===========================================================================
static int g_colorMode = 0;

static bool IsColorEnabled() {
    if (g_colorMode == 1) return true;
    if (g_colorMode == -1) return false;
    if (const char* nc = getenv("NO_COLOR")) {
        if (*nc != '\0') return false;
    }
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

// Copy text to the system clipboard (used for API base URLs).
static bool CopyToClipboard(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hg) { CloseClipboard(); return false; }
    char* p = static_cast<char*>(GlobalLock(hg));
    if (!p) { GlobalFree(hg); CloseClipboard(); return false; }
    memcpy(p, text.c_str(), text.size() + 1);
    GlobalUnlock(hg);
    if (SetClipboardData(CF_TEXT, hg) == nullptr) {
        GlobalFree(hg);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
#else
    FILE* f = popen("xclip -selection clipboard 2>/dev/null || xsel --clipboard 2>/dev/null || wl-copy 2>/dev/null", "w");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);
    return pclose(f) == 0;
#endif
}

static bool OpenInBrowser(unsigned port) {
    std::string proto = (port == 443 || port == 8443) ? "https" : "http";
    std::string url = proto + "://localhost:" + std::to_string(port);
#ifdef _WIN32
    std::wstring wurl(url.begin(), url.end());
    HINSTANCE res = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(res) > 32;
#else
    std::string cmd = "xdg-open " + url + " >/dev/null 2>&1 &";
    return system(cmd.c_str()) == 0;
#endif
}

enum Key {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_ESC,
    KEY_KILL,
    KEY_OPEN,
    KEY_PING,
    KEY_REFRESH,
    KEY_QUIT,
    KEY_YES,
    KEY_NO,
    KEY_SETTINGS,
    KEY_BACKSPACE,
    KEY_CHAR
};

// Last raw character typed (valid when ReadKey returns KEY_CHAR).
static int g_lastKeyChar = 0;
// When true, ReadKey returns KEY_CHAR for every printable key (text input mode).
static bool g_rawInput = false;

#ifdef _WIN32
static Key ReadKey(int timeoutMs) {
    g_lastKeyChar = 0;
    int waited = 0;
    while (!_kbhit()) {
        Sleep(20);
        waited += 20;
        if (waited >= timeoutMs) return KEY_NONE;
    }
    int ch = _getch();
    if (ch == 224 || ch == 0) {
        int arrow = _getch();
        if (arrow == 72) return KEY_UP;
        if (arrow == 80) return KEY_DOWN;
        if (arrow == 75) return KEY_LEFT;
        if (arrow == 77) return KEY_RIGHT;
        return KEY_NONE;
    }
    if (ch == 13 || ch == 10) return KEY_ENTER;
    if (ch == 27) return KEY_ESC;
    if (ch == 8) return KEY_BACKSPACE;
    if (g_rawInput) {
        if (ch >= 32 && ch < 127) {
            g_lastKeyChar = ch;
            return KEY_CHAR;
        }
        return KEY_NONE;
    }
    if (ch == 'q' || ch == 'Q') return KEY_QUIT;
    if (ch == 'k' || ch == 'K' || ch == 'x' || ch == 'X') return KEY_KILL;
    if (ch == 'o' || ch == 'O') return KEY_OPEN;
    if (ch == 'p' || ch == 'P') return KEY_PING;
    if (ch == 'r' || ch == 'R') return KEY_REFRESH;
    if (ch == 's' || ch == 'S') return KEY_SETTINGS;
    if (ch == 'j' || ch == 'J') return KEY_DOWN;
    if (ch == 'w' || ch == 'W') return KEY_UP;
    if (ch == 'y' || ch == 'Y') return KEY_YES;
    if (ch == 'n' || ch == 'N') return KEY_NO;
    if (ch >= 32 && ch < 127) {
        g_lastKeyChar = ch;
        return KEY_CHAR;
    }
    return KEY_NONE;
}

struct RawTerminalScope {
    RawTerminalScope() {}
    ~RawTerminalScope() {}
};
#else
struct RawTerminalScope {
    termios orig{};
    bool active = false;
    RawTerminalScope() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active = true;
        }
    }
    ~RawTerminalScope() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
};

static Key ReadKey(int timeoutMs) {
    g_lastKeyChar = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int s = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    if (s <= 0) return KEY_NONE;

    char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) <= 0) return KEY_NONE;
    if (ch == 27) {
        char seq[2] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                if (seq[1] == 'A') return KEY_UP;
                if (seq[1] == 'B') return KEY_DOWN;
                if (seq[1] == 'C') return KEY_RIGHT;
                if (seq[1] == 'D') return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }
    if (ch == 10 || ch == 13) return KEY_ENTER;
    if (ch == 127 || ch == 8) return KEY_BACKSPACE;
    if (g_rawInput) {
        if (ch >= 32 && ch < 127) {
            g_lastKeyChar = ch;
            return KEY_CHAR;
        }
        return KEY_NONE;
    }
    if (ch == 'q' || ch == 'Q') return KEY_QUIT;
    if (ch == 'k' || ch == 'K' || ch == 'x' || ch == 'X') return KEY_KILL;
    if (ch == 'o' || ch == 'O') return KEY_OPEN;
    if (ch == 'p' || ch == 'P') return KEY_PING;
    if (ch == 'r' || ch == 'R') return KEY_REFRESH;
    if (ch == 's' || ch == 'S') return KEY_SETTINGS;
    if (ch == 'j' || ch == 'J') return KEY_DOWN;
    if (ch == 'y' || ch == 'Y') return KEY_YES;
    if (ch == 'n' || ch == 'N') return KEY_NO;
    if (ch >= 32 && ch < 127) {
        g_lastKeyChar = ch;
        return KEY_CHAR;
    }
    return KEY_NONE;
}
#endif

// ===========================================================================
// Render & Actions
// ===========================================================================
static std::string PingPort(unsigned port) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "Err";

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
#else
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return "Err";

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<u_short>(port));
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    connect(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;

    int sel = select(static_cast<int>(s + 1), nullptr, &wset, nullptr, &tv);
    if (sel <= 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return "Timeout";
    }

    const char* req = "HEAD / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, req, static_cast<int>(strlen(req)), 0);

    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s, &rset);
    tv.tv_sec = 0;
    tv.tv_usec = 250000;

    sel = select(static_cast<int>(s + 1), &rset, nullptr, nullptr, &tv);
    std::string res = "Open";
    if (sel > 0) {
        char buf[64] = {0};
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 8 && memcmp(buf, "HTTP/", 5) == 0) {
            char* sp = strchr(buf, ' ');
            if (sp && sp + 4 <= buf + n) {
                res = std::string(sp + 1, 3);
            } else {
                res = "HTTP";
            }
        }
    }

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return res;
}

// True if something accepts a TCP connection on 127.0.0.1:<port>.
static bool IsPortOpen(unsigned port) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
#else
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<u_short>(port));
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    connect(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 300000;

    bool ok = false;
    int sel = select(static_cast<int>(s + 1), nullptr, &wset, nullptr, &tv);
    if (sel > 0) {
        int err = 0;
#ifdef _WIN32
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
        ok = (err == 0);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return ok;
}

// True if <port> can be bound (i.e. nothing is listening / using it).
static bool IsPortFree(unsigned port) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    int excl = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl),
               sizeof(excl));
#else
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<u_short>(port));
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    bool free = (bind(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return free;
}

static int ExecuteKill(unsigned killPort, bool forceKill) {
    std::vector<Row> rows;
    CollectRows(rows, true, true);
    std::vector<unsigned long> targetPids;
    for (const auto& r : rows) {
        if (r.port == killPort && r.pid > 0) {
            if (std::find(targetPids.begin(), targetPids.end(), r.pid) == targetPids.end()) {
                targetPids.push_back(r.pid);
            }
        }
    }

    if (targetPids.empty()) {
        Out("No process found listening on port " + std::to_string(killPort) + ".\n");
        return 1;
    }

    int killedCount = 0;
    for (unsigned long pid : targetPids) {
        ProcInfo pi = GetProcInfo(pid);
        std::string procName = pi.name.empty() ? "unknown" : pi.name;
        if (!forceKill) {
            Out("Terminate process PID " + std::to_string(pid) + " (" + procName + ") on port " +
                std::to_string(killPort) + "? [y/N]: ");
            fflush(stdout);
            char ans[64] = {0};
            if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
                Out("Skipped PID " + std::to_string(pid) + ".\n");
                continue;
            }
        }
        if (TerminateProc(pid)) {
            Out("[OK] Terminated PID " + std::to_string(pid) + " (" + procName + ")\n");
            killedCount++;
        } else {
            Out("[ERROR] Failed to terminate PID " + std::to_string(pid) +
                " (try running as Administrator/root)\n");
        }
    }
    return (killedCount > 0) ? 0 : 1;
}

static void FilterByText(std::vector<Row>& rows);
static void FilterHiddenDirs(std::vector<Row>& rows);
static void ApplyIdeDirOverride(ProcInfo& pi, unsigned port);

static void RenderOutput(bool all, bool includeUdp, bool filterPort, unsigned startPort,
                         unsigned endPort, bool jsonOutput, bool csvOutput, bool yamlOutput,
                         bool pingCheck) {
    std::vector<Row> rows;
    CollectRows(rows, all, includeUdp);

    if (filterPort) {
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Row& r) {
                                      return r.port < startPort || r.port > endPort;
                                  }),
                   rows.end());
    }

    FilterHiddenDirs(rows);
    FilterByText(rows);

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.port != b.port) return a.port < b.port;
        if (a.addr != b.addr) return a.addr < b.addr;
        if (a.proto != b.proto) return a.proto < b.proto;
        return a.pid < b.pid;
    });

    // Remove duplicates
    rows.erase(std::unique(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.port == b.port && a.addr == b.addr && a.state == b.state && a.pid == b.pid &&
               a.proto == b.proto;
    }), rows.end());

    std::map<unsigned long, ProcInfo> cache;
    auto proc = [&](unsigned long pid) -> const ProcInfo& {
        auto it = cache.find(pid);
        if (it == cache.end()) it = cache.emplace(pid, GetProcInfo(pid)).first;
        return it->second;
    };

    if (jsonOutput) {
        Out("[\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            ProcInfo pi = proc(r.pid);
            if (pi.name.empty() || pi.name == "-") {
                auto* se = getservbyport(htons(static_cast<u_short>(r.port)), "tcp");
                if (se && se->s_name) pi.name = se->s_name;
                else pi.name = "system";
            }
            std::string tag = DetectTechStack(pi.dir);
            if (!tag.empty()) pi.name += " [" + tag + "]";

            if (pi.dir.empty() || pi.dir == "-") {
#ifdef _WIN32
                wchar_t sysDir[MAX_PATH];
                if (GetSystemDirectoryW(sysDir, MAX_PATH)) pi.dir = W2U8(std::wstring(sysDir) + L"\\");
                else pi.dir = "C:\\Windows\\system32\\";
#else
                pi.dir = "/etc/";
#endif
            }
            ApplyIdeDirOverride(pi, r.port);
            Out("  {\n");
            Out("    \"protocol\": \"" + r.proto + "\",\n");
            Out("    \"local_address\": \"" + r.addr + "\",\n");
            Out("    \"local_port\": " + std::to_string(r.port) + ",\n");
            Out("    \"state\": \"" + r.state + "\",\n");
            if (pingCheck) {
                Out("    \"health\": \"" + PingPort(r.port) + "\",\n");
            }
            Out("    \"pid\": " + std::to_string(r.pid) + ",\n");
            Out("    \"process\": \"" + EscapeJson(pi.name) + "\",\n");
            Out("    \"dir\": \"" + EscapeJson(pi.dir) + "\"\n");
            Out(i + 1 < rows.size() ? "  },\n" : "  }\n");
        }
        Out("]\n");
        return;
    }

    if (yamlOutput) {
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            ProcInfo pi = proc(r.pid);
            if (pi.name.empty() || pi.name == "-") {
                auto* se = getservbyport(htons(static_cast<u_short>(r.port)), "tcp");
                if (se && se->s_name) pi.name = se->s_name;
                else pi.name = "system";
            }
            std::string tag = DetectTechStack(pi.dir);
            if (!tag.empty()) pi.name += " [" + tag + "]";

            if (pi.dir.empty() || pi.dir == "-") {
#ifdef _WIN32
                pi.dir = "C:\\Windows\\system32\\";
#else
                pi.dir = "/etc/";
#endif
            }
            ApplyIdeDirOverride(pi, r.port);
            Out("- protocol: " + r.proto + "\n");
            Out("  local_address: \"" + r.addr + "\"\n");
            Out("  local_port: " + std::to_string(r.port) + "\n");
            Out("  state: " + r.state + "\n");
            if (pingCheck) {
                Out("  health: \"" + PingPort(r.port) + "\"\n");
            }
            Out("  pid: " + std::to_string(r.pid) + "\n");
            Out("  process: \"" + EscapeJson(pi.name) + "\"\n");
            Out("  dir: \"" + EscapeJson(pi.dir) + "\"\n");
        }
        return;
    }

    if (csvOutput) {
        if (pingCheck) {
            Out("LocalAddress,LocalPort,Protocol,State,Health,PID,Process,Dir\n");
        } else {
            Out("LocalAddress,LocalPort,Protocol,State,PID,Process,Dir\n");
        }
        for (const auto& r : rows) {
            ProcInfo pi = proc(r.pid);
            if (pi.name.empty() || pi.name == "-") {
                auto* se = getservbyport(htons(static_cast<u_short>(r.port)), "tcp");
                if (se && se->s_name) pi.name = se->s_name;
                else pi.name = "system";
            }
            std::string tag = DetectTechStack(pi.dir);
            if (!tag.empty()) pi.name += " [" + tag + "]";

            if (pi.dir.empty() || pi.dir == "-") {
#ifdef _WIN32
                pi.dir = "C:\\Windows\\system32\\";
#else
                pi.dir = "/etc/";
#endif
            }
            ApplyIdeDirOverride(pi, r.port);
            std::string pidStr = (r.pid == 0) ? "-" : std::to_string(r.pid);
            if (pingCheck) {
                Out(r.addr + "," + std::to_string(r.port) + "," + r.proto + "," + r.state + "," +
                    PingPort(r.port) + "," + pidStr + ",\"" + EscapeCsv(pi.name) + "\",\"" +
                    EscapeCsv(pi.dir) + "\"\n");
            } else {
                Out(r.addr + "," + std::to_string(r.port) + "," + r.proto + "," + r.state + "," +
                    pidStr + ",\"" + EscapeCsv(pi.name) + "\",\"" + EscapeCsv(pi.dir) + "\"\n");
            }
        }
        return;
    }

    std::vector<std::string> headers;
    if (pingCheck) {
        headers = {"LocalAddress", "LocalPort", "State", "Health", "PID", "Process", "Dir"};
    } else {
        headers = {"LocalAddress", "LocalPort", "State", "PID", "Process", "Dir"};
    }

    std::vector<std::vector<std::string>> table;
    for (const auto& r : rows) {
        ProcInfo pi = proc(r.pid);
        if (pi.name.empty() || pi.name == "-") {
            auto* se = getservbyport(htons(static_cast<u_short>(r.port)), "tcp");
            if (se && se->s_name) {
                pi.name = se->s_name;
            } else {
                pi.name = "system";
            }
        }
        std::string tag = DetectTechStack(pi.dir);
        if (!tag.empty()) pi.name += " [" + tag + "]";

        if (pi.dir.empty() || pi.dir == "-") {
#ifdef _WIN32
            wchar_t sysDir[MAX_PATH];
            if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
                pi.dir = W2U8(std::wstring(sysDir) + L"\\");
            } else {
                pi.dir = "C:\\Windows\\system32\\";
            }
#else
            const char* tryDirs[] = {"/etc/", "/usr/sbin/", "/usr/bin/"};
            for (const char* td : tryDirs) {
                std::string p = std::string(td) + pi.name;
                if (access(p.c_str(), F_OK) == 0) {
                    pi.dir = td;
                    break;
                }
            }
            if (pi.dir.empty()) pi.dir = "/etc/";
#endif
        }
        ApplyIdeDirOverride(pi, r.port);
        std::string pidStr = (r.pid == 0) ? "-" : std::to_string(r.pid);
        std::string stateDisplay = r.state;
        if (IsPublicDatabase(r.port, r.addr)) {
            stateDisplay += " [PUB!]";
        }

        if (pingCheck) {
            std::string health = PingPort(r.port);
            table.push_back({r.addr, std::to_string(r.port), stateDisplay, health, pidStr, pi.name,
                             pi.dir});
        } else {
            table.push_back(
                {r.addr, std::to_string(r.port), stateDisplay, pidStr, pi.name, pi.dir});
        }
    }

    std::map<unsigned, int> portCount;
    for (const auto& r : rows) {
        portCount[r.port]++;
    }

    size_t numCols = headers.size();
    std::vector<size_t> width(numCols);
    for (size_t c = 0; c < numCols; ++c) width[c] = headers[c].size();
    for (const auto& row : table) {
        for (size_t c = 0; c < numCols; ++c) width[c] = (std::max)(width[c], row[c].size());
    }

    auto printRow = [&](const std::vector<std::string>& cells, bool padLast, int portFreq,
                        bool isPub, bool isHeader) {
        std::string line;
        for (size_t c = 0; c < numCols; ++c) {
            std::string cellPadded = Pad(cells[c], width[c]);
            if (!isHeader && IsColorEnabled()) {
                if (c == 1 && portFreq != 0) {
                    if (portFreq > 1) {
                        line += "\x1b[1;31m" + cellPadded + "\x1b[0m";
                    } else {
                        line += "\x1b[1;32m" + cellPadded + "\x1b[0m";
                    }
                } else if (c == 2 && isPub) {
                    line += "\x1b[1;33m" + cellPadded + "\x1b[0m";
                } else if (pingCheck && c == 3) {
                    // Health column color
                    if (cells[c] == "200" || cells[c] == "Open") {
                        line += "\x1b[1;32m" + cellPadded + "\x1b[0m";
                    } else if (cells[c] == "301" || cells[c] == "302") {
                        line += "\x1b[1;36m" + cellPadded + "\x1b[0m";
                    } else if (cells[c] == "Timeout" || cells[c] == "Err") {
                        line += "\x1b[1;31m" + cellPadded + "\x1b[0m";
                    } else {
                        line += "\x1b[1;33m" + cellPadded + "\x1b[0m";
                    }
                } else {
                    if (c + 1 < numCols || padLast) line += cellPadded;
                    else line += cells[c];
                }
            } else {
                if (c + 1 < numCols || padLast) line += cellPadded;
                else line += cells[c];
            }
            if (c + 1 < numCols) line += "  ";
        }
        line += "\n";
        Out(line);
    };

    printRow(headers, true, 0, false, true);
    {
        std::string sep;
        for (size_t c = 0; c < numCols; ++c) {
            sep += std::string(width[c], '-');
            if (c + 1 < numCols) sep += "  ";
        }
        sep += "\n";
        Out(sep);
    }
    for (size_t i = 0; i < table.size(); ++i) {
        int freq = (i < rows.size()) ? portCount[rows[i].port] : 1;
        bool isPub = (i < rows.size()) ? IsPublicDatabase(rows[i].port, rows[i].addr) : false;
        printRow(table[i], false, freq, isPub, false);
    }
}

// Truncate a string to at most `w` bytes (ASCII-safe for column alignment).
static std::string Truncate(const std::string& s, size_t w) {
    return s.size() <= w ? s : s.substr(0, w);
}

static std::string ToLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string TrimWs(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

static bool ContainsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return ToLowerStr(haystack).find(ToLowerStr(needle)) != std::string::npos;
}

// Compare two working directories, ignoring case, trailing slashes and surrounding space.
static bool SameDir(const std::string& a, const std::string& b) {
    std::string x = ToLowerStr(TrimWs(a));
    std::string y = ToLowerStr(TrimWs(b));
    while (x.size() > 1 && (x.back() == '\\' || x.back() == '/')) x.pop_back();
    while (y.size() > 1 && (y.back() == '\\' || y.back() == '/')) y.pop_back();
    if (x.empty() || y.empty()) return false;
    return x == y;
}

// CLI text filter (e.g. `nxport node` or `nxport warkop`): matches name or Dir.
static std::string g_textFilter;

static void FillDirFallback(ProcInfo& pi);

static void FilterByText(std::vector<Row>& rows) {
    if (g_textFilter.empty()) return;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Row& r) {
                                  ProcInfo pi = GetProcInfo(r.pid);
                                  if (pi.name.empty() || pi.name == "-") {
                                      auto* se = getservbyport(
                                          htons(static_cast<u_short>(r.port)), "tcp");
                                      if (se && se->s_name) pi.name = se->s_name;
                                  }
                                  FillDirFallback(pi);
                                  ApplyIdeDirOverride(pi, r.port);
                                  if (ContainsCI(pi.name, g_textFilter)) return false;
                                  if (ContainsCI(pi.dir, g_textFilter)) return false;
                                  if (ContainsCI(r.proto, g_textFilter)) return false;
                                  return true;
                              }),
               rows.end());
}

// Fill a sensible fallback directory when the real CWD is unavailable.
static void FillDirFallback(ProcInfo& pi) {
    if (!pi.dir.empty() && pi.dir != "-") return;
#ifdef _WIN32
    wchar_t sysDir[MAX_PATH];
    if (GetSystemDirectoryW(sysDir, MAX_PATH))
        pi.dir = W2U8(std::wstring(sysDir) + L"\\");
    else
        pi.dir = "C:\\Windows\\system32\\";
#else
    pi.dir = "/etc/";
#endif
}

// ---------------------------------------------------------------------------
// Persistent settings (saved across runs)
// ---------------------------------------------------------------------------
struct ServerEntry {
    std::string name;  // label shown in the list
    std::string dir;   // working directory to run in
    std::string cmd;   // command to start the server
    unsigned port = 0; // optional port (for stop / open in browser)
};

struct Settings {
    std::vector<std::string> hiddenDirs;  // folders to hide (persisted)
    std::vector<ServerEntry> servers;     // saved servers (history)
    bool includeUdp = false;
    bool showAllStates = false;
    int refreshSeconds = 2;
    unsigned apiPort = 7900;
};

// Internal API server control (implemented near RunApiServer).
static bool ApiServerRunning();
static bool StartApiServer();
static bool StopApiServer();

static Settings g_settings;

// Escape/unescape a field so it can live on one "srv=" line (separator '|').
static std::string FieldEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '|') out += "\\p";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

static std::string FieldUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'p') out += '|';
            else if (n == '\\') out += '\\';
            else { out += '\\'; out += n; }
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string SettingsFilePath() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    std::string base = (n > 0 && n < MAX_PATH) ? std::string(buf, n) : std::string(".");
    std::string dir = base + "\\nxport";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.conf";
#else
    const char* home = getenv("HOME");
    std::string base = home ? home : ".";
    std::string dir = base + "/.config/nxport";
    mkdir(dir.c_str(), 0755);
    return dir + "/settings.conf";
#endif
}

static void LoadSettings(Settings& s) {
    FILE* f = fopen(SettingsFilePath().c_str(), "rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("dir=", 0) == 0) {
            std::string v = TrimWs(l.substr(4));
            if (!v.empty()) s.hiddenDirs.push_back(v);
        } else if (l.rfind("udp=", 0) == 0) {
            s.includeUdp = (l.substr(4) == "1");
        } else if (l.rfind("all=", 0) == 0) {
            s.showAllStates = (l.substr(4) == "1");
        } else if (l.rfind("refresh=", 0) == 0) {
            int v = atoi(l.c_str() + 8);
            if (v >= 1 && v <= 60) s.refreshSeconds = v;
        } else if (l.rfind("apiport=", 0) == 0) {
            unsigned v = static_cast<unsigned>(strtoul(l.c_str() + 8, nullptr, 10));
            if (v >= 1024 && v <= 65535) s.apiPort = v;
        } else if (l.rfind("srv=", 0) == 0) {
            // srv=name|dir|cmd|port
            std::string body = l.substr(4);
            std::vector<std::string> parts;
            std::string cur;
            for (size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '|') {
                    parts.push_back(cur);
                    cur.clear();
                } else {
                    cur += body[i];
                }
            }
            parts.push_back(cur);
            while (parts.size() < 4) parts.push_back("");
            ServerEntry e;
            e.name = FieldUnescape(parts[0]);
            e.dir = FieldUnescape(parts[1]);
            e.cmd = FieldUnescape(parts[2]);
            e.port = static_cast<unsigned>(strtoul(parts[3].c_str(), nullptr, 10));
            if (!e.cmd.empty() || !e.dir.empty()) s.servers.push_back(e);
        }
    }
    fclose(f);
}

static void SaveSettings(const Settings& s) {
    FILE* f = fopen(SettingsFilePath().c_str(), "wb");
    if (!f) return;
    for (const auto& d : s.hiddenDirs) fprintf(f, "dir=%s\n", d.c_str());
    for (const auto& e : s.servers)
        fprintf(f, "srv=%s|%s|%s|%u\n", FieldEscape(e.name).c_str(), FieldEscape(e.dir).c_str(),
                FieldEscape(e.cmd).c_str(), e.port);
    fprintf(f, "udp=%d\n", s.includeUdp ? 1 : 0);
    fprintf(f, "all=%d\n", s.showAllStates ? 1 : 0);
    fprintf(f, "refresh=%d\n", s.refreshSeconds);
    fprintf(f, "apiport=%u\n", s.apiPort);
    fclose(f);
}

// Remove rows whose Dir matches a saved "hidden folder" entry (blacklist).
static void FilterHiddenDirs(std::vector<Row>& rows) {
    if (g_settings.hiddenDirs.empty()) return;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Row& r) {
                                  ProcInfo pi = GetProcInfo(r.pid);
                                  FillDirFallback(pi);
                                  ApplyIdeDirOverride(pi, r.port);
                                  for (const auto& d : g_settings.hiddenDirs)
                                      if (ContainsCI(pi.dir, d)) return true;
                                  return false;
                              }),
               rows.end());
}

// Launch a saved server detached from this terminal (fire-and-forget).
static bool StartServer(const ServerEntry& e) {
    if (e.cmd.empty()) return false;
#ifdef _WIN32
    // Hide the console window entirely (not just without a window) so the
    // spawned server never flashes a shell.
    std::string cmdline = "cmd /c " + e.cmd;
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    std::string cwd = e.dir.empty() ? std::string(".") : e.dir;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, cwd.c_str(), &si,
                             &pi);
    if (!ok) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Child: detach from the terminal and run the command.
        setsid();
        if (!e.dir.empty()) {
            if (chdir(e.dir.c_str()) != 0) _exit(127);
        }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", e.cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

// Stop a saved server: kill by its port, else by matching Dir.
static int StopServer(const ServerEntry& e) {
    std::vector<Row> rows;
    CollectRows(rows, true, true);
    int killed = 0;
    for (const auto& r : rows) {
        if (r.pid == 0) continue;
        bool match = false;
        if (e.port != 0 && r.port == e.port) {
            match = true;
        } else if (e.port == 0 && !e.dir.empty()) {
            ProcInfo pi = GetProcInfo(r.pid);
            if (!pi.dir.empty() && ContainsCI(pi.dir, e.dir)) match = true;
        }
        if (match && TerminateProc(r.pid)) killed++;
    }
    return killed;
}

// ---------------------------------------------------------------------------
// Start-command auto detection (from project files in the working directory)
// ---------------------------------------------------------------------------
static bool PathExists(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

static std::string ReadFileText(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "/" + name;
}

// Guess how to start a server that lives in `dir`, based on project files.
static std::string DetectStartCommand(const std::string& dir) {
    if (dir.empty()) return "";
    if (PathExists(JoinPath(dir, "package.json"))) {
        std::string text = ReadFileText(JoinPath(dir, "package.json"));
        const char* scripts[] = {"dev", "start", "serve", "develop"};
        std::string script;
        for (const char* s : scripts) {
            std::string key = std::string("\"") + s + "\"";
            if (text.find(key) != std::string::npos) {
                script = s;
                break;
            }
        }
        std::string pm = "npm";
        if (PathExists(JoinPath(dir, "pnpm-lock.yaml"))) pm = "pnpm";
        else if (PathExists(JoinPath(dir, "yarn.lock"))) pm = "yarn";
        else if (PathExists(JoinPath(dir, "bun.lockb")) || PathExists(JoinPath(dir, "bun.lock")))
            pm = "bun";
        if (!script.empty()) {
            if (pm == "npm") return "npm run " + script;
            if (pm == "yarn") return "yarn " + script;
            if (pm == "pnpm") return "pnpm " + script;
            if (pm == "bun") return "bun run " + script;
        }
        return "";
    }
    if (PathExists(JoinPath(dir, "manage.py"))) return "python manage.py runserver";
    if (PathExists(JoinPath(dir, "artisan"))) return "php artisan serve";
    if (PathExists(JoinPath(dir, "Cargo.toml"))) return "cargo run";
    if (PathExists(JoinPath(dir, "go.mod"))) return "go run .";
    if (PathExists(JoinPath(dir, "mix.exs"))) return "mix phx.server";
    return "";
}

// ---------------------------------------------------------------------------
// NxCode IDE server mapping: the IDE runs its static/PHP servers in-process,
// so the owning port's Dir is the install folder. The port -> project folder
// mapping lives in the IDE's QSettings; read it to show the real project Dir.
// ---------------------------------------------------------------------------
static std::map<unsigned, std::string> g_ideDirs;

static void LoadNxCodeServerDirs() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    path = std::string(buf, n) + "\\NXC-Dev\\Editor IDE.ini";
#else
    const char* home = getenv("HOME");
    if (!home) return;
    path = std::string(home) + "/.config/NXC-Dev/Editor IDE.conf";
#endif
    std::string text = ReadFileText(path);
    if (text.empty()) return;

    size_t pos = text.find("entries=");
    if (pos == std::string::npos) return;
    std::string raw = text.substr(pos + 8);
    // QSettings INI escapes: \" -> " and \\ -> \.
    std::string s;
    for (size_t k = 0; k < raw.size(); ++k) {
        if (raw[k] == '\\' && k + 1 < raw.size()) {
            char nx = raw[k + 1];
            if (nx == '"') { s += '"'; k++; continue; }
            if (nx == '\\') { s += '\\'; k++; continue; }
        }
        s += raw[k];
    }

    size_t i = 0;
    for (;;) {
        size_t fp = s.find("\"folder\":\"", i);
        if (fp == std::string::npos) break;
        fp += 10;
        std::string folderRaw;
        for (; fp < s.size(); ++fp) {
            if (s[fp] == '"') break;
            folderRaw += s[fp];
        }
        std::string folder;
        for (size_t k = 0; k < folderRaw.size(); ++k) {
            if (folderRaw[k] == '\\' && k + 1 < folderRaw.size()) {
                char nx = folderRaw[k + 1];
                if (nx == '"') { folder += '"'; k++; continue; }
                if (nx == '\\') { folder += '\\'; k++; continue; }
                if (nx == '/') { folder += '/'; k++; continue; }
            }
            folder += folderRaw[k];
        }
        size_t pp = s.find("\"port\":", fp);
        unsigned port = 0;
        if (pp != std::string::npos) {
            port = static_cast<unsigned>(strtoul(s.c_str() + pp + 7, nullptr, 10));
            i = pp + 7;
        } else {
            i = fp;
        }
        if (port != 0 && !folder.empty()) g_ideDirs[port] = folder;
    }
}

// If the port belongs to an NxCode IDE server and the current Dir is the IDE
// install folder, show the mapped project folder instead.
static void ApplyIdeDirOverride(ProcInfo& pi, unsigned port) {
    if (g_ideDirs.empty()) return;
    auto it = g_ideDirs.find(port);
    if (it == g_ideDirs.end()) return;
    if (pi.dir.empty() || pi.dir == "-" || ContainsCI(pi.dir, "NxCode IDE")) {
        pi.dir = it->second;
    }
}

// Merge the currently-shown dashboard rows into the saved server history.
// Dedupes by Dir (never saves the same folder twice) and skips hidden folders.
static int ImportRunningServers(Settings& s, const std::vector<Row>& rows) {
    int added = 0;
    for (const auto& r : rows) {
        ProcInfo pi = GetProcInfo(r.pid);
        FillDirFallback(pi);
        ApplyIdeDirOverride(pi, r.port);
        if (pi.dir.empty() || pi.dir == "-") continue;
        bool hidden = false;
        for (const auto& d : s.hiddenDirs)
            if (ContainsCI(pi.dir, d)) { hidden = true; break; }
        if (hidden) continue;

        std::string name = pi.name;
        if (name.empty() || name == "-") name = "port " + std::to_string(r.port);

        int found = -1;
        for (size_t i = 0; i < s.servers.size(); ++i)
            if (SameDir(s.servers[i].dir, pi.dir)) { found = static_cast<int>(i); break; }
        if (found >= 0) {
            if (s.servers[found].port == 0) s.servers[found].port = r.port;
            if (s.servers[found].cmd.empty()) s.servers[found].cmd = DetectStartCommand(pi.dir);
            continue;
        }
        ServerEntry e;
        e.name = name;
        e.dir = pi.dir;
        e.cmd = DetectStartCommand(pi.dir);  // best-effort from project files
        e.port = r.port;
        s.servers.push_back(e);
        added++;
    }
    if (added > 0) SaveSettings(s);
    return added;
}

// Settings menu items.
enum SItem {
    S_ADD_DIR = 0,
    S_FILTERLIST,
    S_SERVERS,
    S_SYSINFO,
    S_SERVE,
    S_SERVE_PORT,
    S_UDP,
    S_ALL,
    S_REFRESH,
    S_RESET,
    S_COUNT
};

enum Screen {
    SC_LIST,
    SC_SETTINGS,
    SC_FILTERLIST,
    SC_SERVERS,
    SC_SYSINFO,
    SC_INPUT
};

// What the text-input screen is currently collecting.
enum InputPurpose {
    IN_HIDEDIR = 0,
    IN_SRV_NAME,
    IN_SRV_DIR,
    IN_SRV_CMD,
    IN_SRV_PORT,
    IN_APIPORT
};

static const char* SItemLabel(int i) {
    switch (i) {
        case S_ADD_DIR:    return "+ Hide Folder";
        case S_FILTERLIST: return "Hidden Folders";
        case S_SERVERS:    return "Server History";
        case S_SYSINFO:    return "System Info";
        case S_SERVE:      return "API Server";
        case S_SERVE_PORT: return "API Server Port";
        case S_UDP:        return "Show UDP";
        case S_ALL:        return "All TCP States";
        case S_REFRESH:    return "Refresh Interval";
        case S_RESET:      return "Reset All Settings";
        default:           return "";
    }
}

static std::string SItemValue(int i, const Settings& s) {
    switch (i) {
        case S_FILTERLIST: return "(" + std::to_string(s.hiddenDirs.size()) + ")";
        case S_SERVERS:    return "(" + std::to_string(s.servers.size()) + ")";
        case S_SERVE:      return ApiServerRunning() ? "running" : "stopped";
        case S_SERVE_PORT: return std::to_string(s.apiPort);
        case S_UDP:        return s.includeUdp ? "ON" : "off";
        case S_ALL:        return s.showAllStates ? "ON" : "off";
        case S_REFRESH:    return std::to_string(s.refreshSeconds) + " seconds";
        default:           return "";
    }
}

static void RunDashboard(bool all, bool includeUdp, bool filterPort, unsigned startPort,
                         unsigned endPort) {
    RawTerminalScope rawScope;

    (void)all;
    (void)includeUdp;

    Out("\x1b[?25l");  // hide cursor
    Out("\x1b[2J\x1b[H");

    // Persistent settings (filters saved across runs).
    Settings& settings = g_settings;

    Screen screen = SC_LIST;
    int selectedIdx = 0;
    int settingsSel = 0;
    int filterSel = 0;
    int serverSel = 0;
    InputPurpose inputPurpose = IN_HIDEDIR;
    std::string srvName, srvDir, srvCmd;
    std::string inputBuffer;
    // 0=port, 1=pid, 2=process, 3=addr, 4=dir
    enum SortMode {
        SORT_PORT = 0,
        SORT_PID,
        SORT_PROC,
        SORT_ADDR,
        SORT_DIR
    };
    int sortMode = SORT_PORT;
    bool searchMode = false;
    std::string searchQuery;   // applied filter
    std::string searchBuffer;  // being typed

    std::string statusMsg = "Ready. \u2191/\u2193 select  \u00b7  s settings  \u00b7  q quit";
    std::map<unsigned, std::string> pingResults;
    bool confirmKill = false;
    unsigned long killTargetPid = 0;
    unsigned killTargetPort = 0;
    std::string killTargetName;

    std::vector<Row> rows;
    std::map<unsigned long, ProcInfo> cache;
    bool needRefresh = true;
    bool dirty = true;
    bool importedOnce = false;
    bool firstScan = true;
    std::set<unsigned> prevPorts;
    std::map<unsigned, std::chrono::steady_clock::time_point> newUntil;
    auto lastRefresh = std::chrono::steady_clock::now();

    for (;;) {
        auto now = std::chrono::steady_clock::now();

        long elapsedMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRefresh).count());
        if (needRefresh || elapsedMs >= static_cast<long>(settings.refreshSeconds) * 1000) {
            rows.clear();
            cache.clear();
            CollectRows(rows, settings.showAllStates, settings.includeUdp);
            if (filterPort) {
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                          [&](const Row& r) {
                                              return r.port < startPort || r.port > endPort;
                                          }),
                           rows.end());
            }
            // Hide rows whose Dir matches a saved entry (blacklist).
            FilterHiddenDirs(rows);
            FilterByText(rows);
            // search query from search mode
            if (searchMode && !searchQuery.empty()) {
                rows.erase(std::remove_if(rows.begin(), rows.end(),
                                          [&](const Row& r) {
                                              ProcInfo pi = GetProcInfo(r.pid);
                                              if (pi.name.empty() || pi.name == "-") {
                                                  auto* se = getservbyport(
                                                      htons(static_cast<u_short>(r.port)), "tcp");
                                                  if (se && se->s_name) pi.name = se->s_name;
                                              }
                                              FillDirFallback(pi);
                                              ApplyIdeDirOverride(pi, r.port);
                                              return !(ContainsCI(pi.name, searchQuery) ||
                                                       ContainsCI(pi.dir, searchQuery) ||
                                                       ContainsCI(r.addr, searchQuery) ||
                                                       (std::to_string(r.port).find(searchQuery) !=
                                                        std::string::npos));
                                          }),
                           rows.end());
            }
            std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                if (sortMode == SORT_PID) {
                    if (a.pid != b.pid) return a.pid < b.pid;
                } else if (sortMode == SORT_PROC) {
                    ProcInfo pa = GetProcInfo(a.pid);
                    ProcInfo pb = GetProcInfo(b.pid);
                    if (pa.name != pb.name) return ToLowerStr(pa.name) < ToLowerStr(pb.name);
                } else if (sortMode == SORT_ADDR) {
                    if (a.addr != b.addr) return a.addr < b.addr;
                } else if (sortMode == SORT_DIR) {
                    ProcInfo pa = GetProcInfo(a.pid);
                    ProcInfo pb = GetProcInfo(b.pid);
                    if (pa.dir != pb.dir) return ToLowerStr(pa.dir) < ToLowerStr(pb.dir);
                } else {
                    if (a.port != b.port) return a.port < b.port;
                }
                if (a.port != b.port) return a.port < b.port;
                if (a.addr != b.addr) return a.addr < b.addr;
                if (a.proto != b.proto) return a.proto < b.proto;
                return a.pid < b.pid;
            });
            rows.erase(std::unique(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.port == b.port && a.addr == b.addr && a.state == b.state &&
                       a.pid == b.pid && a.proto == b.proto;
            }), rows.end());
            // On the first scan of this session, seed the server history from what is
            // currently listening (dedupes by Dir, skips hidden folders).
            if (!importedOnce) {
                importedOnce = true;
                int added = ImportRunningServers(settings, rows);
                if (added > 0)
                    statusMsg = "\x1b[1;32m[OK] Imported " + std::to_string(added) +
                                " running server(s) to Server History.\x1b[0m";
            }
            // Watch-diff: track ports that appear / disappear between scans.
            {
                std::set<unsigned> cur;
                for (const auto& r : rows) cur.insert(r.port);
                if (!firstScan) {
                    std::vector<unsigned> fresh, gone;
                    for (unsigned p : cur)
                        if (!prevPorts.count(p)) fresh.push_back(p);
                    for (unsigned p : prevPorts)
                        if (!cur.count(p)) gone.push_back(p);
                    auto exp = now + std::chrono::seconds(8);
                    for (unsigned p : fresh) newUntil[p] = exp;
                    if (!fresh.empty() || !gone.empty()) {
                        std::string m;
                        if (!fresh.empty()) {
                            m += "\x1b[1;32m[NEW]\x1b[0m";
                            for (unsigned p : fresh) m += " " + std::to_string(p);
                        }
                        if (!gone.empty()) {
                            if (!m.empty()) m += "   ";
                            m += "\x1b[1;33m[GONE]\x1b[0m";
                            for (unsigned p : gone) m += " " + std::to_string(p);
                        }
                        statusMsg = m;
                    }
                }
                firstScan = false;
                prevPorts = cur;
            }
            lastRefresh = now;
            needRefresh = false;
            // Only the live port list / system info need periodic redraws; the
            // settings / history / input screens stay static (no 2s flicker).
            if (screen == SC_LIST || screen == SC_SYSINFO) dirty = true;
            if (selectedIdx >= static_cast<int>(rows.size()))
                selectedIdx = static_cast<int>(rows.size()) - 1;
            if (selectedIdx < 0) selectedIdx = 0;
        }

        if (dirty) {
            int termRows = 30, termCols = 100;
            GetTerminalSize(termRows, termCols);

            // ---- Build the whole frame in one buffer (single write => no flicker) ----
            std::string f;
            f.reserve(1 << 15);
            f += "\x1b[?2026h";  // begin synchronized update
            f += "\x1b[H";       // cursor home (no full clear)

            time_t tnow = time(nullptr);
            tm* lt = localtime(&tnow);
            char timeBuf[64] = {0};
            if (lt) strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", lt);

            if (screen == SC_SETTINGS) {
                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  Settings\x1b[0m   \x1b[90m";
                f += timeBuf;
                f += "\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n";
                for (int i = 0; i < S_COUNT; ++i) {
                    bool sel = (i == settingsSel);
                    std::string line = std::string(sel ? "> " : "  ") + Pad(SItemLabel(i), 30);
                    std::string val = SItemValue(i, settings);
                    if (!val.empty()) line += "  " + val;
                    if (sel)
                        f += "\x1b[1;37;48;2;233;84;32m" + line + "\x1b[0m\x1b[K\r\n";
                    else
                        f += line + "\x1b[K\r\n";
                }
                f += "\x1b[K\r\n";
                f += "\x1b[90m  [\u2191/\u2193] select   [Enter] choose/change   [\u2190/\u2192] toggle   "
                     "[s/Esc] back\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n\x1b[1;30;47m STATUS \x1b[0m " + statusMsg + "\x1b[K\r\n";
            } else if (screen == SC_FILTERLIST) {
                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  Hidden Folders\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n";
                if (settings.hiddenDirs.empty()) {
                    f += "\x1b[90m  (no hidden folders)\x1b[0m\x1b[K\r\n";
                } else {
                    for (size_t i = 0; i < settings.hiddenDirs.size(); ++i) {
                        bool sel = (static_cast<int>(i) == filterSel);
                        std::string line = std::string(sel ? "> " : "  ") + settings.hiddenDirs[i];
                        if (sel)
                            f += "\x1b[1;37;48;2;233;84;32m" + line + "\x1b[0m\x1b[K\r\n";
                        else
                            f += line + "\x1b[K\r\n";
                    }
                }
                f += "\x1b[K\r\n";
                f += "\x1b[90m  [\u2191/\u2193] select   [Enter/d] show again   [a] add   [Esc/s] "
                     "back\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n\x1b[1;30;47m STATUS \x1b[0m " + statusMsg + "\x1b[K\r\n";
            } else if (screen == SC_SERVERS) {
                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  Server History\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n";
                if (settings.servers.empty()) {
                    f += "\x1b[90m  (no saved servers - press a to add one)\x1b[0m\x1b[K\r\n";
                } else {
                    for (size_t i = 0; i < settings.servers.size(); ++i) {
                        const ServerEntry& e = settings.servers[i];
                        bool sel = (static_cast<int>(i) == serverSel);
                        std::string label = e.name.empty() ? e.dir : e.name;
                        std::string line = std::string(sel ? "> " : "  ") + Pad(label, 22);
                        if (e.port) line += ":" + std::to_string(e.port) + " ";
                        else line += "     ";
                        line += e.cmd;
                        if (sel)
                            f += "\x1b[1;37;48;2;233;84;32m" + line + "\x1b[0m\x1b[K\r\n";
                        else
                            f += line + "\x1b[K\r\n";
                    }
                }
                f += "\x1b[K\r\n";
                f += "\x1b[90m  [\u2191/\u2193] select   [Enter/s] start   [x] stop   [o] open   "
                     "[c] copy URL   [a] add   [d] remove   [Esc] back\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n\x1b[1;30;47m STATUS \x1b[0m " + statusMsg + "\x1b[K\r\n";
            } else if (screen == SC_SYSINFO) {
                // Budget lines so the frame never exceeds the terminal height
                // (scrolling would duplicate the header). 8 fixed lines
                // (title, blanks, help, STATUS) + host/summary/ram/net/ips.
                int siMaxRows = termRows - 4;
                if (siMaxRows < 10) siMaxRows = 10;
                int siUsed = 3;  // title + blank + host(assumed present)
                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  System Info\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n";
                SysInfo si = GetSysInfo();
                SysStats st = SampleSysStats();
                if (!si.hostName.empty()) {
                    f += "  Host: \x1b[1m" + si.hostName + "\x1b[0m";
                    if (!si.osName.empty()) f += "   \x1b[90m" + si.osName + "\x1b[0m";
                    f += "\x1b[K\r\n";
                }
                f += "\x1b[K\r\n";
                siUsed += 1;
                // CPU summary + per-core bars (bar count fits the screen).
                {
                    int pct = st.cpu;
                    std::string cpuLine = "  CPU";
                    if (!si.cpuBrand.empty()) cpuLine += "  \x1b[1m" + si.cpuBrand + "\x1b[0m";
                    cpuLine += "  (" + std::to_string(si.cpuCount) + " cores)  ";
                    if (pct < 0) cpuLine += "\x1b[90mn/a\x1b[0m";
                    else {
                        const char* c = pct >= 85 ? "\x1b[1;31m" : (pct >= 60 ? "\x1b[1;33m" : "\x1b[1;32m");
                        cpuLine += std::string(c) + std::to_string(pct) + "%\x1b[0m";
                    }
                    f += cpuLine + "\x1b[K\r\n";
                    siUsed++;
                    if (si.cpuCoreSamples > 0) {
                        // Reserve space for RAM(1) + disks(N) + blank + net(1) + blank
                        // + help(1) + status(1) after the bars.
                        int reserve = 3 + static_cast<int>(si.disks.size()) + (si.ips.empty() ? 0 : 2);
                        int barBudget = siMaxRows - siUsed - reserve;
                        if (barBudget < 0) barBudget = 0;
                        if (barBudget > si.cpuCoreSamples) barBudget = si.cpuCoreSamples;
                        for (int i = 0; i < barBudget; ++i) {
                            double v = si.cpuPerCore[i];
                            int filled = static_cast<int>(v / 100.0 * 20.0 + 0.5);
                            std::string bar;
                            const char* c = v >= 85.0 ? "\x1b[1;31m" : (v >= 60.0 ? "\x1b[1;33m" : "\x1b[1;32m");
                            bar += std::string(c);
                            for (int b = 0; b < 20; ++b) bar += (b < filled) ? "\u2588" : "\u2591";
                            bar += "\x1b[0m";
                            char lab[16];
                            snprintf(lab, sizeof(lab), "%3d%%", static_cast<int>(v));
                            char coreLab[16];
                            snprintf(coreLab, sizeof(coreLab), "  cpu%-2d ", i);
                            f += "    " + std::string(coreLab) + bar + " " + lab + "\x1b[K\r\n";
                            siUsed++;
                        }
                        if (barBudget < si.cpuCoreSamples) {
                            int rem = si.cpuCoreSamples - barBudget;
                            f += "    \x1b[90m... " + std::to_string(rem) +
                                 " more cores\x1b[0m\x1b[K\r\n";
                            siUsed++;
                        }
                    }
                }
                f += "\x1b[K\r\n";
                // RAM.
                {
                    std::string ramLine = "  RAM  ";
                    if (st.ram < 0) {
                        ramLine += "\x1b[90mn/a\x1b[0m";
                    } else {
                        const char* c = st.ram >= 85 ? "\x1b[1;31m" : (st.ram >= 60 ? "\x1b[1;33m" : "\x1b[1;32m");
                        ramLine += std::string(c) + std::to_string(st.ram) + "%\x1b[0m";
                    }
                    if (si.ramTotal > 0) {
                        std::string ramDet = FormatBytes(si.ramUsed) + " / " + FormatBytes(si.ramTotal);
                        if (!si.ramType.empty()) ramDet += "  " + si.ramType;
                        if (!si.ramSpeed.empty()) ramDet += "  " + si.ramSpeed;
                        ramLine += "   \x1b[90m" + ramDet + "\x1b[0m";
                    }
                    f += ramLine + "\x1b[K\r\n";
                }
                // Disks.
                for (const auto& d : si.disks) {
                    unsigned long long used = d.total - d.free_;
                    int pct = (d.total > 0) ? static_cast<int>(used * 100 / d.total) : 0;
                    const char* c = pct >= 85 ? "\x1b[1;31m" : (pct >= 60 ? "\x1b[1;33m" : "\x1b[1;32m");
                    char lab[16];
                    snprintf(lab, sizeof(lab), "%s", d.letter);
                    f += "  Disk " + std::string(lab) + " " + std::string(c) +
                         std::to_string(pct) + "%\x1b[0m   " + FormatBytes(used) + " / " +
                         FormatBytes(d.total) + "\x1b[K\r\n";
                }
                f += "\x1b[K\r\n";
                // Network.
                {
                    f += "  Network  ";
                    if (st.netBps < 0) f += "\x1b[90mn/a\x1b[0m";
                    else f += "\x1b[1;36m" + FormatBps(st.netBps) + "\x1b[0m";
                    f += "  (total rx+tx, all interfaces)\x1b[K\r\n";
                }
                f += "\x1b[K\r\n";
                // IPs.
                if (!si.ips.empty()) {
                    f += "  \x1b[1mIP Addresses:\x1b[0m\x1b[K\r\n";
                    int ipBudget = siMaxRows - siUsed - 4;  // help/status/blank reserve
                    if (ipBudget < 0) ipBudget = 0;
                    int shown = 0;
                    for (const auto& ip : si.ips) {
                        if (shown >= ipBudget) {
                            f += "    \x1b[90m... " +
                                 std::to_string(static_cast<int>(si.ips.size()) - shown) +
                                 " more\x1b[0m\x1b[K\r\n";
                            break;
                        }
                        f += "    " + ip + "\x1b[K\r\n";
                        shown++;
                        siUsed++;
                    }
                }
                f += "\x1b[K\r\n";
                f += "\x1b[90m  [Esc/s] back   [r] refresh\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n\x1b[1;30;47m STATUS \x1b[0m " + statusMsg + "\x1b[K\r\n";
            } else if (screen == SC_INPUT) {
                std::string title = "Hide Folder";
                std::string line1, line2;
                switch (inputPurpose) {
                    case IN_HIDEDIR:
                        title = "Hide Folder";
                        line1 = "  Processes in this folder will be \x1b[1mhidden\x1b[0m from the list.";
                        line2 = "  Enter a folder path/name (substring), e.g. D:\\Warkop";
                        break;
                    case IN_SRV_NAME:
                        title = "Add Server (1/4)";
                        line1 = "  A short name for this server, e.g. Warkop API";
                        line2 = "  (leave empty to use the folder name)";
                        break;
                    case IN_SRV_DIR:
                        title = "Add Server (2/4)";
                        line1 = "  Working directory to run the server in.";
                        line2 = "  e.g. D:\\Warkop\\api";
                        break;
                    case IN_SRV_CMD:
                        title = "Add Server (3/4)";
                        line1 = "  Command that starts the server.";
                        line2 = "  e.g. npm run dev   or   python manage.py runserver";
                        break;
                    case IN_SRV_PORT:
                        title = "Add Server (4/4)";
                        line1 = "  Port the server listens on (optional).";
                        line2 = "  Used for Stop and Open-in-browser, e.g. 8080";
                        break;
                    case IN_APIPORT:
                        title = "API Server Port";
                        line1 = "  Port for the internal API server (1024-65535).";
                        line2 = "  Endpoints: /api/ports  /api/servers  /api/stats";
                        break;
                }
                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  " + title + "\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n";
                f += line1 + "\x1b[K\r\n";
                f += line2 + "\x1b[K\r\n";
                f += "\x1b[K\r\n";
                f += "\x1b[1m  > \x1b[0m" + inputBuffer + "_\x1b[K\r\n";
                f += "\x1b[K\r\n";
                f += "\x1b[90m  [Enter] next/save   [Backspace] delete   [Esc] cancel\x1b[0m\x1b[K\r\n";
                f += "\x1b[K\r\n\x1b[1;30;47m STATUS \x1b[0m " + statusMsg + "\x1b[K\r\n";
            } else {
                // ---- SC_LIST ----
                auto proc = [&](unsigned long pid) -> const ProcInfo& {
                    auto it = cache.find(pid);
                    if (it == cache.end()) it = cache.emplace(pid, GetProcInfo(pid)).first;
                    return it->second;
                };

                std::vector<std::string> headers = {"",    "LocalAddress", "LocalPort",
                                                    "State", "PID",        "Process",
                                                    "Dir"};
                const size_t numCols = headers.size();

                std::vector<std::vector<std::string>> table;
                std::vector<bool> isNewRow;
                table.reserve(rows.size());
                isNewRow.reserve(rows.size());
                for (size_t i = 0; i < rows.size(); ++i) {
                    const Row& r = rows[i];
                    ProcInfo pi = proc(r.pid);
                    if (pi.name.empty() || pi.name == "-") {
                        auto* se = getservbyport(htons(static_cast<u_short>(r.port)), "tcp");
                        if (se && se->s_name) pi.name = se->s_name;
                        else pi.name = "system";
                    }
                    std::string tag = DetectTechStack(pi.dir);
                    if (!tag.empty()) pi.name += " [" + tag + "]";
                    FillDirFallback(pi);
                    ApplyIdeDirOverride(pi, r.port);
                    std::string stateStr = r.state;
                    if (IsPublicDatabase(r.port, r.addr)) stateStr += " [PUB!]";
                    auto itPing = pingResults.find(r.port);
                    if (itPing != pingResults.end()) stateStr += " (" + itPing->second + ")";
                    std::string pidStr = (r.pid == 0) ? "-" : std::to_string(r.pid);
                    bool isNew = false;
                    auto itNew = newUntil.find(r.port);
                    if (itNew != newUntil.end() && now < itNew->second) {
                        isNew = true;
                        stateStr += " [NEW]";
                    }
                    isNewRow.push_back(isNew);
                    table.push_back({(static_cast<int>(i) == selectedIdx) ? ">" : (isNew ? "*" : " "),
                                     r.addr, std::to_string(r.port), stateStr, pidStr, pi.name,
                                     pi.dir});
                }

                std::vector<size_t> width(numCols, 0);
                for (size_t c = 0; c < numCols; ++c) width[c] = headers[c].size();
                for (const auto& row : table)
                    for (size_t c = 0; c < numCols; ++c)
                        width[c] = (std::max)(width[c], row[c].size());

                size_t fixed = 0;
                for (size_t c = 0; c + 1 < numCols; ++c) fixed += width[c] + 2;
                size_t dirCap = (termCols > static_cast<int>(fixed) + 12)
                                    ? static_cast<size_t>(termCols) - fixed - 1
                                    : 12;
                if (width[numCols - 1] > dirCap) width[numCols - 1] = dirCap;

                int maxDataRows = termRows - 10;  // keep frame <= termRows-1 lines so no terminal scroll
                if (maxDataRows < 3) maxDataRows = 3;
                int first = 0;
                int total = static_cast<int>(table.size());
                if (total > maxDataRows) {
                    first = selectedIdx - maxDataRows / 2;
                    if (first < 0) first = 0;
                    if (first + maxDataRows > total) first = total - maxDataRows;
                }
                int last = (std::min)(total, first + maxDataRows);

                f += "\x1b[1;38;2;233;84;32m  NXPORT\x1b[0m\x1b[1m  live\x1b[0m   \x1b[90m";
                f += timeBuf;
                f += "  \u00b7  ";
                f += std::to_string(total);
                f += " port";
                if (searchMode || !searchQuery.empty()) {
                    f += "  \u00b7  filter:\"" + (searchMode ? searchBuffer : searchQuery) + "\"";
                }
                if (sortMode != SORT_PORT) {
                    static const char* sn[] = {"port", "pid", "process", "addr", "dir"};
                    f += "  \u00b7  sort:" + std::string(sn[sortMode]);
                }
                f += "\x1b[0m\x1b[K\r\n";
                // System stats line: CPU / RAM / Disk / Network.
                {
                    SysStats st = SampleSysStats();
                    auto pctCol = [](int v) {
                        if (v < 0) return std::string("\x1b[90mn/a\x1b[0m");
                        const char* c = v >= 85 ? "\x1b[1;31m" : (v >= 60 ? "\x1b[1;33m" : "\x1b[1;32m");
                        return std::string(c) + std::to_string(v) + "%\x1b[0m";
                    };
                    f += "\x1b[1m  CPU\x1b[0m\x1b[90m (";
                    f += pctCol(st.cpu);
                    f += ")  RAM (";
                    f += pctCol(st.ram);
                    f += ")  Disk (";
                    f += pctCol(st.disk);
                    f += ")  Network (";
                    if (st.netBps < 0) {
                        f += "\x1b[90mn/a\x1b[0m";
                    } else {
                        f += "\x1b[1;36m" + FormatBps(st.netBps) + "\x1b[0m";
                    }
                    f += ")\x1b[0m\x1b[K\r\n";
                }
                f += "\x1b[90m  [\u2191/\u2193] select   [Enter/o] open browser   [k] kill   [p] ping   "
                     "[r] refresh   [s] settings   [/] search   [c] sort   [q] quit\x1b[0m\x1b[K\r\n";
                if (searchMode)
                    f += "\x1b[1;33m  search: \x1b[0m" + searchBuffer + "_\x1b[K\r\n";
                else
                    f += "\x1b[K\r\n";

                std::string hdrLine;
                for (size_t c = 0; c < numCols; ++c) {
                    hdrLine += Pad(headers[c], width[c]);
                    if (c + 1 < numCols) hdrLine += "  ";
                }
                f += "\x1b[1m";
                f += hdrLine;
                f += "\x1b[0m\x1b[K\r\n";

                std::string sepLine;
                for (size_t c = 0; c < numCols; ++c) {
                    sepLine += std::string(width[c], '-');
                    if (c + 1 < numCols) sepLine += "  ";
                }
                f += sepLine;
                f += "\x1b[K\r\n";

                for (int i = first; i < last; ++i) {
                    bool isSel = (i == selectedIdx);
                    int portFreq = 0;
                    for (const auto& r : rows)
                        if (r.port == rows[i].port) portFreq++;
                    bool isPub = IsPublicDatabase(rows[i].port, rows[i].addr);

                    std::string rowLine;
                    for (size_t c = 0; c < numCols; ++c) {
                        std::string cell = table[i][c];
                        if (c + 1 == numCols) cell = Truncate(cell, width[c]);
                        std::string cellPadded = Pad(cell, width[c]);
                        if (!isSel && c == 0 && isNewRow[i]) {
                            rowLine += "\x1b[1;32m" + cellPadded + "\x1b[0m";
                        } else if (!isSel && c == 2 && portFreq > 1) {
                            rowLine += "\x1b[1;31m" + cellPadded + "\x1b[0m";
                        } else if (!isSel && c == 2) {
                            rowLine += "\x1b[1;32m" + cellPadded + "\x1b[0m";
                        } else if (!isSel && c == 3 && isPub) {
                            rowLine += "\x1b[1;33m" + cellPadded + "\x1b[0m";
                        } else if (!isSel && c == 3 && isNewRow[i]) {
                            rowLine += "\x1b[1;32m" + cellPadded + "\x1b[0m";
                        } else {
                            rowLine += cellPadded;
                        }
                        if (c + 1 < numCols) rowLine += "  ";
                    }

                    if (isSel) {
                        f += "\x1b[1;37;48;2;233;84;32m";
                        f += rowLine;
                        f += "\x1b[0m\x1b[K\r\n";
                    } else {
                        f += rowLine;
                        f += "\x1b[K\r\n";
                    }
                }

                f += "\x1b[K\r\n";
                f += "\x1b[1;30;47m STATUS \x1b[0m ";
                f += statusMsg;
                f += "\x1b[K";
                if (confirmKill) {
                f += "\r\n";
                f += "\x1b[1;31m[WARNING]\x1b[0m Kill PID " +
                     std::to_string(killTargetPid) + " (" + killTargetName + ") on port " +
                     std::to_string(killTargetPort) +
                     "? Press \x1b[1m[y]\x1b[0m to YES, any other key to CANCEL.\x1b[K";
                }
            }

            f += "\x1b[J";       // clear any leftover lines below
            f += "\x1b[?2026l";  // end synchronized update
            Out(f);              // single write
            fflush(stdout);
            dirty = false;
        }

        // Responsive key polling without forcing a redraw every tick.
        g_rawInput = (screen == SC_INPUT) || (screen == SC_LIST && searchMode);
        Key k = ReadKey(60);
        if (k == KEY_NONE) continue;

        if (confirmKill) {
            if (k == KEY_YES) {
                if (TerminateProc(killTargetPid)) {
                    statusMsg = "\x1b[1;32m[OK] Killed PID " +
                                std::to_string(killTargetPid) + " (" + killTargetName + ").\x1b[0m";
                } else {
                    statusMsg = "\x1b[1;31m[ERROR] Failed to kill PID " +
                                std::to_string(killTargetPid) +
                                " (requires Administrator/root).\x1b[0m";
                }
            } else {
                statusMsg = "Kill cancelled.";
            }
            confirmKill = false;
            needRefresh = true;
            dirty = true;
            continue;
        }

        if (screen == SC_INPUT) {
            if (k == KEY_ESC) {
                inputBuffer.clear();
                screen = (inputPurpose == IN_HIDEDIR) ? SC_SETTINGS : SC_SERVERS;
                dirty = true;
            } else if (k == KEY_BACKSPACE) {
                if (!inputBuffer.empty()) inputBuffer.pop_back();
                dirty = true;
            } else if (k == KEY_CHAR) {
                inputBuffer += static_cast<char>(g_lastKeyChar);
                dirty = true;
            } else if (k == KEY_ENTER) {
                std::string v = TrimWs(inputBuffer);
                inputBuffer.clear();
                switch (inputPurpose) {
                    case IN_HIDEDIR:
                        if (!v.empty()) {
                            settings.hiddenDirs.push_back(v);
                            SaveSettings(settings);
                            statusMsg = "\x1b[1;32m[OK] Folder hidden: " + v + "\x1b[0m";
                            needRefresh = true;
                        }
                        screen = SC_SETTINGS;
                        break;
                    case IN_SRV_NAME:
                        srvName = v;
                        inputPurpose = IN_SRV_DIR;
                        break;
                    case IN_SRV_DIR:
                        srvDir = v;
                        inputBuffer = DetectStartCommand(srvDir);  // prefill guessed command
                        inputPurpose = IN_SRV_CMD;
                        break;
                    case IN_SRV_CMD:
                        srvCmd = v;
                        inputPurpose = IN_SRV_PORT;
                        break;
                    case IN_SRV_PORT: {
                        ServerEntry e;
                        e.name = srvName;
                        e.dir = srvDir;
                        e.cmd = srvCmd;
                        e.port = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 10));
                        if (!e.cmd.empty()) {
                            int found = -1;
                            for (size_t i = 0; i < settings.servers.size(); ++i)
                                if (!e.dir.empty() && SameDir(settings.servers[i].dir, e.dir)) {
                                    found = static_cast<int>(i);
                                    break;
                                }
                            if (found >= 0) {
                                if (!e.name.empty()) settings.servers[found].name = e.name;
                                settings.servers[found].cmd = e.cmd;
                                if (e.port != 0) settings.servers[found].port = e.port;
                                serverSel = found;
                                statusMsg = "\x1b[1;32m[OK] Server updated: " +
                                            (e.name.empty() ? e.dir : e.name) + "\x1b[0m";
                            } else {
                                settings.servers.push_back(e);
                                serverSel = static_cast<int>(settings.servers.size()) - 1;
                                statusMsg = "\x1b[1;32m[OK] Server saved: " +
                                            (e.name.empty() ? e.dir : e.name) + "\x1b[0m";
                            }
                            SaveSettings(settings);
                        }
                        screen = SC_SERVERS;
                        break;
                    }
                    case IN_APIPORT: {
                        unsigned p = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 10));
                        if (p >= 1024 && p <= 65535) {
                            settings.apiPort = p;
                            SaveSettings(settings);
                            statusMsg = "\x1b[1;32m[OK] API port set to " + std::to_string(p) +
                                        "  (start the API server to apply)\x1b[0m";
                        } else {
                            statusMsg = "\x1b[1;31m[ERROR] Port must be 1024-65535.\x1b[0m";
                        }
                        screen = SC_SETTINGS;
                        break;
                    }
                }
                dirty = true;
            }

            continue;
        }

        if (screen == SC_SYSINFO) {
            if (k == KEY_ESC || k == KEY_SETTINGS) {
                screen = SC_SETTINGS;
                dirty = true;
            } else if (k == 'r' || k == 'R') {
                dirty = true;  // re-sample
            }
            continue;
        }

        if (screen == SC_SERVERS) {
            if (k == KEY_ESC) {
                screen = SC_SETTINGS;
                dirty = true;
            } else if (k == KEY_UP) {
                if (!settings.servers.empty()) {
                    if (serverSel > 0) serverSel--;
                    else serverSel = static_cast<int>(settings.servers.size()) - 1;
                }
                dirty = true;
            } else if (k == KEY_DOWN) {
                if (!settings.servers.empty()) {
                    if (serverSel + 1 < static_cast<int>(settings.servers.size())) serverSel++;
                    else serverSel = 0;
                }
                dirty = true;
            } else if ((k == KEY_ENTER || k == KEY_SETTINGS) && !settings.servers.empty()) {
                const ServerEntry& e = settings.servers[serverSel];
                std::string label = e.name.empty() ? e.dir : e.name;
                if (e.cmd.empty()) {
                    statusMsg = "\x1b[1;33m[WARNING] Auto-detected entry has no start command - "
                                "add one with 'a' (same folder).\x1b[0m";
                } else if (StartServer(e)) {
                    statusMsg = "\x1b[1;32m[OK] Started server: " + label + "\x1b[0m";
                    needRefresh = true;
                } else {
                    statusMsg = "\x1b[1;31m[ERROR] Failed to start server.\x1b[0m";
                }
                dirty = true;
            } else if (k == KEY_KILL && !settings.servers.empty()) {
                const ServerEntry& e = settings.servers[serverSel];
                std::string label = e.name.empty() ? e.dir : e.name;
                int killed = StopServer(e);
                if (killed > 0)
                    statusMsg = "\x1b[1;32m[OK] Stopped " + label + " (" +
                                std::to_string(killed) + " process).\x1b[0m";
                else
                    statusMsg = "\x1b[1;33m[WARNING] Nothing to stop for " + label +
                                " (set a port, or start it first).\x1b[0m";
                needRefresh = true;
                dirty = true;
            } else if (k == KEY_OPEN && !settings.servers.empty()) {
                const ServerEntry& e = settings.servers[serverSel];
                if (e.port != 0 && OpenInBrowser(e.port)) {
                    statusMsg = "\x1b[1;32m[OK] Browser opened: http://localhost:" +
                                std::to_string(e.port) + "\x1b[0m";
                } else {
                    statusMsg = "\x1b[1;33m[WARNING] No port set for this server.\x1b[0m";
                }
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'c' && !settings.servers.empty()) {
                // Copy the API base URL for testing in Postman.
                const ServerEntry& e = settings.servers[serverSel];
                if (e.port != 0) {
                    std::string url = "http://localhost:" + std::to_string(e.port);
                    CopyToClipboard(url);
                    statusMsg = "\x1b[1;32m[OK] Copied to clipboard: " + url + "\x1b[0m";
                } else {
                    statusMsg = "\x1b[1;33m[WARNING] No port set for this server.\x1b[0m";
                }
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'a') {
                inputBuffer.clear();
                srvName = srvDir = srvCmd = "";
                inputPurpose = IN_SRV_NAME;
                screen = SC_INPUT;
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'd' && !settings.servers.empty()) {
                std::string label = settings.servers[serverSel].name;
                if (label.empty()) label = settings.servers[serverSel].dir;
                settings.servers.erase(settings.servers.begin() + serverSel);
                if (serverSel >= static_cast<int>(settings.servers.size()))
                    serverSel = static_cast<int>(settings.servers.size()) - 1;
                if (serverSel < 0) serverSel = 0;
                SaveSettings(settings);
                statusMsg = "\x1b[1;33m[Removed] server: " + label + "\x1b[0m";
                dirty = true;
            }
            continue;
        }

        if (screen == SC_FILTERLIST) {
            if (k == KEY_ESC || k == KEY_SETTINGS) {
                screen = SC_SETTINGS;
                dirty = true;
            } else if (k == KEY_UP) {
                if (!settings.hiddenDirs.empty()) {
                    if (filterSel > 0) filterSel--;
                    else filterSel = static_cast<int>(settings.hiddenDirs.size()) - 1;
                }
                dirty = true;
            } else if (k == KEY_DOWN) {
                if (!settings.hiddenDirs.empty()) {
                    if (filterSel + 1 < static_cast<int>(settings.hiddenDirs.size())) filterSel++;
                    else filterSel = 0;
                }
                dirty = true;
            } else if ((k == KEY_ENTER || (k == KEY_CHAR && g_lastKeyChar == 'd')) &&
                       !settings.hiddenDirs.empty()) {
                std::string removed = settings.hiddenDirs[filterSel];
                settings.hiddenDirs.erase(settings.hiddenDirs.begin() + filterSel);
                if (filterSel >= static_cast<int>(settings.hiddenDirs.size()))
                    filterSel = static_cast<int>(settings.hiddenDirs.size()) - 1;
                if (filterSel < 0) filterSel = 0;
                SaveSettings(settings);
                statusMsg = "\x1b[1;33m[Unhidden] folder: " + removed + "\x1b[0m";
                needRefresh = true;
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'a') {
                inputBuffer.clear();
                inputPurpose = IN_HIDEDIR;
                screen = SC_INPUT;
                dirty = true;
            }
            continue;
        }

        if (screen == SC_SETTINGS) {
            if (k == KEY_ESC || k == KEY_SETTINGS) {
                screen = SC_LIST;
                dirty = true;
            } else if (k == KEY_UP) {
                if (settingsSel > 0) settingsSel--;
                else settingsSel = S_COUNT - 1;
                dirty = true;
            } else if (k == KEY_DOWN) {
                if (settingsSel + 1 < S_COUNT) settingsSel++;
                else settingsSel = 0;
                dirty = true;
            } else if (k == KEY_LEFT || k == KEY_RIGHT ||
                       (k == KEY_ENTER &&
                        (settingsSel == S_UDP || settingsSel == S_ALL || settingsSel == S_REFRESH))) {
                if (settingsSel == S_UDP) {
                    settings.includeUdp = !settings.includeUdp;
                    needRefresh = true;
                } else if (settingsSel == S_ALL) {
                    settings.showAllStates = !settings.showAllStates;
                    needRefresh = true;
                } else if (settingsSel == S_REFRESH) {
                    settings.refreshSeconds += (k == KEY_LEFT) ? -1 : 1;
                    if (settings.refreshSeconds < 1) settings.refreshSeconds = 10;
                    if (settings.refreshSeconds > 10) settings.refreshSeconds = 1;
                }
                SaveSettings(settings);
                dirty = true;
            } else if (k == KEY_ENTER && settingsSel == S_ADD_DIR) {
                inputBuffer.clear();
                inputPurpose = IN_HIDEDIR;
                screen = SC_INPUT;
                dirty = true;
            } else if (k == KEY_ENTER && settingsSel == S_FILTERLIST) {
                filterSel = 0;
                screen = SC_FILTERLIST;
                dirty = true;
            } else if (k == KEY_ENTER && settingsSel == S_SERVERS) {
                serverSel = 0;
                screen = SC_SERVERS;
                dirty = true;
            } else if (k == KEY_ENTER && settingsSel == S_SYSINFO) {
                screen = SC_SYSINFO;
                dirty = true;
            } else if ((k == KEY_ENTER || k == KEY_LEFT || k == KEY_RIGHT) &&
                       settingsSel == S_SERVE) {
                if (ApiServerRunning()) {
                    if (StopApiServer())
                        statusMsg = "\x1b[1;32m[OK] API server stopped.\x1b[0m";
                    else
                        statusMsg = "\x1b[1;31m[ERROR] Failed to stop API server.\x1b[0m";
                } else {
                    if (StartApiServer())
                        statusMsg = "\x1b[1;32m[OK] API server running at http://localhost:" +
                                    std::to_string(settings.apiPort) + "\x1b[0m";
                    else
                        statusMsg = "\x1b[1;31m[ERROR] Failed to start API server.\x1b[0m";
                }
                SaveSettings(settings);
                dirty = true;
            } else if (k == KEY_ENTER && settingsSel == S_SERVE_PORT) {
                // Type the port directly (raw input screen).
                if (ApiServerRunning()) {
                    statusMsg = "\x1b[1;33m[WARNING] Stop the API server before changing its port.\x1b[0m";
                    dirty = true;
                } else {
                    inputBuffer = std::to_string(settings.apiPort);
                    inputPurpose = IN_APIPORT;
                    screen = SC_INPUT;
                    dirty = true;
                }
            } else if (k == KEY_ENTER && settingsSel == S_RESET) {
                settings.hiddenDirs.clear();
                settings.servers.clear();
                settings.includeUdp = false;
                settings.showAllStates = false;
                settings.refreshSeconds = 2;
                SaveSettings(settings);
                statusMsg = "Settings reset.";
                needRefresh = true;
                dirty = true;
            }
            continue;
        }

        // ---- SC_LIST key handling ----
        if (searchMode) {
            if (k == KEY_ESC) {
                searchMode = false;
                searchBuffer = searchQuery;
                dirty = true;
            } else if (k == KEY_ENTER) {
                searchQuery = TrimWs(searchBuffer);
                searchMode = false;
                needRefresh = true;
                dirty = true;
            } else if (k == KEY_BACKSPACE) {
                if (!searchBuffer.empty()) searchBuffer.pop_back();
                dirty = true;
            } else if (k == KEY_CHAR) {
                searchBuffer += static_cast<char>(g_lastKeyChar);
                dirty = true;
            }
            continue;
        }
        if (k == KEY_SETTINGS) {
            settingsSel = 0;
            screen = SC_SETTINGS;
            dirty = true;
        } else if (k == KEY_QUIT || k == KEY_ESC) {
            break;
        } else if (k == KEY_CHAR && g_lastKeyChar == '/') {
            searchMode = true;
            searchBuffer = searchQuery;
            dirty = true;
        } else if (k == KEY_CHAR && g_lastKeyChar == 'c') {
            sortMode = (sortMode + 1) % 5;
            needRefresh = true;
            dirty = true;
        } else if (k == KEY_UP) {
            if (!rows.empty()) {
                if (selectedIdx > 0) selectedIdx--;
                else selectedIdx = static_cast<int>(rows.size()) - 1;
                dirty = true;
            }
        } else if (k == KEY_DOWN) {
            if (!rows.empty()) {
                if (selectedIdx + 1 < static_cast<int>(rows.size())) selectedIdx++;
                else selectedIdx = 0;
                dirty = true;
            }
        } else if ((k == KEY_OPEN || k == KEY_ENTER) && !rows.empty()) {
            unsigned port = rows[selectedIdx].port;
            std::string proto = (port == 443 || port == 8443) ? "https" : "http";
            if (OpenInBrowser(port)) {
                statusMsg = "\x1b[1;32m[OK] Browser opened: " + proto + "://localhost:" +
                            std::to_string(port) + "\x1b[0m";
            } else {
                statusMsg = "\x1b[1;31m[ERROR] Failed to open browser.\x1b[0m";
            }
            dirty = true;
        } else if (k == KEY_PING && !rows.empty()) {
            unsigned port = rows[selectedIdx].port;
            std::string res = PingPort(port);
            pingResults[port] = res;
            statusMsg = "Health check port " + std::to_string(port) + ": \x1b[1m" + res + "\x1b[0m";
            dirty = true;
        } else if (k == KEY_KILL && !rows.empty()) {
            if (rows[selectedIdx].pid > 0) {
                killTargetPid = rows[selectedIdx].pid;
                killTargetPort = rows[selectedIdx].port;
                ProcInfo pi = GetProcInfo(killTargetPid);
                killTargetName = pi.name.empty() ? "unknown" : pi.name;
                confirmKill = true;
            } else {
                statusMsg = "\x1b[1;33m[WARNING] Can't kill: unknown PID.\x1b[0m";
            }
            dirty = true;
        } else if (k == KEY_REFRESH) {
            statusMsg = "Port list refreshed.";
            pingResults.clear();
            needRefresh = true;
            dirty = true;
        }
    }

    Out("\x1b[?25h\x1b[2J\x1b[H");
}

// ===========================================================================
// main
// ===========================================================================
// ===========================================================================
// Internal API server (nxport serve) — lets external clients (browser /
// Postman) monitor ports and manage the server history.
// ===========================================================================
#ifdef _WIN32
static std::string JsonEscape(const std::string& s) {
    return EscapeJson(s);
}

// Build the /api/ports JSON (same data as the dashboard table).
static std::string BuildPortsJson(bool all, bool includeUdp) {
    std::vector<Row> rows;
    CollectRows(rows, all, includeUdp);
    FilterHiddenDirs(rows);
    FilterByText(rows);
    std::string out = "[";
    bool first = true;
    for (const auto& r : rows) {
        ProcInfo pi = GetProcInfo(r.pid);
        FillDirFallback(pi);
        ApplyIdeDirOverride(pi, r.port);
        std::string name = pi.name;
        if (name.empty() || name == "-") name = "system";
        std::string tag = DetectTechStack(pi.dir);
        if (!tag.empty()) name += " [" + tag + "]";
        if (!first) out += ",";
        first = false;
        out += "{\"proto\":\"" + r.proto + "\"";
        out += ",\"addr\":\"" + JsonEscape(r.addr) + "\"";
        out += ",\"port\":" + std::to_string(r.port);
        out += ",\"state\":\"" + JsonEscape(r.state) + "\"";
        out += ",\"pid\":" + std::to_string(r.pid);
        out += ",\"process\":\"" + JsonEscape(name) + "\"";
        out += ",\"dir\":\"" + JsonEscape(pi.dir) + "\"}";
    }
    out += "]";
    return out;
}

// Build the /api/servers JSON from the saved history.
static std::string BuildServersJson() {
    std::string out = "[";
    for (size_t i = 0; i < g_settings.servers.size(); ++i) {
        const ServerEntry& e = g_settings.servers[i];
        if (i) out += ",";
        out += "{\"name\":\"" + JsonEscape(e.name) + "\"";
        out += ",\"dir\":\"" + JsonEscape(e.dir) + "\"";
        out += ",\"cmd\":\"" + JsonEscape(e.cmd) + "\"";
        out += ",\"port\":" + std::to_string(e.port) + "}";
    }
    out += "]";
    return out;
}
#endif  // _WIN32

// Start/stop the internal API server (port 7900) as a detached process.
static bool ApiServerRunning() {
    return IsPortOpen(g_settings.apiPort);
}

static bool StartApiServer() {
    if (ApiServerRunning()) return true;  // already up
#ifdef _WIN32
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) return false;
    std::string cmdline = std::string("\"") + exe + "\" serve " +
                          std::to_string(g_settings.apiPort);
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                             &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // give it a moment to bind
    for (int i = 0; i < 20 && !ApiServerRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return ApiServerRunning();
#else
    return false;
#endif
}

static bool StopApiServer() {
    if (!ApiServerRunning()) return true;
#ifdef _WIN32
    std::vector<Row> rows;
    CollectRows(rows, true, false);
    bool killed = false;
    for (const auto& r : rows) {
        if (r.port == g_settings.apiPort && r.pid != 0) {
            ProcInfo pi = GetProcInfo(r.pid);
            // Only kill our own nxport serve instance, not an unrelated process.
            if (pi.name.find("nxport") != std::string::npos && TerminateProc(r.pid))
                killed = true;
        }
    }
    return killed;
#else
    return false;
#endif
}

// Foreground API server loop (blocks; Ctrl+C to stop).
static void RunApiServer(unsigned apiPort) {
#ifdef _WIN32
    Out("nxport serve: listening on http://localhost:" + std::to_string(apiPort) +
        "  (Ctrl+C to stop)\n");
    Out("  GET /health       - service info\n");
    Out("  GET /api/ports    - live port list (JSON)\n");
    Out("  GET /api/servers  - server history (JSON)\n");
    Out("  GET /api/stats    - CPU/RAM/Disk/Network (JSON)\n");
    Out("  GET /api/start/<port>  - start a saved server\n");
    Out("  GET /api/stop/<port>   - stop a saved server\n");
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        Out("Error: socket failed\n");
        return;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(apiPort));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, 16) != 0) {
        Out("Error: cannot bind port " + std::to_string(apiPort) +
            " (in use?)\n");
        return;
    }
    for (;;) {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        char req[8192] = {0};
        int n = recv(c, req, sizeof(req) - 1, 0);
        std::string body = "Not Found";
        std::string status = "404 Not Found";
        if (n > 0) {
            std::string request(req);
            std::string method = request.substr(0, request.find(' '));
            std::string path = request.substr(request.find(' ') + 1);
            path = path.substr(0, path.find(' '));
            size_t q = path.find('?');
            if (q != std::string::npos) path = path.substr(0, q);
            if (path == "/api/ports") {
                body = BuildPortsJson(g_settings.showAllStates, g_settings.includeUdp);
                status = "200 OK";
            } else if (path == "/api/servers") {
                body = BuildServersJson();
                status = "200 OK";
            } else if (path.rfind("/api/start/", 0) == 0) {
                unsigned port = static_cast<unsigned>(
                    strtoul(path.c_str() + 11, nullptr, 10));
                bool started = false;
                for (const auto& e : g_settings.servers) {
                    if (e.port == port) { started = StartServer(e); break; }
                }
                body = started ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not found or failed\"}";
                status = started ? "200 OK" : "400 Bad Request";
            } else if (path.rfind("/api/stop/", 0) == 0) {
                unsigned port = static_cast<unsigned>(
                    strtoul(path.c_str() + 11, nullptr, 10));
                int killed = 0;
                for (const auto& e : g_settings.servers) {
                    if (e.port == port) { killed = StopServer(e); break; }
                }
                body = killed > 0 ? "{\"ok\":true,\"killed\":" + std::to_string(killed) + "}"
                                  : "{\"ok\":false,\"error\":\"nothing stopped\"}";
                status = killed > 0 ? "200 OK" : "400 Bad Request";
            } else if (path == "/api/stats") {
                SysStats st = SampleSysStats();
                body = "{\"cpu\":" + std::to_string(st.cpu) +
                       ",\"ram\":" + std::to_string(st.ram) +
                       ",\"disk\":" + std::to_string(st.disk) +
                       ",\"netBps\":" + std::to_string(st.netBps) + "}";
                status = "200 OK";
            } else if (path == "/" || path == "/health") {
                body = "{\"ok\":true,\"service\":\"nxport\",\"version\":\"" NXPORT_VERSION "\"}";
                status = "200 OK";
            }
        }
        std::string resp = "HTTP/1.1 " + status + "\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                           "Connection: close\r\n\r\n" + body;
        send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(c);
    }
#else
    (void)apiPort;
    Out("nxport serve: not implemented on this platform yet\n");
#endif
}

int main(int argc, char** argv) {    bool all = false;
    bool includeUdp = false;
    bool filterPort = false;
    bool isKill = false;
    bool forceKill = false;
    bool jsonOutput = false;
    bool csvOutput = false;
    bool yamlOutput = false;
    bool pingCheck = false;
    bool isWatch = false;
    bool onceMode = false;
    bool isWait = false;
    bool isFind = false;
    bool isServe = false;
    unsigned servePort = 7900;
    unsigned waitPort = 0;
    unsigned findStart = 3000;
    int findCount = 1;
    int waitTimeout = 60;  // seconds; 0 = wait forever
    int watchInterval = 1;
    unsigned startPort = 0;
    unsigned endPort = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            PrintHelp();
            return 0;
        } else if (a == "-V" || a == "--version") {
            Out(std::string("nxport ") + NXPORT_VERSION + "\n");
            return 0;
        } else if (a == "kill" || a == "-k") {
            isKill = true;
            if (i + 1 < argc) {
                char* end = nullptr;
                unsigned long p = strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0') {
                    startPort = static_cast<unsigned>(p);
                    endPort = startPort;
                    i++;
                }
            }
        } else if (a == "serve" || a == "--serve") {
            isServe = true;
            if (i + 1 < argc) {
                char* end = nullptr;
                unsigned long p = strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0' && p > 0 && p <= 65535) {
                    servePort = static_cast<unsigned>(p);
                    i++;
                }
            }
        } else if (a == "wait" || a == "--wait") {
            isWait = true;
            if (i + 1 < argc) {
                char* end = nullptr;
                unsigned long p = strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0') {
                    waitPort = static_cast<unsigned>(p);
                    i++;
                }
            }
        } else if (a == "find" || a == "--find") {
            isFind = true;
            if (i + 1 < argc) {
                char* end = nullptr;
                unsigned long p = strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0') {
                    findStart = static_cast<unsigned>(p);
                    i++;
                }
            }
        } else if (a == "--timeout") {
            if (i + 1 < argc) waitTimeout = atoi(argv[++i]);
        } else if (a == "--count") {
            if (i + 1 < argc) findCount = (std::max)(1, atoi(argv[++i]));
        } else if (a == "-f" || a == "--force") {
            forceKill = true;
        } else if (a == "-u" || a == "--udp") {
            includeUdp = true;
        } else if (a == "-p" || a == "--ping") {
            pingCheck = true;
        } else if (a == "-a" || a == "--all") {
            all = true;
        } else if (a == "--json") {
            jsonOutput = true;
        } else if (a == "--csv") {
            csvOutput = true;
        } else if (a == "--yaml") {
            yamlOutput = true;
        } else if (a == "--color") {
            g_colorMode = 1;
        } else if (a == "--no-color") {
            g_colorMode = -1;
        } else if (a == "--once" || a == "--static") {
            onceMode = true;
        } else if (a == "-w" || a == "--watch") {
            isWatch = true;
            if (i + 1 < argc) {
                char* end = nullptr;
                unsigned long w = strtoul(argv[i + 1], &end, 10);
                if (end && *end == '\0') {
                    watchInterval = (std::max)(1, static_cast<int>(w));
                    i++;
                }
            }
        } else {
            size_t dash = a.find('-');
            size_t dots = a.find("..");
            bool matched = false;
            if (dash != std::string::npos && dash > 0 && dash + 1 < a.size()) {
                startPort = static_cast<unsigned>(strtoul(a.substr(0, dash).c_str(), nullptr, 10));
                endPort = static_cast<unsigned>(strtoul(a.substr(dash + 1).c_str(), nullptr, 10));
                if (startPort > 0 && endPort >= startPort) {
                    filterPort = true;
                    matched = true;
                }
            } else if (dots != std::string::npos && dots > 0 && dots + 2 < a.size()) {
                startPort = static_cast<unsigned>(strtoul(a.substr(0, dots).c_str(), nullptr, 10));
                endPort = static_cast<unsigned>(strtoul(a.substr(dots + 2).c_str(), nullptr, 10));
                if (startPort > 0 && endPort >= startPort) {
                    filterPort = true;
                    matched = true;
                }
            } else {
                char* end = nullptr;
                unsigned long p = strtoul(a.c_str(), &end, 10);
                if (end && *end == '\0') {
                    filterPort = true;
                    startPort = static_cast<unsigned>(p);
                    endPort = startPort;
                    matched = true;
                }
            }
            // Bare word: smart fuzzy search by process name or Dir.
            if (!matched && !a.empty() && a[0] != '-') {
                g_textFilter = a;
            }
        }
    }

    PlatformInit();

    LoadSettings(g_settings);
    LoadNxCodeServerDirs();
    if (includeUdp) g_settings.includeUdp = true;
    if (all) g_settings.showAllStates = true;

    if (isKill) {
        if (startPort == 0) {
            Out("Error: please specify a port to kill, e.g. 'nxport kill 8080'\n");
            PlatformShutdown();
            return 1;
        }
        int rc = ExecuteKill(startPort, forceKill);
        PlatformShutdown();
        return rc;
    }

    if (isServe) {
        RunApiServer(servePort);
        PlatformShutdown();
        return 0;
    }

    if (isWait) {
        if (waitPort == 0) {
            Out("Error: please specify a port to wait for, e.g. 'nxport wait 3000'\n");
            PlatformShutdown();
            return 1;
        }
        Out("nxport: waiting for port " + std::to_string(waitPort) + " ...\n");
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (IsPortOpen(waitPort)) {
                Out("nxport: port " + std::to_string(waitPort) + " is up.\n");
                PlatformShutdown();
                return 0;
            }
            if (waitTimeout > 0) {
                long secs = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
                if (secs >= waitTimeout) {
                    Out("nxport: timeout after " + std::to_string(waitTimeout) +
                        "s waiting for port " + std::to_string(waitPort) + ".\n");
                    PlatformShutdown();
                    return 1;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    if (isFind) {
        unsigned p = findStart == 0 ? 3000 : findStart;
        std::string result;
        int found = 0;
        for (unsigned tries = 0; tries < 4096 && found < findCount; ++tries, ++p) {
            if (p == 0 || p > 65535) break;
            if (IsPortFree(p)) {
                result += std::to_string(p) + "\n";
                found++;
            }
        }
        if (found == 0) {
            Out("nxport: no free port found from " + std::to_string(findStart) + ".\n");
            PlatformShutdown();
            return 1;
        }
        Out(result);
        PlatformShutdown();
        return 0;
    }

    // Default mode in interactive terminal: Live Real-Time Dashboard (stays open)!
    bool isInteractive = !onceMode && !isWatch && !jsonOutput && !csvOutput && !yamlOutput &&
                         IsColorEnabled();
    if (isInteractive) {
        RunDashboard(all, includeUdp, filterPort, startPort, endPort);
    } else if (isWatch) {
        for (;;) {
            if (IsColorEnabled()) {
                Out("\x1b[2J\x1b[H");
            } else {
                Out("\n--- nxport watch ---\n");
            }
            time_t now = time(nullptr);
            tm* lt = localtime(&now);
            char timeBuf[64] = {0};
            if (lt) strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", lt);
            Out("nxport --watch (" + std::to_string(watchInterval) + "s, Ctrl+C to stop) - [" +
                std::string(timeBuf) + "]\n\n");

            RenderOutput(all, includeUdp, filterPort, startPort, endPort, false, false, false,
                         pingCheck);
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(watchInterval));
        }
    } else {
        RenderOutput(all, includeUdp, filterPort, startPort, endPort, jsonOutput, csvOutput,
                     yamlOutput, pingCheck);
        fflush(stdout);
    }

    PlatformShutdown();
    return 0;
}
