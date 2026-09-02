/* wstr_net.h - the console's side of the wire.
 *
 * Deliberately tiny and connectionless. The console never learns the PC's
 * address from a config file: it binds one UDP socket, waits for a HELLO, and
 * streams to whoever sent it. That is the whole pairing story - no IP to type
 * in, no rebuild when the PC's DHCP lease changes.
 */
#ifndef WSTR_NET_H
#define WSTR_NET_H

#include <stdint.h>
#include <stddef.h>
#include "wstr_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      fd;
    uint32_t peer_addr;      /* host byte order; 0 = nobody is listening   */
    uint16_t peer_port;
    uint32_t last_hello_ms;  /* for the idle timeout                       */

    /* Whatever the newest HELLO asked for. The streaming loop reads these
     * every frame, so moving a slider on the PC takes effect immediately. */
    uint8_t  quality;
    uint8_t  fps;
    uint16_t out_width;
    uint16_t out_height;
    uint8_t  want_audio;
    uint8_t  source;
    uint8_t  stage;          /* WSTR_STAGE_*, how far the capture may go   */
    uint8_t  alloc_mode;     /* WSTR_ALLOC_*, where surfaces come from     */
    uint8_t  sync_mode;      /* WSTR_SYNC_*, how to wait for the GPU       */
    uint8_t  tile_mode;      /* WSTR_TILE_*, capture surface tiling        */
    uint8_t  auto_quality;   /* 1 = adapt quality to hit the frame rate    */
    uint8_t  video_off;      /* 1 = audio only                            */
    uint8_t  audio_source;   /* WSTR_AUDIO_SRC_*                          */

    uint8_t  last_send_errno;  /* set when a sendto() failed   */
    uint16_t chunk_size;       /* payload bytes per datagram   */
    uint8_t  chunk_rung;       /* index into the size ladder   */
    uint16_t chunk_ok;         /* clean sends since the last drop */
    uint16_t chunk_retry;      /* clean sends needed before climbing */
    uint8_t  size_fails;       /* consecutive EMSGSIZE failures  */

    uint32_t seq_video;
    uint32_t seq_audio;
} wstr_net;

/* Bind the media socket. Returns 0 on success, -1 on failure. */
int  wstr_net_open(wstr_net *n, uint16_t port);
void wstr_net_close(wstr_net *n);

/* Drain any waiting HELLO/BYE packets and apply them. Non-blocking; call it
 * once per frame. Returns 1 if a peer is currently connected. */
int  wstr_net_poll(wstr_net *n, uint32_t now_ms);

/* True if a peer said hello recently enough to still be considered present. */
int  wstr_net_connected(const wstr_net *n, uint32_t now_ms);

/* Split a payload across MTU-sized chunks and send them. Returns 0 on success,
 * -1 if any send failed (the frame is then simply lost - it is a live stream,
 * and the next frame is a full JPEG anyway). */
int  wstr_net_send_frame(wstr_net *n, const uint8_t *data, size_t len,
                         uint16_t w, uint16_t h, uint8_t flags, uint32_t now_ms);

int  wstr_net_send_audio(wstr_net *n, const uint8_t *pcm, size_t len,
                         uint32_t now_ms);

int  wstr_net_send_status(wstr_net *n, const wstr_status_t *st, uint32_t now_ms);

/* Send one line of diagnostic text to the PC. Best-effort and lossy by
 * design: when the console locks up, the value of a log line is that it was
 * already on the wire before the call that killed it. */
int  wstr_net_log(wstr_net *n, const char *text, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* WSTR_NET_H */
