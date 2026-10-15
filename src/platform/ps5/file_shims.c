#include <fcntl.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

extern int sceKernelDlsym(int module, const char* name, void** symbol);
static _Atomic(uintptr_t) directory_gateway;
struct directory_result { long value; long failed; };

// The pinned SDK CRT enters libkernel's getpid stub at +0xa. Match that
// existing contract without reading execute-only pages or issuing a syscall
// from the folder title's executable text. Capture carry before stack cleanup.
__attribute__((naked, noinline)) static struct directory_result directory_syscall(
    void* entry, long number, long first, const char* second, long third, long fourth)
{
    __asm__ volatile(
        "movq %rdi, %r11\n"
        "movq %rsi, %rax\n"
        "movq %rdx, %rdi\n"
        "movq %rcx, %rsi\n"
        "movq %r8, %rdx\n"
        "movq %r9, %r10\n"
        "subq $8, %rsp\n"
        "call *%r11\n"
        "setc %dl\n"
        "movzbl %dl, %edx\n"
        "addq $8, %rsp\n"
        "ret\n");
}

int z3cloudplay_ps5_directory_api_ready(void)
{
    if (atomic_load_explicit(&directory_gateway, memory_order_acquire)) return 1;
    void* getpid_entry = 0;
    if (sceKernelDlsym(0x2001, "getpid", &getpid_entry) != 0 || !getpid_entry) return 0;
    void* gateway = (void*)((uintptr_t)getpid_entry + 0xa);
    const struct directory_result probe = directory_syscall(gateway, SYS_getpid, 0, 0, 0, 0);
    if (probe.failed || probe.value != getpid()) return 0;
    atomic_store_explicit(&directory_gateway, (uintptr_t)gateway, memory_order_release);
    return 1;
}

static int directory_call(long number, int directory, const char* path, long third, long fourth)
{
    if (!z3cloudplay_ps5_directory_api_ready()) { errno = ENOSYS; return -1; }
    void* entry = (void*)atomic_load_explicit(&directory_gateway, memory_order_acquire);
    const struct directory_result result = directory_syscall(entry, number, directory, path, third, fourth);
    if (result.failed) { errno = (int)result.value; return -1; }
    return (int)result.value;
}

// Qualified nonblocking cooperative lock primitive. ManagedFileLock admits the
// private stable inode; receipt/lifecycle gates are enforced by its caller.
int z3cloudplay_ps5_file_lock(int descriptor, int operation)
{
    return directory_call(SYS_flock, descriptor,
                          (const char*)(uintptr_t)operation, 0, 0);
}


int __wrap_openat(int directory, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return directory_call(SYS_openat, directory, path, flags, mode);
}

int __wrap_mkdirat(int directory, const char* path, mode_t mode)
{
    return directory_call(SYS_mkdirat, directory, path, mode, 0);
}
