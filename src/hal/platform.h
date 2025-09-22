#ifndef HAL_PLATFORM_H
#define HAL_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_entropy_cabac;     /* IMP_Encoder_SetChnEntropyMode available */
    bool has_set_bufsize;       /* IMP_Encoder_SetStreamBufSize available */
    bool has_jpeg_qp;           /* IMP_Encoder_SetChnQp for JPEG available */
    bool has_dmic;              /* Digital MIC (imp_dmic.h) available */
    bool has_hevc;              /* H.265 support */
} hal_caps_t;

/* Query platform capabilities */
const hal_caps_t* hal_caps(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_PLATFORM_H */

