// Minimal, test-only stand-in for the three real Windows SDK headers this SDK's
// Windows edge includes (<winsock2.h>, <ws2tcpip.h>, <windows.h>), none of which exists
// on a macOS or Linux host.
//
// This file exists SOLELY so that
//
//   clang++ -std=c++17 -fsyntax-only \
//     -DPD_FORCE_WINDOWS_PLATFORM -DPD_WINDOWS_SYNTAX_CHECK \
//     -include core/tests/win32_stub.h ...
//
// can parse and type-check core/src/transport_win.cpp, core/src/platform_file_win.cpp,
// core/include/printerdriver/net_platform.hpp and the Windows branches of
// core/tests/fake_printer.hpp against the real pd.h/core headers on a plain host
// compiler. scripts/check_windows_syntax.sh is the exact command, and it runs a
// negative control that seeds a deliberate type error and requires the check to fail —
// so "the check passed" is a statement with something behind it.
//
// It is a SYNTAX AND TYPE CHECK FIXTURE, NOT an implementation:
//   - every function below is declared and never defined (fine for -fsyntax-only,
//     which never reaches the linker);
//   - the signatures mirror the real Win32/Winsock ones closely enough to catch real
//     mistakes — SOCKET is UINT_PTR (so storing one in an int is a type error here,
//     exactly as it is on Windows), HANDLE is void*, setsockopt/getsockopt take char*
//     buffers, send/recv take int lengths and return int, and the addrlen parameters
//     are int rather than POSIX's socklen_t;
//   - struct layouts are NOT ABI-compatible with anything, and the constant values are
//     placeholders except where a real value is load-bearing for a `constexpr` here;
//   - it is never compiled into anything shipped. A real Windows build never defines
//     PD_WINDOWS_SYNTAX_CHECK, so it always includes the genuine SDK headers.
//
// Keep this in sync with whatever subset of Win32/Winsock the Windows edge actually
// calls; it only needs to cover that subset. If a new call is added there and not here,
// the syntax check fails loudly, which is the intended failure mode.

#pragma once

#if defined(_WIN32)
#error "win32_stub.h is a host-side fixture; a real Windows build uses the genuine SDK headers"
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdint>

// -- Four names the host already owns -------------------------------------------------
//
// This file is force-included ahead of everything, but the C++ standard library then
// drags in the host's <sys/types.h> and <sys/resource.h> regardless, and those define
// `timeval`, `fd_set`, the FD_* macros and `select` — the same spellings Winsock uses.
// Defining them here as well is a redefinition error, so the stub takes the host's for
// exactly those four and supplies everything else itself.
//
// The honest cost: for select() the *layout and macro semantics* being checked are the
// host's, not Winsock's, so this fixture cannot catch a mistake that only a real fd_set
// would reject. It still checks the call shape, and every other Winsock and Win32
// signature below is the Windows one. (The byte-order calls htonl/ntohs are not stubbed
// at all and are not used by the Windows edge: they are macros on some hosts, so
// pd::net has its own endian-independent helpers instead — see net_platform.hpp.)
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

// --- <windows.h> scalar types ------------------------------------------------------

using BOOL = int;
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = unsigned long;
using LONG = long;
using ULONG = unsigned long;
using UINT_PTR = std::uintptr_t;
using LPVOID = void*;
using LPCVOID = const void*;
using LPSTR = char*;
using LPCSTR = const char*;
using HANDLE = void*;
using HLOCAL = void*;
using HMODULE = void*;
using LPDWORD = DWORD*;
using u_long = unsigned long;
using u_short = unsigned short;
using u_int = unsigned int;

struct _SECURITY_ATTRIBUTES;
using LPSECURITY_ATTRIBUTES = _SECURITY_ATTRIBUTES*;
struct _OVERLAPPED;
using LPOVERLAPPED = _OVERLAPPED*;

#define MAKEWORD(low, high) \
  ((WORD)(((BYTE)((low) & 0xff)) | ((WORD)((BYTE)((high) & 0xff))) << 8))

#define MAX_PATH 260

// --- <winsock2.h> ------------------------------------------------------------------

using SOCKET = UINT_PTR;

#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR (-1)

constexpr int AF_UNSPEC = 0;
constexpr int AF_INET = 2;
constexpr int AF_INET6 = 23;
constexpr int SOCK_STREAM = 1;
// Discovery's local-subnet probe opens a connected UDP socket to ask the routing table
// which source address a remote one would use; it never transmits (core/src/discovery.cpp).
constexpr int SOCK_DGRAM = 2;
constexpr int IPPROTO_TCP = 6;
constexpr int SOL_SOCKET = 0xffff;
constexpr int SO_ERROR = 0x1007;
constexpr int SO_REUSEADDR = 0x0004;
constexpr int TCP_NODELAY = 0x0001;
constexpr int SD_BOTH = 2;
constexpr long FIONBIO = 0x8004667e;

constexpr int WSAEINTR = 10004;
constexpr int WSAEINPROGRESS = 10036;
constexpr int WSAEWOULDBLOCK = 10035;

struct WSAData {
  WORD wVersion;
  WORD wHighVersion;
  char szDescription[257];
  char szSystemStatus[129];
};
using WSADATA = WSAData;

int WSAStartup(WORD version, WSADATA* data);
int WSACleanup(void);
int WSAGetLastError(void);

struct in_addr {
  ULONG s_addr;
};

struct sockaddr {
  unsigned short sa_family;
  char sa_data[14];
};

struct sockaddr_in {
  short sin_family;
  unsigned short sin_port;
  in_addr sin_addr;
  char sin_zero[8];
};

// `timeval`, `fd_set`, FD_ZERO/FD_SET/FD_ISSET and `select` are the host's — see the
// note at the top of this file. Everything from here down is the Winsock signature.

SOCKET socket(int family, int type, int protocol);
int closesocket(SOCKET socket);
int shutdown(SOCKET socket, int how);
int ioctlsocket(SOCKET socket, long command, u_long* argument);
int setsockopt(SOCKET socket, int level, int option, const char* value, int length);
int getsockopt(SOCKET socket, int level, int option, char* value, int* length);
int send(SOCKET socket, const char* buffer, int length, int flags);
int recv(SOCKET socket, char* buffer, int length, int flags);
int connect(SOCKET socket, const sockaddr* name, int length);
int bind(SOCKET socket, const sockaddr* name, int length);
int listen(SOCKET socket, int backlog);
SOCKET accept(SOCKET socket, sockaddr* address, int* length);
int getsockname(SOCKET socket, sockaddr* name, int* length);

// --- <ws2tcpip.h> ------------------------------------------------------------------

struct addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  // size_t here rather than POSIX's socklen_t, which is what makes the casts in
  // transport_win.cpp load-bearing rather than decorative.
  std::size_t ai_addrlen;
  char* ai_canonname;
  sockaddr* ai_addr;
  addrinfo* ai_next;
};

int getaddrinfo(const char* node, const char* service, const addrinfo* hints,
                addrinfo** result);
void freeaddrinfo(addrinfo* result);

// --- <windows.h> functions ---------------------------------------------------------

constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x00000100;
constexpr DWORD FORMAT_MESSAGE_IGNORE_INSERTS = 0x00000200;
constexpr DWORD FORMAT_MESSAGE_FROM_SYSTEM = 0x00001000;

constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD ERROR_ALREADY_EXISTS = 183;

constexpr DWORD GENERIC_WRITE = 0x40000000;
constexpr DWORD FILE_APPEND_DATA = 0x0004;
constexpr DWORD FILE_SHARE_READ = 0x00000001;
constexpr DWORD CREATE_ALWAYS = 2;
constexpr DWORD OPEN_ALWAYS = 4;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x00000080;

constexpr DWORD REPLACEFILE_IGNORE_MERGE_ERRORS = 0x00000002;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x00000001;
constexpr DWORD MOVEFILE_WRITE_THROUGH = 0x00000008;

#define INVALID_HANDLE_VALUE ((HANDLE)(std::intptr_t)-1)

DWORD GetLastError(void);
DWORD GetCurrentProcessId(void);
DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD message_id, DWORD language_id,
                     LPSTR buffer, DWORD size, va_list* arguments);
HLOCAL LocalFree(HLOCAL memory);

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                   DWORD disposition, DWORD flags, HANDLE templates);
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD to_write, LPDWORD written,
               LPOVERLAPPED overlapped);
BOOL FlushFileBuffers(HANDLE file);
BOOL CloseHandle(HANDLE object);
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL ReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags,
                  LPVOID exclude, LPVOID reserved);

struct WIN32_FIND_DATAA {
  DWORD dwFileAttributes;
  DWORD nFileSizeHigh;
  DWORD nFileSizeLow;
  char cFileName[MAX_PATH];
  char cAlternateFileName[14];
};
using LPWIN32_FIND_DATAA = WIN32_FIND_DATAA*;

HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL FindNextFileA(HANDLE search, LPWIN32_FIND_DATAA data);
BOOL FindClose(HANDLE search);
