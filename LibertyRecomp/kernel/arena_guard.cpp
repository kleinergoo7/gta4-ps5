// WORKAROUND: see arena_guard.h. Moved here from main.cpp (step 5:
// workarounds live in kernel/memory, labeled, logging every trigger).
// Signal-safe: only open/read/close/mmap/munmap/mprotect/write plus manual
// parsing here - no malloc, stdio, or STL.

#include "arena_guard.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "memory.h"

extern Memory g_memory;

namespace ArenaGuard
{
namespace
{

struct sigaction g_prevSegv;

bool CommitFault(uint64_t fa_in);
void SegvEntry(int sig, siginfo_t* info, void* uctx);

void SegvEntry(int sig, siginfo_t* info, void* uctx)
{
    bool handled = false;
    if (sig == SIGSEGV && info)
    {
        handled = CommitFault(reinterpret_cast<uint64_t>(info->si_addr));
    }
    if (!handled)
    {
        // Decline: forward to the previously installed handler (SEH chain,
        // default disposition, whatever was there). SA_RESETHAND is not set
        // so we stay installed.
        if (g_prevSegv.sa_sigaction)
        {
            g_prevSegv.sa_sigaction(sig, info, uctx);
            return;
        }
        // No previous handler: restore default and re-raise to die loudly.
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, nullptr);
        raise(sig);
        _exit(139);
    }
}

void WriteAll(const char* msg, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t w = ::write(STDERR_FILENO, msg + off, len - off);
        if (w <= 0)
            break;
        off += static_cast<size_t>(w);
    }
}

void WriteHex32(char*& p, uint32_t v)
{
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        unsigned dg = (v >> shift) & 0xF;
        *p++ = static_cast<char>(dg < 10 ? '0' + dg : 'a' + dg - 10);
    }
}

// Returns true when fa was committed (caller resumes), false otherwise.
bool CommitFault(uint64_t fa_in)
{
    uint8_t* base = g_memory.base;
    if (!base)
    {
        return false;
    }
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    const uintptr_t fa = static_cast<uintptr_t>(fa_in);
    // Keep the null page fatal; stay strictly inside the 4GB arena.
    if (fa < b + 0x10000u || fa >= b + 0x100000000ULL)
    {
        return false;
    }
    const uintptr_t page = fa & ~uintptr_t(0xFFFu);
    // The link map here is ~85KB (Vulkan layers etc.); read it whole.
    const size_t kCap = 262144;
    char* buf = static_cast<char*>(::mmap(nullptr, kCap, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    size_t used = 0;
    if (buf != MAP_FAILED)
    {
        int fd = ::open("/proc/self/maps", O_RDONLY);
        if (fd >= 0)
        {
            for (;;)
            {
                if (used >= kCap)
                {
                    break;
                }
                ssize_t r = ::read(fd, buf + used, kCap - used);
                if (r <= 0)
                {
                    break;
                }
                used += static_cast<size_t>(r);
            }
            ::close(fd);
        }
    }
    // Parse "start-end perms ..." lines; stop at buffer end (truncated
    // maps simply miss, which safely declines to handle).
    bool is_noaccess = false;
    size_t i = 0;
    if (buf == MAP_FAILED)
    {
        return false;
    }
    while (i < used)
    {
        // Parse start address (hex).
        uintptr_t start = 0;
        bool have_start = false;
        while (i < used && buf[i] != '-' && buf[i] != '\n')
        {
            char c = buf[i++];
            unsigned v;
            if (c >= '0' && c <= '9')
                v = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v = static_cast<unsigned>(c - 'A' + 10);
            else
                break;
            start = (start << 4) | v;
            have_start = true;
        }
        if (i >= used || buf[i] != '-')
        {
            while (i < used && buf[i] != '\n')
                ++i;
            if (i < used)
                ++i;
            continue;
        }
        ++i; // '-'
        uintptr_t end = 0;
        bool have_end = false;
        while (i < used && buf[i] != ' ' && buf[i] != '\n')
        {
            char c = buf[i++];
            unsigned v;
            if (c >= '0' && c <= '9')
                v = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v = static_cast<unsigned>(c - 'A' + 10);
            else
                break;
            end = (end << 4) | v;
            have_end = true;
        }
        if (i >= used || buf[i] != ' ')
        {
            while (i < used && buf[i] != '\n')
                ++i;
            if (i < used)
                ++i;
            continue;
        }
        ++i; // ' '
        if (i + 4 > used)
        {
            break;
        }
        if (have_start && have_end && start <= page && page < end && buf[i] == '-' &&
            buf[i + 1] == '-' && buf[i + 2] == '-')
        {
            is_noaccess = true;
            break;
        }
        while (i < used && buf[i] != '\n')
            ++i;
        if (i < used)
            ++i;
    }
    if (!is_noaccess)
    {
        ::munmap(buf, kCap);
        // Log every in-arena decline so nothing slips through silently.
        static int declines = 0;
        int d = __atomic_add_fetch(&declines, 1, __ATOMIC_RELAXED);
        char msg[112];
        char* p = msg;
        const char* pre = "[arena-decline] guest=0x";
        while (*pre)
            *p++ = *pre++;
        WriteHex32(p, static_cast<uint32_t>(fa - b));
        const char* mid = " (#";
        while (*mid)
            *p++ = *mid++;
        char num[12];
        int nd = 0;
        int t = d;
        do
        {
            num[nd++] = static_cast<char>('0' + (t % 10));
            t /= 10;
        } while (t > 0);
        while (nd > 0)
            *p++ = num[--nd];
        *p++ = ')';
        *p++ = '\n';
        WriteAll(msg, static_cast<size_t>(p - msg));
        return false;
    }

    // Budget: never commit more than 256MB total across the run; beyond that
    // something is genuinely wrong, so crash loudly instead of masking it.
    // Same-page guard: a fault on a page we just committed means the commit
    // didn't take (or access is blocked another way); bail instead of looping.
    static int commits = 0;
    static uintptr_t last_page = 0;
    static int last_page_hits = 0;
    if (page == last_page)
    {
        if (++last_page_hits > 3)
        {
            ::munmap(buf, kCap);
            return false;
        }
    }
    else
    {
        last_page = page;
        last_page_hits = 0;
    }
    ::munmap(buf, kCap);
    int n = __atomic_add_fetch(&commits, 1, __ATOMIC_RELAXED);
    if (n > 65536)
    {
        return false;
    }
    if (::mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE) != 0)
    {
        return false;
    }
    // Log every commit with guest address and running total.
    {
        char msg[112];
        char* p = msg;
        const char* pre = "[arena-commit] guest=0x";
        while (*pre)
            *p++ = *pre++;
        WriteHex32(p, static_cast<uint32_t>(fa - b));
        const char* mid = " (#";
        while (*mid)
            *p++ = *mid++;
        char num[12];
        int nd = 0;
        int t = n;
        do
        {
            num[nd++] = static_cast<char>('0' + (t % 10));
            t /= 10;
        } while (t > 0);
        while (nd > 0)
            *p++ = num[--nd];
        *p++ = ')';
        *p++ = '\n';
        WriteAll(msg, static_cast<size_t>(p - msg));
    }
    return true;
}

} // namespace

void Install()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SegvEntry;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    // The rex exception chain is preempted during Setup
    // (guest::initialize installs its SEH handler over the chain entry and
    // nothing restores it), so chain-based handlers never fire for these
    // faults. This installs directly after Setup and chains to whatever was
    // installed before us on decline, so SEH/MMIO/default behavior is
    // preserved. Installed after Memory::Initialize so MMIO-owned ranges keep
    // their emulation (MMIOHandler runs first and claims them).
    if (sigaction(SIGSEGV, &sa, &g_prevSegv) != 0)
    {
        const char* warn = "[Main] WARNING: ArenaGuard sigaction install failed\n";
        WriteAll(warn, strlen(warn));
    }
}

} // namespace ArenaGuard
