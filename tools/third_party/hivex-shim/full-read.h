#ifndef HW_HIVEX_FULL_READ_H
#define HW_HIVEX_FULL_READ_H
#include <errno.h>
#include <unistd.h>
/* gnulib full_read under another name: wimlib exports its own full_read. */
static inline size_t hw_hivex_full_read(int fd, void *buf, size_t count)
{
    size_t done = 0;
    while (done < count) {
        ssize_t r = read(fd, (char *)buf + done, count - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) { errno = 0; break; }
        done += (size_t)r;
    }
    return done;
}
#define full_read hw_hivex_full_read
#endif
