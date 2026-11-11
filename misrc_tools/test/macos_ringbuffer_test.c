// SPDX-License-Identifier: GPL-3.0-or-later
// Real Darwin POSIX shared-memory mirroring and descriptor lifetime.
#include "../common/ringbuffer.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    for (int iteration=0; iteration<32; iteration++) {
        ringbuffer_t rb = {0};
        size_t size=(size_t)sysconf(_SC_PAGESIZE)*4;
        assert(rb_init(&rb,"misrc-test",size)==0);
        int fd=rb.fd;
        rb.buffer[0]=17;
        assert(rb.buffer[size]==17);
        rb.buffer[size+1]=231;
        assert(rb.buffer[1]==231);
        rb_close(&rb);
        errno=0;
        assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
    }
    return 0;
}
