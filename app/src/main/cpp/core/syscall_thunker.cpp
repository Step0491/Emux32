#include "syscall_thunker.h"
#include "guest_memory.h"
#include "guest_thread.h"
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <vector>
#include <linux/futex.h>
#include <android/log.h>

#define TAG "EmuxSyscallThunker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

int32_t SyscallThunker::handleFutex(uc_engine* uc, uint32_t uaddr, int op, uint32_t val,
                                   uint32_t timeout, uint32_t uaddr2, uint32_t val3) {
    if (!uc || uaddr == 0) return -EFAULT;

    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    struct timespec ts{};
    struct timespec* pts = nullptr;

    if (timeout != 0 && (cmd == FUTEX_WAIT || cmd == FUTEX_LOCK_PI || cmd == FUTEX_WAIT_BITSET)) {
        timespec32 ts32{};
        if (uc_mem_read(uc, timeout, &ts32, sizeof(ts32)) == UC_ERR_OK) {
            ts.tv_sec = ts32.tv_sec;
            ts.tv_nsec = ts32.tv_nsec;
            pts = &ts;
        }
    }

    auto& gm = GuestMemory::getSharedInstance();
    if (gm.isReady() && gm.isMapped(uaddr)) {
        uint32_t* hostPtr = reinterpret_cast<uint32_t*>(gm.g2h(uaddr));
        uint32_t* hostPtr2 = nullptr;
        if (uaddr2 != 0 && gm.isMapped(uaddr2)) {
            hostPtr2 = reinterpret_cast<uint32_t*>(gm.g2h(uaddr2));
        }

        // Normalizza con FUTEX_PRIVATE_FLAG per garantire che tutti i thread dello stesso processo
        // usino il medesimo hash bucket nel kernel Linux ed evitino deadlock tra WAIT e WAKE
        int normOp = cmd | FUTEX_PRIVATE_FLAG;

        if (cmd == FUTEX_WAIT && !pts) {
            // Schedulazione cooperativa non bloccante: timeout a slice di 250ms per verificare shutdown o cambi di valore
            struct timespec pollSlice = {0, 250000000}; // 250 ms
            while (!GuestThreadScheduler::getInstance().isShutdown()) {
                long ret = syscall(SYS_futex, hostPtr, normOp, val, &pollSlice, hostPtr2, val3);
                if (ret == 0) return 0;
                if (errno == EWOULDBLOCK) return -EWOULDBLOCK;
                if (errno == ETIMEDOUT) {
                    if (*hostPtr != val) return -EWOULDBLOCK;
                    continue;
                }
                return -errno;
            }
            return -EINTR;
        }

        long ret = syscall(SYS_futex, hostPtr, normOp, val, pts, hostPtr2, val3);
        if (ret < 0) {
            return -errno;
        }
        return static_cast<int32_t>(ret);
    }

    // Fallback se memoria non ancora mappata in GuestMemory
    uint32_t curVal = 0;
    uc_mem_read(uc, uaddr, &curVal, 4);

    if (cmd == FUTEX_WAIT) {
        if (curVal != val) {
            return -EWOULDBLOCK;
        }
        if (pts) {
            nanosleep(pts, nullptr);
        } else {
            usleep(1000);
        }
        return 0;
    } else if (cmd == FUTEX_WAKE) {
        return static_cast<int32_t>(val);
    }

    return 0;
}

int32_t SyscallThunker::handleWritev(uc_engine* uc, int fd, uint32_t iovGuestPtr, int iovcnt) {
    if (!uc || iovGuestPtr == 0 || iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    std::vector<iovec32> guestIov(iovcnt);
    if (uc_mem_read(uc, iovGuestPtr, guestIov.data(), sizeof(iovec32) * iovcnt) != UC_ERR_OK) {
        return -EFAULT;
    }

    std::vector<struct iovec> hostIov(iovcnt);
    std::vector<std::vector<uint8_t>> buffers(iovcnt);

    for (int i = 0; i < iovcnt; ++i) {
        hostIov[i].iov_len = guestIov[i].iov_len;
        buffers[i].resize(guestIov[i].iov_len);
        if (guestIov[i].iov_len > 0) {
            uc_mem_read(uc, guestIov[i].iov_base, buffers[i].data(), guestIov[i].iov_len);
            hostIov[i].iov_base = buffers[i].data();
        } else {
            hostIov[i].iov_base = nullptr;
        }
    }

    ssize_t ret = writev(fd, hostIov.data(), iovcnt);
    return (ret >= 0) ? static_cast<int32_t>(ret) : -errno;
}

int32_t SyscallThunker::handleReadv(uc_engine* uc, int fd, uint32_t iovGuestPtr, int iovcnt) {
    if (!uc || iovGuestPtr == 0 || iovcnt <= 0 || iovcnt > 1024) return -EINVAL;

    std::vector<iovec32> guestIov(iovcnt);
    if (uc_mem_read(uc, iovGuestPtr, guestIov.data(), sizeof(iovec32) * iovcnt) != UC_ERR_OK) {
        return -EFAULT;
    }

    std::vector<struct iovec> hostIov(iovcnt);
    std::vector<std::vector<uint8_t>> buffers(iovcnt);

    for (int i = 0; i < iovcnt; ++i) {
        hostIov[i].iov_len = guestIov[i].iov_len;
        buffers[i].resize(guestIov[i].iov_len);
        hostIov[i].iov_base = buffers[i].data();
    }

    ssize_t ret = readv(fd, hostIov.data(), iovcnt);
    if (ret > 0) {
        ssize_t remaining = ret;
        for (int i = 0; i < iovcnt && remaining > 0; ++i) {
            size_t take = std::min<size_t>(guestIov[i].iov_len, remaining);
            if (take > 0) {
                uc_mem_write(uc, guestIov[i].iov_base, buffers[i].data(), take);
                remaining -= take;
            }
        }
    }

    return (ret >= 0) ? static_cast<int32_t>(ret) : -errno;
}

int32_t SyscallThunker::handleNanosleep(uc_engine* uc, uint32_t reqPtr, uint32_t remPtr) {
    if (!uc || reqPtr == 0) return -EFAULT;
    timespec32 req32{};
    if (uc_mem_read(uc, reqPtr, &req32, sizeof(req32)) != UC_ERR_OK) return -EFAULT;

    struct timespec reqHost{};
    reqHost.tv_sec = req32.tv_sec;
    reqHost.tv_nsec = req32.tv_nsec;

    struct timespec remHost{};
    int ret = nanosleep(&reqHost, remPtr != 0 ? &remHost : nullptr);

    if (ret != 0 && remPtr != 0) {
        timespec32 rem32{};
        rem32.tv_sec = static_cast<int32_t>(remHost.tv_sec);
        rem32.tv_nsec = static_cast<int32_t>(remHost.tv_nsec);
        uc_mem_write(uc, remPtr, &rem32, sizeof(rem32));
    }

    return (ret == 0) ? 0 : -errno;
}

int32_t SyscallThunker::dispatch(uc_engine* uc, uint32_t sysno, uint32_t r0, uint32_t r1,
                                 uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5) {
    switch (sysno) {
        case 146: // ARM writev
            return handleWritev(uc, static_cast<int>(r0), r1, static_cast<int>(r2));
        case 145: // ARM readv
            return handleReadv(uc, static_cast<int>(r0), r1, static_cast<int>(r2));
        case 162: // ARM nanosleep
            return handleNanosleep(uc, r0, r1);
        case 240: // ARM futex
            return handleFutex(uc, r0, static_cast<int>(r1), r2, r3, r4, r5);
        default:
            return 0;
    }
}

} // namespace emux
