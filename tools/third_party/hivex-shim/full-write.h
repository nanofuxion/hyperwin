#ifndef HW_HIVEX_FULL_WRITE_H
#define HW_HIVEX_FULL_WRITE_H
#include <errno.h>
#include <unistd.h>
static inline size_t hw_hivex_full_write(int fd, const void *buf, size_t count)
{
    size_t done = 0;
    while (done < count) {
        ssize_t r = write(fd, (const char *)buf + done, count - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (size_t)r;
    }
    return done;
}
#define full_write hw_hivex_full_write
#endif
