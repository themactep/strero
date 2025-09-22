#ifndef HAL_IMP_H
#define HAL_IMP_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Unified payload and RC definitions */
typedef enum {
    HAL_PT_H264 = 0,
    HAL_PT_H265 = 1,
    HAL_PT_JPEG = 2,
} hal_payload_t;

typedef enum {
    HAL_RC_FIXQP = 0,
    HAL_RC_CBR   = 1,
    HAL_RC_VBR   = 2,
} hal_rc_mode_t;

typedef struct { uint16_t num, den; } hal_fps_t;

/* Unified encoder channel attributes */
typedef struct {
    hal_payload_t payload;     /* H264/H265/JPEG */
    uint16_t      width;
    uint16_t      height;
    hal_fps_t     fps;         /* num/den */
    uint16_t      gop;         /* GOP length in frames */
    hal_rc_mode_t rc_mode;     /* RC mode */
    uint32_t      target_kbps; /* target bitrate in kbps (for CBR/VBR) */
    int8_t        init_qp;     /* initial QP (for FIXQP), -1 to ignore */
    uint8_t       profile;     /* optional profile hint (SDK-specific), 0 for default */
} hal_enc_attr_t;

/* Opaque stream wrapper (platform-specific inside implementation). The
 * implementation allocates native stream storage on hal_stream_get() and
 * frees it on hal_stream_release().
 */
typedef struct hal_stream {
    void   *impl; /* pointer to native IMPEncoderStream */
} hal_stream_t;

/* Encoder lifecycle */
int hal_enc_create_group(int group);
int hal_enc_destroy_group(int group);
int hal_enc_create(int ch, const hal_enc_attr_t *attr);
int hal_enc_destroy(int ch);
int hal_enc_register(int group, int ch);
int hal_enc_unregister(int group, int ch);
int hal_enc_start(int ch);
int hal_enc_stop(int ch);
int hal_enc_request_idr(int ch);

/* Optional tuning (no-op if unsupported as per hal_caps) */
int hal_enc_set_stream_bufcnt(int ch, int cnt);
int hal_enc_set_stream_bufsize(int ch, uint32_t bytes);
int hal_enc_set_entropy_cabac(int ch, int enable);
int hal_enc_set_jpeg_qp(int ch, int qp);

/* Attribute query */
int hal_enc_get_attr(int ch, hal_enc_attr_t *attr);

/* Stream APIs */
int hal_stream_poll(int ch, int timeout_ms);
int hal_stream_get(int ch, hal_stream_t *s, int block);
int hal_stream_release(int ch, hal_stream_t *s);
int hal_stream_pack_count(const hal_stream_t *s);
uint32_t hal_stream_pack_length(const hal_stream_t *s, int index);
uint64_t hal_stream_pack_timestamp_us(const hal_stream_t *s, int index);
/* Copy pack data to dst (handles wrap-around/pack layout internally); returns bytes copied */
uint32_t hal_stream_copy_pack(const hal_stream_t *s, int index, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* HAL_IMP_H */

