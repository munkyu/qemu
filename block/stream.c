/*
 * Image streaming
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi   <stefanha@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "block_int.h"

enum {
    /*
     * Size of data buffer for populating the image file.  This should be large
     * enough to process multiple clusters in a single call, so that populating
     * contiguous regions of the image is efficient.
     */
    STREAM_BUFFER_SIZE = 512 * 1024, /* in bytes */
};

typedef struct StreamBlockJob {
    BlockJob common;
    BlockDriverState *base;
} StreamBlockJob;

static int coroutine_fn stream_populate(BlockDriverState *bs,
                                        int64_t sector_num, int nb_sectors,
                                        void *buf)
{
    struct iovec iov = {
        .iov_base = buf,
        .iov_len  = nb_sectors * BDRV_SECTOR_SIZE,
    };
    QEMUIOVector qiov;

    qemu_iovec_init_external(&qiov, &iov, 1);

    /* Copy-on-read the unallocated clusters */
    return bdrv_co_copy_on_readv(bs, sector_num, nb_sectors, &qiov);
}

static void coroutine_fn stream_run(void *opaque)
{
    StreamBlockJob *s = opaque;
    BlockDriverState *bs = s->common.bs;
    int64_t sector_num, end;
    int ret = 0;
    int n;
    void *buf;

    s->common.len = bdrv_getlength(bs);
    if (s->common.len < 0) {
        block_job_complete(&s->common, s->common.len);
        return;
    }

    end = s->common.len >> BDRV_SECTOR_BITS;
    buf = qemu_blockalign(bs, STREAM_BUFFER_SIZE);

    /* Turn on copy-on-read for the whole block device so that guest read
     * requests help us make progress.  Only do this when copying the entire
     * backing chain since the copy-on-read operation does not take base into
     * account.
     */
    if (!base) {
        bdrv_enable_copy_on_read(bs);
    }

    for (sector_num = 0; sector_num < end; sector_num += n) {
        if (block_job_is_cancelled(&s->common)) {
            break;
        }

        /* TODO rate-limit */
        /* Note that even when no rate limit is applied we need to yield with
         * no pending I/O here so that qemu_aio_flush() is able to return.
         */
        co_sleep_ns(rt_clock, 0);

        ret = bdrv_co_is_allocated(bs, sector_num,
                                   STREAM_BUFFER_SIZE / BDRV_SECTOR_SIZE, &n);
        trace_stream_one_iteration(s, sector_num, n, ret);
        if (ret == 0) {
            ret = stream_populate(bs, sector_num, n, buf);
        }
        if (ret < 0) {
            break;
        }

        /* Publish progress */
        s->common.offset += n * BDRV_SECTOR_SIZE;
    }

    if (!base) {
        bdrv_disable_copy_on_read(bs);
    }

    if (sector_num == end && ret == 0) {
        ret = bdrv_change_backing_file(bs, NULL, NULL);
    }

    qemu_vfree(buf);
    block_job_complete(&s->common, ret);
}

static BlockJobType stream_job_type = {
    .instance_size = sizeof(StreamBlockJob),
    .job_type      = "stream",
};

int stream_start(BlockDriverState *bs, BlockDriverState *base,
                 BlockDriverCompletionFunc *cb, void *opaque)
{
    StreamBlockJob *s;
    Coroutine *co;

    s = block_job_create(&stream_job_type, bs, cb, opaque);
    if (!s) {
        return -EBUSY; /* bs must already be in use */
    }

    s->base = base;

    co = qemu_coroutine_create(stream_run);
    trace_stream_start(bs, base, s, co, opaque);
    qemu_coroutine_enter(co, s);
    return 0;
}
