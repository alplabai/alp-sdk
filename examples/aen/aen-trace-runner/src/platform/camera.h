/* src/platform/camera.h */
#ifndef TR_PLATFORM_CAMERA_H
#define TR_PLATFORM_CAMERA_H

#include <stddef.h>
#include <stdint.h>

int            tr_camera_open(void);             /* 0 on success, negative on failure */
const uint8_t *tr_camera_frame(size_t *len_out);  /* NULL when no frame is ready */
void           tr_camera_release(void);           /* returns the frame to the driver */
int16_t        tr_camera_width(void);
int16_t        tr_camera_height(void);

/* Stop the stream and release the handle. Safe to call even if
 * tr_camera_open() was never called or already closed (no-op then). For a
 * caller that opened the camera speculatively and then decided nothing will
 * ever consume a frame (e.g. the detector geometry didn't fit) -- stops the
 * CPI and frees it rather than leaving it streaming into the video pool for
 * the rest of the run. */
void tr_camera_close(void);

#endif /* TR_PLATFORM_CAMERA_H */
