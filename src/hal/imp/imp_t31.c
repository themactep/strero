#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

#include "hal/platform.h"
#include "hal/imp.h"

/* ---------------- Platform capabilities (T31) ---------------- */
static const hal_caps_t g_caps = {
    .has_entropy_cabac = true,
    .has_set_bufsize   = true,
    .has_jpeg_qp       = true,
    .has_dmic          = true,  /* present on T31 SDK */
    .has_hevc          = true,
};

const hal_caps_t* hal_caps(void) {
    return &g_caps;
}

/* ---------------- Internal mapping helpers ---------------- */
static inline IMPEncoderProfile map_profile(hal_payload_t pt, uint8_t profile_hint) {
    (void)profile_hint; /* can be extended later */
    switch (pt) {
        case HAL_PT_H265: return IMP_ENC_PROFILE_HEVC_MAIN;
        case HAL_PT_JPEG: return IMP_ENC_PROFILE_JPEG;
        case HAL_PT_H264:
        default:          return IMP_ENC_PROFILE_AVC_HIGH; /* good default */
    }
}

static inline IMPEncoderRcMode map_rc(hal_rc_mode_t rc) {
    switch (rc) {
        case HAL_RC_FIXQP: return IMP_ENC_RC_MODE_FIXQP;
        case HAL_RC_VBR:   return IMP_ENC_RC_MODE_VBR;
        case HAL_RC_CBR:
        default:           return IMP_ENC_RC_MODE_CBR;
    }
}

static inline hal_payload_t unmap_profile(uint32_t eProfile) {
    uint32_t enc_type = (eProfile >> 24);
    if (enc_type == IMP_ENC_TYPE_HEVC) return HAL_PT_H265;
    if (enc_type == IMP_ENC_TYPE_JPEG) return HAL_PT_JPEG;
    return HAL_PT_H264;
}

/* ---------------- Encoder lifecycle ---------------- */
int hal_enc_create_group(int group) {
    return IMP_Encoder_CreateGroup(group);
}

int hal_enc_destroy_group(int group) {
    return IMP_Encoder_DestroyGroup(group);
}

int hal_enc_create(int ch, const hal_enc_attr_t *a) {
    if (!a) return -1;

    IMPEncoderChnAttr attr;
    memset(&attr, 0, sizeof(attr));

    IMPEncoderProfile profile = map_profile(a->payload, a->profile);
    IMPEncoderRcMode  rcMode  = map_rc(a->rc_mode);

    /* Use vendor helper to initialize sane defaults */
    int ret = IMP_Encoder_SetDefaultParam(
        &attr, profile, rcMode,
        a->width, a->height,
        a->fps.num, a->fps.den,
        a->gop,
        0, /* uMaxSameSenceCnt: default */
        (a->init_qp >= 0) ? a->init_qp : -1,
        a->target_kbps * 1000u /* bitrate in bps */);
    if (ret < 0) return ret;

    return IMP_Encoder_CreateChn(ch, &attr);
}

int hal_enc_destroy(int ch) {
    return IMP_Encoder_DestroyChn(ch);
}

int hal_enc_register(int group, int ch) {
    return IMP_Encoder_RegisterChn(group, ch);
}

int hal_enc_unregister(int group, int ch) {
    (void)group; /* T31 API takes only channel */
    return IMP_Encoder_UnRegisterChn(ch);
}

int hal_enc_start(int ch) {
    return IMP_Encoder_StartRecvPic(ch);
}

int hal_enc_stop(int ch) {
    return IMP_Encoder_StopRecvPic(ch);
}

int hal_enc_request_idr(int ch) {
    return IMP_Encoder_RequestIDR(ch);
}

/* ---------------- Optional tuning ---------------- */
int hal_enc_set_stream_bufcnt(int ch, int cnt) {
    (void)ch; (void)cnt;
    /* No direct API in T31 public headers; keep as no-op returning 0 */
    return 0;
}

int hal_enc_set_stream_bufsize(int ch, uint32_t bytes) {
    return IMP_Encoder_SetStreamBufSize(ch, bytes);
}

int hal_enc_set_entropy_cabac(int ch, int enable) {
    return IMP_Encoder_SetChnEntropyMode(ch, enable ? 1 : 0);
}

int hal_enc_set_jpeg_qp(int ch, int qp) {
    return IMP_Encoder_SetChnQp(ch, qp);
}

/* ---------------- Attribute query ---------------- */
int hal_enc_get_attr(int ch, hal_enc_attr_t *a) {
    if (!a) return -1;
    memset(a, 0, sizeof(*a));

    IMPEncoderChnAttr attr;
    memset(&attr, 0, sizeof(attr));
    int ret = IMP_Encoder_GetChnAttr(ch, &attr);
    if (ret < 0) return ret;

    a->payload = unmap_profile(attr.encAttr.eProfile);
    a->width   = attr.encAttr.uWidth;
    a->height  = attr.encAttr.uHeight;
    a->fps.num = attr.rcAttr.outFrmRate.frmRateNum;
    a->fps.den = attr.rcAttr.outFrmRate.frmRateDen;
    a->gop     = attr.gopAttr.uGopLength;

    switch (attr.rcAttr.attrRcMode.rcMode) {
        case IMP_ENC_RC_MODE_FIXQP:
            a->rc_mode = HAL_RC_FIXQP;
            a->init_qp = attr.rcAttr.attrRcMode.attrFixQp.iInitialQP;
            break;
        case IMP_ENC_RC_MODE_VBR:
            a->rc_mode = HAL_RC_VBR;
            a->target_kbps = attr.rcAttr.attrRcMode.attrVbr.uTargetBitRate / 1000u;
            break;
        case IMP_ENC_RC_MODE_CBR:
        default:
            a->rc_mode = HAL_RC_CBR;
            a->target_kbps = attr.rcAttr.attrRcMode.attrCbr.uTargetBitRate / 1000u;
            break;
    }
    return 0;
}

/* ---------------- Stream APIs ---------------- */
int hal_stream_poll(int ch, int timeout_ms) {
    return IMP_Encoder_PollingStream(ch, timeout_ms);
}

int hal_stream_get(int ch, hal_stream_t *s, int block) {
    if (!s) return -1;
    IMPEncoderStream *st = (IMPEncoderStream*)malloc(sizeof(IMPEncoderStream));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    int ret = IMP_Encoder_GetStream(ch, st, block ? 1 : 0);
    if (ret < 0) {
        free(st);
        return ret;
    }
    s->impl = st;
    return 0;
}

int hal_stream_release(int ch, hal_stream_t *s) {
    if (!s || !s->impl) return -1;
    int ret = IMP_Encoder_ReleaseStream(ch, (IMPEncoderStream*)s->impl);
    free(s->impl);
    s->impl = NULL;
    return ret;
}

static inline IMPEncoderStream* as_native(const hal_stream_t *s) {
    return (IMPEncoderStream*)s->impl;
}

int hal_stream_pack_count(const hal_stream_t *s) {
    const IMPEncoderStream *st = as_native(s);
    return st ? st->packCount : 0;
}

uint32_t hal_stream_pack_length(const hal_stream_t *s, int index) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= st->packCount) return 0;
    return st->pack[index].length;
}

uint64_t hal_stream_pack_timestamp_us(const hal_stream_t *s, int index) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= st->packCount) return 0;
    return (uint64_t)st->pack[index].timestamp;
}

uint32_t hal_stream_copy_pack(const hal_stream_t *s, int index, uint8_t *dst) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= st->packCount || !dst) return 0;
    const IMPEncoderPack *pk = &st->pack[index];
    uint32_t len = pk->length;
    uint32_t off = pk->offset;

    const uint8_t *base = (const uint8_t*)st->virAddr;
    uint32_t rem = st->streamSize - off;
    if (rem < len) {
        memcpy(dst, base + off, rem);
        memcpy(dst + rem, base, len - rem);
    } else {
        memcpy(dst, base + off, len);
    }
    return len;
}
