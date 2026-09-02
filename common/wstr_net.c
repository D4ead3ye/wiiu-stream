/* wstr_net.c - see wstr_net.h */

#include "wstr_net.h"
#include <string.h>

#ifdef __WIIU__
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <errno.h>
#else
#  ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    define close closesocket
     /* Winsock has no MSG_DONTWAIT; the host build makes the socket
      * non-blocking in wstr_net_open() instead. */
#    ifndef MSG_DONTWAIT
#      define MSG_DONTWAIT 0
#    endif
#  else
#    include <sys/socket.h>
#    include <netinet/in.h>
#    include <arpa/inet.h>
#    include <unistd.h>
#    include <errno.h>
#  endif
#endif

/* A peer that has not said hello for this long is gone - either the PC app was
 * closed or the console went to sleep. Three missed heartbeats. */
#define HELLO_TIMEOUT_MS 3500

/* Datagram sizes to try, largest first.
 *
 * Every sendto() on the console is a synchronous round trip into the network
 * stack and costs about 3.6 ms regardless of size, so an 11-datagram frame
 * spends 40 ms in sends alone - far more than the encode. Fewer, larger
 * datagrams is the fix, but the stack refuses anything it will not fragment,
 * and where that limit sits is not documented anywhere worth trusting.
 *
 * So it is measured rather than assumed: start at the top and step down on
 * the first refusal. The cost of guessing high is one dropped frame per rung,
 * once per session; the payoff is a third of the sends. */
/* 4072 is refused by the console and 2024 is accepted, so the real limit
 * sits between them - hence the extra rung, which is worth a few sends a
 * frame for nothing. */
static const uint16_t k_chunk_ladder[] = { 4072, 3048, 2024, 1376, 552 };
#define CHUNK_LADDER_LEN ((int)(sizeof(k_chunk_ladder) / sizeof(k_chunk_ladder[0])))

/* Big-endian writers. The Wii U is big-endian, so these are all no-ops there,
 * but writing them explicitly keeps the host-side test build honest. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void write_header(uint8_t *p, uint8_t type, uint8_t flags, uint32_t seq,
                         uint16_t idx, uint16_t cnt, uint16_t plen,
                         uint16_t w, uint16_t h, uint32_t now_ms)
{
    put_u32(p + 0,  WSTR_MAGIC);
    p[4] = WSTR_VERSION;
    p[5] = type;
    p[6] = flags;
    p[7] = 0;
    put_u32(p + 8,  seq);
    put_u16(p + 12, idx);
    put_u16(p + 14, cnt);
    put_u16(p + 16, plen);
    put_u16(p + 18, w);
    put_u16(p + 20, h);
    put_u16(p + 22, (uint16_t)(now_ms & 0xffff));
}

int wstr_net_open(wstr_net *n, uint16_t port)
{
    struct sockaddr_in addr;
    int sndbuf = 128 * 1024;

    memset(n, 0, sizeof(*n));
    n->fd = -1;

    /* Defaults, used until the first HELLO overrides them. */
    n->quality    = 70;
    n->fps        = 20;
    n->out_width  = 640;
    n->out_height = 360;
    /* Until a HELLO says otherwise, do nothing but listen. */
    n->stage      = WSTR_STAGE_OFF;
    n->chunk_rung = 0;
    n->chunk_size = k_chunk_ladder[0];
    n->chunk_retry = 600;
    n->size_fails = 0;

    n->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (n->fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(n->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(n->fd);
        n->fd = -1;
        return -1;
    }

    /* The Wii U's send buffer defaults small enough that a burst of chunks
     * for one frame can overrun it. Asking for more is best-effort - a
     * failure here just means more blocking in send(). */
    setsockopt(n->fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

#ifdef _WIN32
    {
        u_long nb = 1;
        ioctlsocket(n->fd, FIONBIO, &nb);
    }
#endif

    return 0;
}

void wstr_net_close(wstr_net *n)
{
    if (n->fd >= 0) {
        close(n->fd);
        n->fd = -1;
    }
    n->peer_addr = 0;
}

int wstr_net_poll(wstr_net *n, uint32_t now_ms)
{
    uint8_t buf[256];
    struct sockaddr_in from;
    socklen_t fromlen;

    if (n->fd < 0) return 0;

    /* Drain everything queued: only the newest HELLO matters, and leaving
     * them to pile up would make slider changes lag by seconds. */
    for (;;) {
        int r;
        fromlen = sizeof(from);
        r = (int)recvfrom(n->fd, (char *)buf, sizeof(buf), MSG_DONTWAIT,
                          (struct sockaddr *)&from, &fromlen);
        if (r < (int)WSTR_HDR_SIZE) break;
        if (get_u32(buf) != WSTR_MAGIC || buf[4] != WSTR_VERSION) continue;

        if (buf[5] == WSTR_PKT_BYE) {
            n->peer_addr = 0;
            continue;
        }
        if (buf[5] != WSTR_PKT_HELLO) continue;
        if (r < (int)(WSTR_HDR_SIZE + sizeof(wstr_hello_t))) continue;

        {
            const uint8_t *pl = buf + WSTR_HDR_SIZE;
            uint16_t pc_port = get_u16(pl + 0);

            n->peer_addr     = ntohl(from.sin_addr.s_addr);
            n->peer_port     = pc_port ? pc_port : WSTR_PORT_PC;
            n->last_hello_ms = now_ms;

            n->quality    = pl[2] ? pl[2] : 70;
            n->fps        = pl[3] ? pl[3] : 20;
            n->out_width  = get_u16(pl + 4);
            n->out_height = get_u16(pl + 6);
            n->want_audio = pl[8];
            n->source     = pl[9];
            n->stage      = pl[10];
            n->alloc_mode = pl[11];
            n->sync_mode  = pl[12];
            n->tile_mode  = pl[13];
            n->auto_quality = pl[14];
            n->video_off    = pl[15];
            n->audio_source = pl[16];
        }
    }

    return wstr_net_connected(n, now_ms);
}

int wstr_net_connected(const wstr_net *n, uint32_t now_ms)
{
    if (n->fd < 0 || n->peer_addr == 0) return 0;
    return (uint32_t)(now_ms - n->last_hello_ms) < HELLO_TIMEOUT_MS;
}

static int send_to_peer(wstr_net *n, const uint8_t *pkt, size_t len)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons(n->peer_port);
    to.sin_addr.s_addr = htonl(n->peer_addr);
    return (int)sendto(n->fd, (const char *)pkt, len, 0,
                       (struct sockaddr *)&to, sizeof(to));
}

static int send_chunked(wstr_net *n, uint8_t type, uint8_t flags, uint32_t seq,
                        const uint8_t *data, size_t len,
                        uint16_t w, uint16_t h, uint32_t now_ms)
{
    uint8_t pkt[WSTR_MTU];
    size_t off = 0;
    uint16_t idx = 0, cnt;
    size_t chunk = n->chunk_size ? n->chunk_size : WSTR_PAYLOAD_DEF;

    if (n->fd < 0 || n->peer_addr == 0 || len == 0) return -1;
    if (chunk > WSTR_PAYLOAD_MAX) chunk = WSTR_PAYLOAD_MAX;

    cnt = (uint16_t)((len + chunk - 1) / chunk);

    while (off < len) {
        size_t plen = len - off;
        if (plen > chunk) plen = chunk;

        write_header(pkt, type, flags, seq, idx, cnt, (uint16_t)plen, w, h, now_ms);
        memcpy(pkt + WSTR_HDR_SIZE, data + off, plen);

        if (send_to_peer(n, pkt, WSTR_HDR_SIZE + plen) < 0) {
            /* Why the send failed decides what to do about it.
             *
             * EMSGSIZE means this datagram is genuinely too large for the
             * path, and the only answer is a smaller one. A full send buffer
             * or a busy link is something else entirely: transient, and gone
             * by the next frame. Treating the two alike is what had the
             * console thrashing down to 552-byte datagrams and back, losing a
             * frame to a failed probe every few seconds - 8% of them across
             * one session.
             *
             * Reported rather than swallowed either way: a silently dropped
             * frame looks exactly like a slow encoder from the PC side. */
            int err = errno;
            n->last_send_errno = (uint8_t)(err > 255 ? 255 : err);

            if (err == EMSGSIZE) {
                n->size_fails++;
            } else {
                n->size_fails = 0;
            }

            /* Two in a row before stepping down, so a single odd failure
             * that happens to report EMSGSIZE does not cost a rung. */
            if (n->size_fails >= 2 && n->chunk_rung + 1 < CHUNK_LADDER_LEN) {
                n->chunk_rung++;
                n->chunk_size = k_chunk_ladder[n->chunk_rung];
                n->size_fails = 0;
                /* Each failed rung makes the next attempt at it rarer. A size
                 * the path refuses now will probably still refuse it soon. */
                if (n->chunk_retry < 30000) n->chunk_retry *= 4;
            }
            n->chunk_ok = 0;
            return -1;
        }

        off += plen;
        idx++;
    }

    /* Climb back after a long clean run.
     *
     * The step down is triggered by a single refusal, which can be transient -
     * a busy moment on the link rather than a hard limit. Without a way back
     * up, one such moment costs every remaining frame of the session: the
     * console was seen sitting at 552 byte datagrams, six times the sends it
     * needed. Retrying costs one dropped frame if the rung is genuinely
     * unavailable, so the interval grows every time a climb fails rather than
     * probing at a fixed rate forever. */
    n->size_fails = 0;
    if (n->chunk_rung > 0 && ++n->chunk_ok >= n->chunk_retry) {
        n->chunk_ok = 0;
        n->chunk_rung--;
        n->chunk_size = k_chunk_ladder[n->chunk_rung];
    }
    return 0;
}

int wstr_net_send_frame(wstr_net *n, const uint8_t *data, size_t len,
                        uint16_t w, uint16_t h, uint8_t flags, uint32_t now_ms)
{
    int r = send_chunked(n, WSTR_PKT_VIDEO, (uint8_t)(flags | WSTR_FLAG_KEY),
                         n->seq_video, data, len, w, h, now_ms);
    n->seq_video++;
    return r;
}

int wstr_net_send_audio(wstr_net *n, const uint8_t *pcm, size_t len,
                        uint32_t now_ms)
{
    int r = send_chunked(n, WSTR_PKT_AUDIO, 0, n->seq_audio, pcm, len, 0, 0, now_ms);
    n->seq_audio++;
    return r;
}

int wstr_net_send_status(wstr_net *n, const wstr_status_t *st, uint32_t now_ms)
{
    uint8_t pkt[WSTR_HDR_SIZE + sizeof(wstr_status_t)];
    uint8_t *p = pkt + WSTR_HDR_SIZE;

    if (n->fd < 0 || n->peer_addr == 0) return -1;

    write_header(pkt, WSTR_PKT_STATUS, 0, 0, 0, 1,
                 (uint16_t)sizeof(wstr_status_t), 0, 0, now_ms);

    put_u16(p + 0,  st->src_width);
    put_u16(p + 2,  st->src_height);
    put_u16(p + 4,  st->out_width);
    put_u16(p + 6,  st->out_height);
    p[8]  = st->fps_actual;
    p[9]  = st->quality;
    p[10] = st->audio_on;
    p[11] = st->source;
    put_u32(p + 12, st->encode_us);
    put_u32(p + 16, st->capture_us);

    return send_to_peer(n, pkt, sizeof(pkt)) < 0 ? -1 : 0;
}

int wstr_net_log(wstr_net *n, const char *text, uint32_t now_ms)
{
    uint8_t pkt[WSTR_HDR_SIZE + 192];
    size_t len = strlen(text);

    if (n->fd < 0 || n->peer_addr == 0) return -1;
    if (len > sizeof(pkt) - WSTR_HDR_SIZE) len = sizeof(pkt) - WSTR_HDR_SIZE;

    write_header(pkt, WSTR_PKT_LOG, 0, 0, 0, 1, (uint16_t)len, 0, 0, now_ms);
    memcpy(pkt + WSTR_HDR_SIZE, text, len);

    return send_to_peer(n, pkt, WSTR_HDR_SIZE + len) < 0 ? -1 : 0;
}
