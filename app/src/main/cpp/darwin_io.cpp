#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <utime.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "darwin_shim.h"
#include "engine.h"

namespace {

struct FlagPair {
    int darwin, linux_value;
};

constexpr FlagPair kOpenFlags[] = {{0x4, O_NONBLOCK}, {0x8, O_APPEND}, {0x100, O_NOFOLLOW}, {0x200, O_CREAT}, {0x400, O_TRUNC}, {0x800, O_EXCL}, {0x20000, O_NOCTTY}, {0x100000, O_DIRECTORY}, {0x1000000, O_CLOEXEC}};
constexpr int kAccessModeMask = 3;

int open_flags_to_linux(int flags) {
    int out = flags & kAccessModeMask;
    for (const auto& pair : kOpenFlags) {
        if (flags & pair.darwin) out |= pair.linux_value;
    }
    return out;
}

int open_flags_to_darwin(int flags) {
    int out = flags & kAccessModeMask;
    for (const auto& pair : kOpenFlags) {
        if (flags & pair.linux_value) out |= pair.darwin;
    }
    return out;
}

struct DarwinStat {
    int32_t dev;
    uint16_t mode, nlink;
    uint64_t ino;
    uint32_t uid, gid;
    int32_t rdev;
    int32_t padding;
    timespec atime, mtime, ctime, birthtime;
    int64_t size, blocks;
    int32_t blksize;
    uint32_t flags, gen;
    int32_t spare;
    int64_t reserved[2];
};
static_assert(sizeof(DarwinStat) == 144);

void fill_stat(const struct stat& in, DarwinStat* out) {
    memset(out, 0, sizeof *out);
    out->dev = static_cast<int32_t>(in.st_dev);
    out->mode = static_cast<uint16_t>(in.st_mode);
    out->nlink = static_cast<uint16_t>(in.st_nlink);
    out->ino = in.st_ino;
    out->uid = in.st_uid;
    out->gid = in.st_gid;
    out->rdev = static_cast<int32_t>(in.st_rdev);
    out->atime = in.st_atim;
    out->mtime = in.st_mtim;
    out->ctime = in.st_ctim;
    out->birthtime = in.st_ctim;
    out->size = in.st_size;
    out->blocks = in.st_blocks;
    out->blksize = static_cast<int32_t>(in.st_blksize);
}

int darwin_stat(const char* path, DarwinStat* out) {
    struct stat in{};
    const int result = stat(path, &in);
    if (result == 0) fill_stat(in, out);
    return result;
}

int darwin_lstat(const char* path, DarwinStat* out) {
    struct stat in{};
    const int result = lstat(path, &in);
    if (result == 0) fill_stat(in, out);
    return result;
}

int darwin_fstat(int fd, DarwinStat* out) {
    struct stat in{};
    const int result = fstat(fd, &in);
    if (result == 0) fill_stat(in, out);
    return result;
}

struct DarwinDirent {
    uint64_t ino, seek_offset;
    uint16_t record_length, name_length;
    uint8_t type;
    char name[1024];
};

void fill_dirent(const dirent& in, DarwinDirent* out) {
    out->ino = in.d_ino;
    out->seek_offset = static_cast<uint64_t>(in.d_off);
    out->name_length = static_cast<uint16_t>(strnlen(in.d_name, sizeof in.d_name));
    out->record_length = sizeof(DarwinDirent);
    out->type = in.d_type;
    memcpy(out->name, in.d_name, out->name_length);
    out->name[out->name_length] = '\0';
}

DarwinDirent* darwin_readdir(DIR* directory) {
    static thread_local DarwinDirent entry;
    const dirent* in = readdir(directory);
    if (in == nullptr) return nullptr;
    fill_dirent(*in, &entry);
    return &entry;
}

int darwin_readdir_r(DIR* directory, DarwinDirent* entry, DarwinDirent** result) {
    errno = 0;
    const dirent* in = readdir(directory);
    if (in == nullptr) {
        *result = nullptr;
        return errno;
    }
    fill_dirent(*in, entry);
    *result = entry;
    return 0;
}

void* darwin_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    constexpr int kDarwinFixed = 0x10, kDarwinNoReserve = 0x40, kDarwinAnonymous = 0x1000, kSharingMask = 3;
    int out = flags & kSharingMask;
    if (flags & kDarwinFixed) out |= MAP_FIXED;
    if (flags & kDarwinNoReserve) out |= MAP_NORESERVE;
    if (flags & kDarwinAnonymous) out |= MAP_ANONYMOUS;
    return mmap(address, length, protection, out, flags & kDarwinAnonymous ? -1 : fd, offset);
}

int darwin_madvise(void* address, size_t length, int advice) {
    constexpr int kDarwinFree = 5;
    return madvise(address, length, advice == kDarwinFree ? MADV_FREE : advice);
}

constexpr int kDarwinInet6 = 30;

int family_to_linux(int family) { return family == kDarwinInet6 ? AF_INET6 : family; }
int family_to_darwin(int family) { return family == AF_INET6 ? kDarwinInet6 : family; }

// BSD sockaddrs start with a length byte, then a one-byte family; Linux has a two-byte family there.
sockaddr_storage address_to_linux(const void* raw, socklen_t length) {
    sockaddr_storage out{};
    if (raw == nullptr || length < 2) return out;
    memcpy(&out, raw, length < sizeof out ? length : sizeof out);
    out.ss_family = static_cast<sa_family_t>(family_to_linux(static_cast<const uint8_t*>(raw)[1]));
    return out;
}

void address_to_darwin(const sockaddr_storage& in, socklen_t in_length, void* out, socklen_t* out_length) {
    if (out == nullptr || out_length == nullptr) return;
    socklen_t length = in_length < *out_length ? in_length : *out_length;
    memcpy(out, &in, length);
    if (length >= 2) {
        static_cast<uint8_t*>(out)[0] = static_cast<uint8_t>(in_length);
        static_cast<uint8_t*>(out)[1] = static_cast<uint8_t>(family_to_darwin(in.ss_family));
    }
    *out_length = in_length;
}

socklen_t linux_address_length(const sockaddr_storage& address, socklen_t given) {
    return address.ss_family == AF_UNIX ? given : address.ss_family == AF_INET ? sizeof(sockaddr_in)
                                              : address.ss_family == AF_INET6  ? sizeof(sockaddr_in6)
                                                                               : given;
}

int darwin_socket(int family, int type, int protocol) { return socket(family_to_linux(family), type, protocol); }
int darwin_socketpair(int family, int type, int protocol, int* pair) { return socketpair(family_to_linux(family), type, protocol, pair); }

int darwin_bind(int fd, const void* address, socklen_t length) {
    const auto out = address_to_linux(address, length);
    return bind(fd, reinterpret_cast<const sockaddr*>(&out), linux_address_length(out, length));
}

int darwin_connect(int fd, const void* address, socklen_t length) {
    const auto out = address_to_linux(address, length);
    return connect(fd, reinterpret_cast<const sockaddr*>(&out), linux_address_length(out, length));
}

int darwin_accept(int fd, void* address, socklen_t* length) {
    sockaddr_storage in{};
    socklen_t in_length = sizeof in;
    const int result = accept(fd, reinterpret_cast<sockaddr*>(&in), &in_length);
    if (result >= 0) address_to_darwin(in, in_length, address, length);
    return result;
}

int darwin_getsockname(int fd, void* address, socklen_t* length) {
    sockaddr_storage in{};
    socklen_t in_length = sizeof in;
    const int result = getsockname(fd, reinterpret_cast<sockaddr*>(&in), &in_length);
    if (result == 0) address_to_darwin(in, in_length, address, length);
    return result;
}

int darwin_getpeername(int fd, void* address, socklen_t* length) {
    sockaddr_storage in{};
    socklen_t in_length = sizeof in;
    const int result = getpeername(fd, reinterpret_cast<sockaddr*>(&in), &in_length);
    if (result == 0) address_to_darwin(in, in_length, address, length);
    return result;
}

int message_flags_to_linux(int flags) {
    constexpr int kDarwinOutOfBand = 1, kDarwinPeek = 2, kDarwinWaitAll = 0x40, kDarwinDontWait = 0x80;
    int out = MSG_NOSIGNAL;
    if (flags & kDarwinOutOfBand) out |= MSG_OOB;
    if (flags & kDarwinPeek) out |= MSG_PEEK;
    if (flags & kDarwinWaitAll) out |= MSG_WAITALL;
    if (flags & kDarwinDontWait) out |= MSG_DONTWAIT;
    return out;
}

ssize_t darwin_send(int fd, const void* data, size_t size, int flags) { return send(fd, data, size, message_flags_to_linux(flags)); }
ssize_t darwin_recv(int fd, void* data, size_t size, int flags) { return recv(fd, data, size, message_flags_to_linux(flags) & ~MSG_NOSIGNAL); }

ssize_t darwin_sendto(int fd, const void* data, size_t size, int flags, const void* address, socklen_t length) {
    if (address == nullptr) return send(fd, data, size, message_flags_to_linux(flags));
    const auto out = address_to_linux(address, length);
    return sendto(fd, data, size, message_flags_to_linux(flags), reinterpret_cast<const sockaddr*>(&out), linux_address_length(out, length));
}

ssize_t darwin_recvfrom(int fd, void* data, size_t size, int flags, void* address, socklen_t* length) {
    sockaddr_storage in{};
    socklen_t in_length = sizeof in;
    const ssize_t result = recvfrom(fd, data, size, message_flags_to_linux(flags) & ~MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&in), &in_length);
    if (result >= 0 && address != nullptr) address_to_darwin(in, in_length, address, length);
    return result;
}

struct DarwinMessage {
    void* name;
    socklen_t name_length;
    iovec* vectors;
    int32_t vector_count;
    void* control;
    socklen_t control_length;
    int32_t flags;
};

ssize_t darwin_sendmsg(int fd, const DarwinMessage* in, int flags) {
    sockaddr_storage address = address_to_linux(in->name, in->name_length);
    msghdr out{};
    if (in->name != nullptr) {
        out.msg_name = &address;
        out.msg_namelen = linux_address_length(address, in->name_length);
    }
    out.msg_iov = in->vectors;
    out.msg_iovlen = static_cast<size_t>(in->vector_count);
    return sendmsg(fd, &out, message_flags_to_linux(flags));
}

ssize_t darwin_recvmsg(int fd, DarwinMessage* in, int flags) {
    sockaddr_storage address{};
    msghdr out{};
    if (in->name != nullptr) {
        out.msg_name = &address;
        out.msg_namelen = sizeof address;
    }
    out.msg_iov = in->vectors;
    out.msg_iovlen = static_cast<size_t>(in->vector_count);
    const ssize_t result = recvmsg(fd, &out, message_flags_to_linux(flags) & ~MSG_NOSIGNAL);
    if (result >= 0) {
        if (in->name != nullptr) address_to_darwin(address, out.msg_namelen, in->name, &in->name_length);
        in->control_length = 0;
        in->flags = 0;
    }
    return result;
}

constexpr int kDarwinSolSocket = 0xffff;
constexpr int kDarwinNoSigPipe = 0x1022;
constexpr int kDarwinIpv6Only = 27;
constexpr int kDarwinTcpKeepAlive = 0x10;
constexpr FlagPair kSocketOptions[] = {{0x4, SO_REUSEADDR}, {0x8, SO_KEEPALIVE}, {0x20, SO_BROADCAST}, {0x80, SO_LINGER}, {0x200, SO_REUSEPORT}, {0x1001, SO_SNDBUF}, {0x1002, SO_RCVBUF}, {0x1005, SO_SNDTIMEO}, {0x1006, SO_RCVTIMEO}, {0x1007, SO_ERROR}, {0x1008, SO_TYPE}};

std::pair<int, int> socket_option_to_linux(int level, int name) {
    if (level == kDarwinSolSocket) {
        for (const auto& pair : kSocketOptions) {
            if (pair.darwin == name) return {SOL_SOCKET, pair.linux_value};
        }
        return {SOL_SOCKET, -1};
    }
    if (level == IPPROTO_IPV6 && name == kDarwinIpv6Only) return {level, IPV6_V6ONLY};
    if (level == IPPROTO_TCP && name == kDarwinTcpKeepAlive) return {level, TCP_KEEPIDLE};
    return {level, name};
}

int darwin_setsockopt(int fd, int level, int name, const void* value, socklen_t length) {
    // Sends here pass MSG_NOSIGNAL, which gives the same behaviour.
    if (level == kDarwinSolSocket && name == kDarwinNoSigPipe) return 0;
    const auto [linux_level, linux_name] = socket_option_to_linux(level, name);
    if (linux_name < 0) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return setsockopt(fd, linux_level, linux_name, value, length);
}

int darwin_getsockopt(int fd, int level, int name, void* value, socklen_t* length) {
    if (level == kDarwinSolSocket && name == kDarwinNoSigPipe) {
        memset(value, 0, *length);
        return 0;
    }
    const auto [linux_level, linux_name] = socket_option_to_linux(level, name);
    if (linux_name < 0) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return getsockopt(fd, linux_level, linux_name, value, length);
}

struct DarwinAddrInfo {
    int flags, family, socket_type, protocol;
    socklen_t address_length;
    char* canonical_name;
    void* address;
    DarwinAddrInfo* next;
};

int darwin_getaddrinfo(const char* host, const char* service, const DarwinAddrInfo* hints, DarwinAddrInfo** result) {
    addrinfo linux_hints{};
    if (hints != nullptr) {
        linux_hints.ai_flags = hints->flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        linux_hints.ai_family = family_to_linux(hints->family);
        linux_hints.ai_socktype = hints->socket_type;
        linux_hints.ai_protocol = hints->protocol;
    }
    addrinfo* found = nullptr;
    const int status = getaddrinfo(host, service, hints != nullptr ? &linux_hints : nullptr, &found);
    if (status != 0) return status;
    DarwinAddrInfo** tail = result;
    *result = nullptr;
    for (const addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
        auto* out = static_cast<DarwinAddrInfo*>(calloc(1, sizeof(DarwinAddrInfo)));
        out->flags = entry->ai_flags;
        out->family = family_to_darwin(entry->ai_family);
        out->socket_type = entry->ai_socktype;
        out->protocol = entry->ai_protocol;
        out->address_length = entry->ai_addrlen;
        out->canonical_name = entry->ai_canonname != nullptr ? strdup(entry->ai_canonname) : nullptr;
        out->address = calloc(1, entry->ai_addrlen);
        sockaddr_storage address{};
        memcpy(&address, entry->ai_addr, entry->ai_addrlen);
        socklen_t length = entry->ai_addrlen;
        address_to_darwin(address, entry->ai_addrlen, out->address, &length);
        *tail = out;
        tail = &out->next;
    }
    freeaddrinfo(found);
    return 0;
}

void darwin_freeaddrinfo(DarwinAddrInfo* list) {
    while (list != nullptr) {
        DarwinAddrInfo* next = list->next;
        free(list->canonical_name);
        free(list->address);
        free(list);
        list = next;
    }
}

int darwin_getnameinfo(const void* address, socklen_t length, char* host, socklen_t host_length, char* service, socklen_t service_length, int flags) {
    constexpr int kDarwinNumericHost = 2, kDarwinNumericService = 8;
    int out_flags = 0;
    if (flags & kDarwinNumericHost) out_flags |= NI_NUMERICHOST;
    if (flags & kDarwinNumericService) out_flags |= NI_NUMERICSERV;
    const auto out = address_to_linux(address, length);
    return getnameinfo(reinterpret_cast<const sockaddr*>(&out), linux_address_length(out, length), host, host_length, service, service_length, out_flags);
}

void* copy_address_to_darwin(const sockaddr* in) {
    if (in == nullptr) return nullptr;
    const socklen_t length = in->sa_family == AF_INET ? sizeof(sockaddr_in) : in->sa_family == AF_INET6 ? sizeof(sockaddr_in6)
                                                                                                        : sizeof(sockaddr);
    sockaddr_storage address{};
    memcpy(&address, in, length);
    void* out = calloc(1, length);
    socklen_t out_length = length;
    address_to_darwin(address, length, out, &out_length);
    return out;
}

// The chain's layout matches Darwin's; only the addresses it points at need converting.
int darwin_getifaddrs(ifaddrs** result) {
    ifaddrs* found = nullptr;
    const int status = getifaddrs(&found);
    if (status != 0) return status;
    ifaddrs** tail = result;
    *result = nullptr;
    for (const ifaddrs* entry = found; entry != nullptr; entry = entry->ifa_next) {
        auto* out = static_cast<ifaddrs*>(calloc(1, sizeof(ifaddrs)));
        out->ifa_name = strdup(entry->ifa_name);
        out->ifa_flags = entry->ifa_flags;
        out->ifa_addr = static_cast<sockaddr*>(copy_address_to_darwin(entry->ifa_addr));
        out->ifa_netmask = static_cast<sockaddr*>(copy_address_to_darwin(entry->ifa_netmask));
        out->ifa_broadaddr = static_cast<sockaddr*>(copy_address_to_darwin(entry->ifa_broadaddr));
        *tail = out;
        tail = &out->ifa_next;
    }
    freeifaddrs(found);
    return 0;
}

void darwin_freeifaddrs(ifaddrs* list) {
    while (list != nullptr) {
        ifaddrs* next = list->ifa_next;
        free(list->ifa_name);
        free(list->ifa_addr);
        free(list->ifa_netmask);
        free(list->ifa_broadaddr);
        free(list);
        list = next;
    }
}

const char* darwin_inet_ntop(int family, const void* address, char* out, socklen_t size) { return inet_ntop(family_to_linux(family), address, out, size); }
int darwin_inet_pton(int family, const char* text, void* out) { return inet_pton(family_to_linux(family), text, out); }

struct DarwinFlock {
    off_t start, length;
    pid_t pid;
    short type, whence;
};

int linux_fcntl(int fd, int command, uint64_t argument) {
    constexpr int kGetFd = 1, kSetFd = 2, kGetFl = 3, kSetFl = 4, kDupFd = 0, kGetLk = 7, kSetLk = 8, kSetLkW = 9, kDupFdCloexec = 67, kNoCache = 48, kGetPath = 50, kFullFsync = 51, kSetNoSigPipe = 73;
    constexpr size_t kMaxPathLength = 1024;
    switch (command) {
        case kDupFd:
            return fcntl(fd, F_DUPFD, static_cast<int>(argument));
        case kDupFdCloexec:
            return fcntl(fd, F_DUPFD_CLOEXEC, static_cast<int>(argument));
        case kGetFd:
            return fcntl(fd, F_GETFD);
        case kSetFd:
            return fcntl(fd, F_SETFD, static_cast<int>(argument));
        case kGetFl: {
            const int flags = fcntl(fd, F_GETFL);
            return flags < 0 ? flags : open_flags_to_darwin(flags);
        }
        case kSetFl:
            return fcntl(fd, F_SETFL, open_flags_to_linux(static_cast<int>(argument)));
        case kGetLk:
        case kSetLk:
        case kSetLkW: {
            auto* in = reinterpret_cast<DarwinFlock*>(argument);
            struct flock out{};
            out.l_type = in->type;
            out.l_whence = in->whence;
            out.l_start = in->start;
            out.l_len = in->length;
            const int result = fcntl(fd, command == kGetLk ? F_GETLK : command == kSetLk ? F_SETLK
                                                                                         : F_SETLKW,
                                     &out);
            if (result == 0 && command == kGetLk) {
                in->type = out.l_type;
                in->whence = out.l_whence;
                in->start = out.l_start;
                in->length = out.l_len;
                in->pid = out.l_pid;
            }
            return result;
        }
        case kNoCache:
        case kSetNoSigPipe:
            return 0;
        case kFullFsync:
            return fsync(fd);
        case kGetPath: {
            const std::string link = "/proc/self/fd/" + std::to_string(fd);
            auto* out = reinterpret_cast<char*>(argument);
            const ssize_t length = readlink(link.c_str(), out, kMaxPathLength - 1);
            if (length < 0) return -1;
            out[length] = '\0';
            return 0;
        }
        default:
            errno = EINVAL;
            return -1;
    }
}

}  // namespace

extern "C" {
int darwin_open_impl(const char* path, int flags, const uint64_t* arguments) { return open(path, open_flags_to_linux(flags), static_cast<mode_t>(arguments[0])); }
int darwin_fcntl_impl(int fd, int command, const uint64_t* arguments) { return linux_fcntl(fd, command, arguments[0]); }
int darwin_ioctl_impl(int fd, unsigned long request, const uint64_t* arguments) {
    constexpr unsigned long kDarwinFionbio = 0x8004667e, kDarwinFionread = 0x4004667f;
    if (request == kDarwinFionbio) return ioctl(fd, FIONBIO, reinterpret_cast<int*>(arguments[0]));
    if (request == kDarwinFionread) return ioctl(fd, FIONREAD, reinterpret_cast<int*>(arguments[0]));
    errno = ENOTTY;
    return -1;
}
void darwin_open();
void darwin_fcntl();
void darwin_ioctl();
}

__asm__(R"(
.text
.globl darwin_open
darwin_open:
    mov x2, sp
    b darwin_open_impl
.globl darwin_fcntl
darwin_fcntl:
    mov x2, sp
    b darwin_fcntl_impl
.globl darwin_ioctl
darwin_ioctl:
    mov x2, sp
    b darwin_ioctl_impl
)");

namespace {

int darwin_kqueue() {
    errno = ENOSYS;
    return -1;
}

int darwin_getxattr(const char*, const char*, void*, size_t, uint32_t, int) {
    errno = ENOTSUP;
    return -1;
}

int darwin_setxattr(const char*, const char*, const void*, size_t, uint32_t, int) {
    errno = ENOTSUP;
    return -1;
}

int darwin_removexattr(const char*, const char*, int) {
    errno = ENOTSUP;
    return -1;
}

ssize_t darwin_listxattr(const char*, char*, size_t, int) { return 0; }

int darwin_lchmod(const char* path, mode_t mode) { return fchmodat(AT_FDCWD, path, mode, AT_SYMLINK_NOFOLLOW); }

int darwin_removefile(const char* path, void*, uint32_t) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return error ? -1 : 0;
}

constexpr int kDarwinUtsNameField = 256;

struct DarwinUtsName {
    char sysname[kDarwinUtsNameField], nodename[kDarwinUtsNameField], release[kDarwinUtsNameField], version[kDarwinUtsNameField], machine[kDarwinUtsNameField];
};

int darwin_uname(DarwinUtsName* out) {
    memset(out, 0, sizeof *out);
    strcpy(out->sysname, "Darwin");
    gethostname(out->nodename, kDarwinUtsNameField - 1);
    strcpy(out->release, "23.0.0");
    strcpy(out->version, "Darwin Kernel Version 23.0.0");
    strcpy(out->machine, "iPhone14,2");
    return 0;
}

}  // namespace

#define DARWIN_FN(name) {#name, reinterpret_cast<void*>(name)}

void add_darwin_io_symbols(DarwinSymbolTable& table) {
    table.insert({
        // The Swift overlay declares these with fixed arguments, so unlike the C ones they arrive in registers.
        {"$s6Darwin4openys5Int32VSPys4Int8VG_ADtF", reinterpret_cast<void*>(+[](const char* path, int flags) -> int { return open(path, open_flags_to_linux(flags)); })},
        {"$s6Darwin5fcntlys5Int32VAD_ADtF", reinterpret_cast<void*>(+[](int fd, int command) -> int { return linux_fcntl(fd, command, 0); })},
        {"$s6Darwin5fcntlys5Int32VAD_A2DtF", reinterpret_cast<void*>(+[](int fd, int command, int argument) -> int { return linux_fcntl(fd, command, static_cast<uint64_t>(argument)); })},
        {"$s6Darwin6S_IFMTs6UInt16Vvg", reinterpret_cast<void*>(+[]() -> uint16_t { return S_IFMT; })},
        {"$s6Darwin7S_IFDIRs6UInt16Vvg", reinterpret_cast<void*>(+[]() -> uint16_t { return S_IFDIR; })},
        {"$s6Darwin7S_IFLNKs6UInt16Vvg", reinterpret_cast<void*>(+[]() -> uint16_t { return S_IFLNK; })},
        {"$s6Darwin7S_IFREGs6UInt16Vvg", reinterpret_cast<void*>(+[]() -> uint16_t { return S_IFREG; })},
        {"open", reinterpret_cast<void*>(darwin_open)},
        {"fcntl", reinterpret_cast<void*>(darwin_fcntl)},
        {"ioctl", reinterpret_cast<void*>(darwin_ioctl)},
        {"stat", reinterpret_cast<void*>(darwin_stat)},
        {"lstat", reinterpret_cast<void*>(darwin_lstat)},
        {"fstat", reinterpret_cast<void*>(darwin_fstat)},
        {"readdir", reinterpret_cast<void*>(darwin_readdir)},
        {"readdir_r", reinterpret_cast<void*>(darwin_readdir_r)},
        {"mmap", reinterpret_cast<void*>(darwin_mmap)},
        {"madvise", reinterpret_cast<void*>(darwin_madvise)},
        {"socket", reinterpret_cast<void*>(darwin_socket)},
        {"socketpair", reinterpret_cast<void*>(darwin_socketpair)},
        {"bind", reinterpret_cast<void*>(darwin_bind)},
        {"connect", reinterpret_cast<void*>(darwin_connect)},
        {"accept", reinterpret_cast<void*>(darwin_accept)},
        {"getsockname", reinterpret_cast<void*>(darwin_getsockname)},
        {"getpeername", reinterpret_cast<void*>(darwin_getpeername)},
        {"send", reinterpret_cast<void*>(darwin_send)},
        {"recv", reinterpret_cast<void*>(darwin_recv)},
        {"sendto", reinterpret_cast<void*>(darwin_sendto)},
        {"recvfrom", reinterpret_cast<void*>(darwin_recvfrom)},
        {"sendmsg", reinterpret_cast<void*>(darwin_sendmsg)},
        {"recvmsg", reinterpret_cast<void*>(darwin_recvmsg)},
        {"setsockopt", reinterpret_cast<void*>(darwin_setsockopt)},
        {"getsockopt", reinterpret_cast<void*>(darwin_getsockopt)},
        {"getaddrinfo", reinterpret_cast<void*>(darwin_getaddrinfo)},
        {"freeaddrinfo", reinterpret_cast<void*>(darwin_freeaddrinfo)},
        {"getnameinfo", reinterpret_cast<void*>(darwin_getnameinfo)},
        {"getifaddrs", reinterpret_cast<void*>(darwin_getifaddrs)},
        {"freeifaddrs", reinterpret_cast<void*>(darwin_freeifaddrs)},
        {"inet_ntop", reinterpret_cast<void*>(darwin_inet_ntop)},
        {"inet_pton", reinterpret_cast<void*>(darwin_inet_pton)},
        {"kqueue", reinterpret_cast<void*>(darwin_kqueue)},
        {"kevent", reinterpret_cast<void*>(+[](int, const void*, int, void*, int, const void*) -> int {
             errno = ENOSYS;
             return -1;
         })},
        {"getxattr", reinterpret_cast<void*>(darwin_getxattr)},
        {"setxattr", reinterpret_cast<void*>(darwin_setxattr)},
        {"removexattr", reinterpret_cast<void*>(darwin_removexattr)},
        {"listxattr", reinterpret_cast<void*>(darwin_listxattr)},
        {"lchmod", reinterpret_cast<void*>(darwin_lchmod)},
        {"removefile", reinterpret_cast<void*>(darwin_removefile)},
        {"uname", reinterpret_cast<void*>(darwin_uname)},
        {"__darwin_check_fd_set_overflow", reinterpret_cast<void*>(+[](int, const void*, int) -> int { return 1; })},
        {"read", reinterpret_cast<void*>(read)},
        {"write", reinterpret_cast<void*>(+[](int fd, const void* data, size_t size) -> ssize_t {
             // An app's own output may be captured into a pipe nobody drains after a crash, so the log gets a copy.
             constexpr size_t kMaxLoggedBytes = 900;
             if (fd == STDOUT_FILENO || fd == STDERR_FILENO) engine_log("STDIO: %.*s", static_cast<int>(size < kMaxLoggedBytes ? size : kMaxLoggedBytes), static_cast<const char*>(data));
             return write(fd, data, size);
         })},
        {"writev", reinterpret_cast<void*>(+[](int fd, const iovec* vectors, int count) -> ssize_t {
             constexpr size_t kMaxLoggedBytes = 900;
             if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
                 for (int v = 0; v < count; ++v) engine_log("STDIO: %.*s", static_cast<int>(vectors[v].iov_len < kMaxLoggedBytes ? vectors[v].iov_len : kMaxLoggedBytes), static_cast<const char*>(vectors[v].iov_base));
             }
             return writev(fd, vectors, count);
         })},
        {"lseek", reinterpret_cast<void*>(lseek)},
        {"mkdir", reinterpret_cast<void*>(mkdir)},
        {"rmdir", reinterpret_cast<void*>(rmdir)},
        {"unlink", reinterpret_cast<void*>(unlink)},
        {"link", reinterpret_cast<void*>(link)},
        {"symlink", reinterpret_cast<void*>(symlink)},
        {"readlink", reinterpret_cast<void*>(readlink)},
        {"mkfifo", reinterpret_cast<void*>(mkfifo)},
        {"pipe", reinterpret_cast<void*>(pipe)},
        {"ftruncate", reinterpret_cast<void*>(ftruncate)},
        {"flock", reinterpret_cast<void*>(flock)},
        {"umask", reinterpret_cast<void*>(static_cast<mode_t (*)(mode_t)>(umask))},
        {"utime", reinterpret_cast<void*>(utime)},
        {"utimes", reinterpret_cast<void*>(utimes)},
        {"lutimes", reinterpret_cast<void*>(lutimes)},
        {"isatty", reinterpret_cast<void*>(isatty)},
        {"opendir", reinterpret_cast<void*>(opendir)},
        {"closedir", reinterpret_cast<void*>(closedir)},
        {"dirfd", reinterpret_cast<void*>(dirfd)},
        {"mprotect", reinterpret_cast<void*>(mprotect)},
        {"munmap", reinterpret_cast<void*>(munmap)},
        {"mlock", reinterpret_cast<void*>(mlock)},
        {"listen", reinterpret_cast<void*>(listen)},
        {"shutdown", reinterpret_cast<void*>(shutdown)},
        {"poll", reinterpret_cast<void*>(poll)},
        {"select", reinterpret_cast<void*>(select)},
        {"gai_strerror", reinterpret_cast<void*>(gai_strerror)},
        {"gethostname", reinterpret_cast<void*>(gethostname)},
        {"gethostbyname", reinterpret_cast<void*>(gethostbyname)},
        {"if_nametoindex", reinterpret_cast<void*>(if_nametoindex)},
        {"inet_addr", reinterpret_cast<void*>(inet_addr)},
        {"in6addr_any", const_cast<in6_addr*>(&in6addr_any)},
        {"in6addr_loopback", const_cast<in6_addr*>(&in6addr_loopback)},
    });
}
