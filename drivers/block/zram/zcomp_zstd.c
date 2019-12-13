/*
 * Copyright (C) 2019 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/zstd.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include "zcomp_zstd.h"

struct zstd_ctx {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void *cwksp;
    void *dwksp;
};

struct zstd_ctx *ctx = NULL;

#define ZSTD_DEF_LEVEL 3

static ZSTD_parameters zstd_params(void)
{
    return ZSTD_getParams(ZSTD_DEF_LEVEL, 0, 0);
}

static int zstd_comp_init(struct zstd_ctx *ctx)
{
    int ret = 0;
    const ZSTD_parameters params = zstd_params();
    const size_t wksp_size = ZSTD_CCtxWorkspaceBound(params.cParams);

    ctx->cwksp = vzalloc(wksp_size);
    if (!ctx->cwksp) {
        ret = -ENOMEM;
        goto out;
    }

    ctx->cctx = ZSTD_initCCtx(ctx->cwksp, wksp_size);
    if (!ctx->cctx) {
        ret = -EINVAL;
        goto out_free;
    }
out:
    return ret;
out_free:
    vfree(ctx->cwksp);
    goto out;
}

static int zstd_decomp_init(struct zstd_ctx *ctx)
{
    int ret = 0;
    const size_t wksp_size = ZSTD_DCtxWorkspaceBound();

    ctx->dwksp = vzalloc(wksp_size);
    if (!ctx->dwksp) {
        ret = -ENOMEM;
        goto out;
    }

    ctx->dctx = ZSTD_initDCtx(ctx->dwksp, wksp_size);
    if (!ctx->dctx) {
        ret = -EINVAL;
        goto out_free;
    }
out:
    return ret;
out_free:
    vfree(ctx->dwksp);
    goto out;
}

static void zstd_comp_exit(struct zstd_ctx *ctx)
{
    vfree(ctx->cwksp);
    ctx->cwksp = NULL;
    ctx->cctx = NULL;
}

static void zstd_decomp_exit(struct zstd_ctx *ctx)
{
    vfree(ctx->dwksp);
    ctx->dwksp = NULL;
    ctx->dctx = NULL;
}

static int __zstd_init(void *ctx)
{
    int ret;

    ret = zstd_comp_init(ctx);
    if (ret)
        return ret;
    ret = zstd_decomp_init(ctx);
    if (ret)
        zstd_comp_exit(ctx);
    return ret;
}

static int __zstd_compress(const u8 *src, unsigned int slen,
               u8 *dst, unsigned int *dlen, void *ctx)
{
    size_t out_len;
    struct zstd_ctx *zctx = ctx;
    const ZSTD_parameters params = zstd_params();

    out_len = ZSTD_compressCCtx(zctx->cctx, dst, *dlen, src, slen, params);
    if (ZSTD_isError(out_len)) {
        ZSTD_ErrorCode errcode = ZSTD_getErrorCode(out_len);
        printk("zram: compress error code2 = %d", errcode);
        return -EINVAL;
    }
    *dlen = out_len;

    printk("zram: slen = %u, out_len = %zu\n", slen, out_len);

    return 0;
}

static int __zstd_decompress(const u8 *src, unsigned int slen,
                 u8 *dst, unsigned int *dlen, void *ctx)
{
    size_t out_len;
    struct zstd_ctx *zctx = ctx;

    out_len = ZSTD_decompressDCtx(zctx->dctx, dst, *dlen, src, slen);
    if (ZSTD_isError(out_len)) {
        ZSTD_ErrorCode errcode = ZSTD_getErrorCode(out_len);
        printk("zram: decompress error code2 = %d", errcode);
        return -EINVAL;
    }
    *dlen = out_len;

    printk("zram: slen = %u, out_len = %zu\n", slen, out_len);

    return 0;
}

static void *zcomp_zstd_create(void)
{
    void *ret;

    /*
     * This function can be called in swapout/fs write path
     * so we can't use GFP_FS|IO. And it assumes we already
     * have at least one stream in zram initialization so we
     * don't do best effort to allocate more stream in here.
     * A default stream will work well without further multiple
     * streams. That's why we use NORETRY | NOWARN.
     */
    ret = kzalloc(sizeof(*ctx), GFP_NOIO | __GFP_NORETRY |
                    __GFP_NOWARN);
    if (!ret)
        ret = __vmalloc(sizeof(*ctx),
                GFP_NOIO | __GFP_NORETRY | __GFP_NOWARN |
                __GFP_ZERO | __GFP_HIGHMEM,
                PAGE_KERNEL);

    if(ret)
        __zstd_init(ret);

    ctx = ret;
    return ret;
}

static void zcomp_zstd_destroy(void *private)
{
    zstd_comp_exit(private);
    zstd_decomp_exit(private);
    kvfree(private);
    ctx = NULL;
}

static int zcomp_zstd_compress(const unsigned char *src, unsigned char *dst,
        size_t *dst_len, void *private)
{
    /* return  : Success if return 0 */
    // lz4_compress(src, PAGE_SIZE, dst, dst_len, private);

    return __zstd_compress(src, PAGE_SIZE, dst, (unsigned int *)dst_len, ctx);
}

static int zcomp_zstd_decompress(const unsigned char *src, size_t src_len,
        unsigned char *dst)
{
    size_t dst_len = PAGE_SIZE;

    /* return  : Success if return 0 */
    return __zstd_decompress(src, src_len, dst, (unsigned int *)&dst_len, ctx);
}

struct zcomp_backend zcomp_zstd = {
    .compress = zcomp_zstd_compress,
    .decompress = zcomp_zstd_decompress,
    .create = zcomp_zstd_create,
    .destroy = zcomp_zstd_destroy,
    .name = "zstd",
};

