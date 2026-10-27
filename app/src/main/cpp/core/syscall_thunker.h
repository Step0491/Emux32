#pragma once

#include <cstdint>
#include <cstddef>
#include <sys/types.h>
#include "unicorn/unicorn.h"

namespace emux {

/**
 * Strutture dati ABI a 32 bit per il thunking con il kernel a 64 bit.
 */
struct timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct iovec32 {
    uint32_t iov_base;
    uint32_t iov_len;
};

class SyscallThunker {
public:
    static int32_t dispatch(uc_engine* uc, uint32_t sysno, uint32_t r0, uint32_t r1,
                            uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5);

private:
    static int32_t handleFutex(uc_engine* uc, uint32_t uaddr, int op, uint32_t val,
                               uint32_t timeout, uint32_t uaddr2, uint32_t val3);
    static int32_t handleWritev(uc_engine* uc, int fd, uint32_t iovGuestPtr, int iovcnt);
    static int32_t handleReadv(uc_engine* uc, int fd, uint32_t iovGuestPtr, int iovcnt);
    static int32_t handleNanosleep(uc_engine* uc, uint32_t reqPtr, uint32_t remPtr);
};

} // namespace emux
