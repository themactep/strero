#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

#include "hal/platform.h"
#include "hal/imp.h"

/* ---------------- Platform capabilities (T20) ---------------- */
static const hal_caps_t g_caps = {
    .has_entropy_cabac = false,
    .has_set_bufsize   = false,
    .has_jpeg_qp       = false,
    .has_dmic          = false,
    .has_hevc          = false,
};

const hal_caps_t* hal_caps(void) {
    return &g_caps;
}

/* ---------------- Mapping helpers ---------------- */
static inline IMPPayloadType map_payload(hal_payload_t pt) {
    switch (pt) {
        case HAL_PT_JPEG: return PT_JPEG;
        case HAL_PT_H265: return PT_H264; /* No PT_H265 on T20; caps.has_hevc=false */
        case HAL_PT_H264:
        default:          return PT_H264;
    }
}

static inline IMPEncoderRcMode map_rc(hal_rc_mode_t rc) {
    switch (rc) {
        case HAL_RC_FIXQP: return ENC_RC_MODE_FIXQP;
        case HAL_RC_VBR:   return ENC_RC_MODE_VBR;
        case HAL_RC_CBR:
        default:           return ENC_RC_MODE_CBR;
    }
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

    IMPEncoderCHNAttr attr;
    memset(&attr, 0, sizeof(attr));

    /* Basic encoder attributes */
    attr.encAttr.enType   = map_payload(a->payload);
    attr.encAttr.bufSize  = 0; /* auto */
    attr.encAttr.profile  = (a->payload == HAL_PT_H264) ? 1 : 0; /* 1:MP for H.264 else baseline */
    attr.encAttr.picWidth  = a->width;
    attr.encAttr.picHeight = a->height;

    /* Rate-control attributes */
    attr.rcAttr.outFrmRate.frmRateNum = a->fps.num;
    attr.rcAttr.outFrmRate.frmRateDen = a->fps.den;
    attr.rcAttr.maxGop = a->gop;
    attr.rcAttr.attrRcMode.rcMode = map_rc(a->rc_mode);



    return IMP_Encoder_CreateChn(ch, &attr);
}

int hal_enc_destroy(int ch) {
    return IMP_Encoder_DestroyChn(ch);
}

int hal_enc_register(int group, int ch) {
    return IMP_Encoder_RegisterChn(group, ch);
}

int hal_enc_unregister(int group, int ch) {
    (void)group;
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

/* ---------------- Optional tuning (unsupported on T20) ---------------- */
int hal_enc_set_stream_bufcnt(int ch, int cnt) {
    (void)ch; (void)cnt;
    return 0; /* no-op */
}

int hal_enc_set_stream_bufsize(int ch, uint32_t bytes) {
    (void)ch; (void)bytes;
    return 0; /* no-op */
}

int hal_enc_set_entropy_cabac(int ch, int enable) {
    (void)ch; (void)enable;
    return 0; /* no-op */
}

int hal_enc_set_jpeg_qp(int ch, int qp) {
    (void)ch; (void)qp;
    return 0; /* no-op */
}

/* ---------------- Attribute query ---------------- */
int hal_enc_get_attr(int ch, hal_enc_attr_t *a) {
    if (!a) return -1;
    memset(a, 0, sizeof(*a));

    IMPEncoderCHNAttr attr;
    memset(&attr, 0, sizeof(attr));
    int ret = IMP_Encoder_GetChnAttr(ch, &attr);
    if (ret < 0) return ret;

    switch (attr.encAttr.enType) {
        case PT_JPEG: a->payload = HAL_PT_JPEG; break;
        default:      a->payload = HAL_PT_H264; break;
    }
    a->width   = attr.encAttr.picWidth;
    a->height  = attr.encAttr.picHeight;
    a->fps.num = attr.rcAttr.outFrmRate.frmRateNum;
    a->fps.den = attr.rcAttr.outFrmRate.frmRateDen;
    a->gop     = attr.rcAttr.maxGop;

    switch (attr.rcAttr.attrRcMode.rcMode) {
        case ENC_RC_MODE_FIXQP: a->rc_mode = HAL_RC_FIXQP; break;
        case ENC_RC_MODE_VBR:   a->rc_mode = HAL_RC_VBR;   break;
        default:                a->rc_mode = HAL_RC_CBR;   break;
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
    return st ? (int)st->packCount : 0;
}

uint32_t hal_stream_pack_length(const hal_stream_t *s, int index) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= (int)st->packCount) return 0;
    return st->pack[index].length;
}

uint64_t hal_stream_pack_timestamp_us(const hal_stream_t *s, int index) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= (int)st->packCount) return 0;
    return (uint64_t)st->pack[index].timestamp;
}

uint32_t hal_stream_copy_pack(const hal_stream_t *s, int index, uint8_t *dst) {
    const IMPEncoderStream *st = as_native(s);
    if (!st || index < 0 || index >= (int)st->packCount || !dst) return 0;
    const IMPEncoderPack *pk = &st->pack[index];
    memcpy(dst, (const void*)pk->virAddr, pk->length);
    return pk->length;
}
