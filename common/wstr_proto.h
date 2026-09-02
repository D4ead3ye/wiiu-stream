/* wstr_proto.h - wire format shared by the Wii U side and the PC receiver.
 *
 * Everything on the wire is big-endian ("network order"), which is the Wii U's
 * native order - so the console never byte-swaps and the PC uses struct '>'.
 *
 * Discovery is PC-driven: the console sits on WSTR_PORT_CONSOLE waiting for a
 * HELLO, then streams to whatever address that HELLO came from until the
 * HELLOs stop. That means there is no IP to configure on the console.
 */
#ifndef WSTR_PROTO_H
#define WSTR_PROTO_H

#include <stdint.h>

#define WSTR_MAGIC        0x57535452u   /* 'WSTR' */
#define WSTR_VERSION      1

#define WSTR_PORT_CONSOLE 41414         /* console listens here for HELLO   */
#define WSTR_PORT_PC      41415         /* PC listens here for media        */

/* Payload per datagram.
 *
 * One Ethernet MTU, minus room for the IPv4 and UDP headers. Larger datagrams
 * were tried, to cut the number of sendto() calls per frame - and the console
 * refused them outright: its network stack will not fragment a UDP datagram,
 * so anything over an MTU fails on every send. The buffer is still sized for
 * the larger value so the runtime size below has somewhere to grow, but the
 * default has to fit in one packet.
 *
 * WSTR_PAYLOAD_MIN is the floor the adaptive fallback will not go below; a
 * path that cannot carry 1 KB cannot carry video either. */
#define WSTR_MTU          8192
#define WSTR_HDR_SIZE     24
#define WSTR_PAYLOAD_MAX  (WSTR_MTU - WSTR_HDR_SIZE)
#define WSTR_PAYLOAD_DEF  (1400 - WSTR_HDR_SIZE)
#define WSTR_PAYLOAD_MIN  (576 - WSTR_HDR_SIZE)

/* packet types */
#define WSTR_PKT_VIDEO    1   /* one chunk of a JPEG frame                  */
#define WSTR_PKT_AUDIO    2   /* one block of interleaved S16BE stereo      */
#define WSTR_PKT_HELLO    3   /* PC -> console: "stream to me, like this"   */
#define WSTR_PKT_STATUS   4   /* console -> PC: heartbeat / capabilities    */
#define WSTR_PKT_BYE      5   /* PC -> console: stop now, don't wait to time out */
#define WSTR_PKT_LOG      6   /* console -> PC: one line of diagnostic text  */

/* header flags */
#define WSTR_FLAG_KEY     0x01  /* video: every JPEG frame is standalone    */
#define WSTR_FLAG_DRC     0x02  /* video: this frame is the GamePad screen  */

/* Fixed 24-byte header in front of every packet. Field meanings that vary by
 * packet type are noted; unused fields are zero. */
typedef struct {
    uint32_t magic;         /* WSTR_MAGIC                                   */
    uint8_t  version;       /* WSTR_VERSION                                 */
    uint8_t  type;          /* WSTR_PKT_*                                   */
    uint8_t  flags;         /* WSTR_FLAG_*                                  */
    uint8_t  reserved;
    uint32_t seq;           /* video/audio: frame or block number           */
    uint16_t chunk_index;   /* 0-based                                      */
    uint16_t chunk_count;   /* total chunks in this frame/block             */
    uint16_t payload_len;   /* bytes of payload following this header       */
    uint16_t width;         /* video: frame width  (0 otherwise)            */
    uint16_t height;        /* video: frame height (0 otherwise)            */
    uint16_t timestamp_ms;  /* low 16 bits of the console's millisecond clock*/
} __attribute__((packed)) wstr_header_t;

/* HELLO payload: the PC's requested settings. Sent about once a second; the
 * console applies whatever the newest one says, so the sliders in the UI take
 * effect live without any handshake. */
typedef struct {
    uint16_t pc_port;       /* where to send media (normally WSTR_PORT_PC)  */
    uint8_t  quality;       /* JPEG quality, 1..100                         */
    uint8_t  fps;           /* target frames per second, 1..60              */
    uint16_t out_width;     /* requested encode size; 0 = console picks     */
    uint16_t out_height;
    uint8_t  want_audio;    /* 1 = also send WSTR_PKT_AUDIO                 */
    uint8_t  source;        /* 0 = TV, 1 = GamePad                          */
    uint8_t  stage;         /* WSTR_STAGE_*, see below                      */
    uint8_t  alloc_mode;    /* WSTR_ALLOC_*, see below                      */
    uint8_t  sync_mode;     /* WSTR_SYNC_*, see below                       */
    uint8_t  tile_mode;     /* WSTR_TILE_*, see below                       */
    uint8_t  auto_quality;  /* 1 = console may lower quality to hit fps      */
    uint8_t  video_off;     /* 1 = audio only; skip capture entirely         */
    uint8_t  audio_source;  /* WSTR_AUDIO_SRC_*                              */
} __attribute__((packed)) wstr_hello_t;

/* Which mixer to capture. The console feeds the TV and the GamePad from
 * separate device mixes, and they are not the same audio - the GamePad often
 * carries menu and voice audio the TV does not, and a title playing through
 * one may leave the other silent. */
#define WSTR_AUDIO_SRC_TV   0
#define WSTR_AUDIO_SRC_DRC  1

/* Where the capture surfaces are allocated from.
 *
 * The game's default heap is the obvious choice - it is MEM2, which the GPU
 * can address - but a title that installed its own allocator means calling
 * that title's code to get it. The plugin's own heap avoids the game entirely
 * but may not be memory the GPU can write to. Both are one-line changes and
 * neither can be reasoned about from here, so the PC picks. */
#define WSTR_ALLOC_GAME_HEAP   0  /* MEMAllocFromDefaultHeapEx              */
#define WSTR_ALLOC_PLUGIN_HEAP 1  /* memalign() in the plugin's own heap    */

/* How the CPU waits for the GPU to finish the capture copy.
 *
 * GX2DrawDone() drains the entire pipeline, and calling it from inside the
 * game's own GX2CopyColorBufferToScanBuffer - mid-frame, while the game is
 * still building its command buffer - is what froze the console at the
 * readback stage. The timestamp path waits for our one submission instead of
 * everything, and does it on the encoder thread where a stall costs the game
 * nothing. */
#define WSTR_SYNC_TIMESTAMP 0  /* GX2WaitTimeStamp on the worker thread     */
#define WSTR_SYNC_NONE      1  /* no wait at all - may read a torn frame    */
#define WSTR_SYNC_DRAWDONE  2  /* GX2DrawDone on the render thread (froze)  */

/* What the capture surface is tiled as - i.e. what GX2CopySurface has to
 * convert the game's render target into.
 *
 * GX2_TILE_MODE_LINEAR_SPECIAL is 16, outside the 0-15 range the hardware's
 * tile modes occupy: it is a driver construct rather than something the GPU
 * understands, and the evidence is that the driver untiles into it on the CPU.
 * That is 3.6 MB of software detiling per capture, on the game's render
 * thread. LINEAR_ALIGNED is a real hardware mode the GPU blits to natively and
 * is still row-major for the CPU to read afterwards. */
#define WSTR_TILE_LINEAR_ALIGNED 0  /* GX2_TILE_MODE_LINEAR_ALIGNED  (1)     */
#define WSTR_TILE_LINEAR_SPECIAL 1  /* GX2_TILE_MODE_LINEAR_SPECIAL (16)     */

/* How far into the capture path the console is allowed to go.
 *
 * A hard freeze on the console gives you no stack trace and no core dump -
 * only the fact that it stopped. Each of these steps is a plausible cause and
 * each one needs different code, so rather than guess and reflash, the PC
 * picks how far to go and the setting takes effect on the next HELLO. Walking
 * the stages up until the console stops identifies the culprit exactly,
 * without a single reboot between attempts.
 *
 * Ordered by when they happen in on_scanout(). */
#define WSTR_STAGE_OFF      0  /* receive HELLO, do nothing else            */
#define WSTR_STAGE_SEND     1  /* + status and log packets from the render thread */
#define WSTR_STAGE_ALLOC    2  /* + allocate the capture surfaces           */
#define WSTR_STAGE_COPY     3  /* + GX2CopySurface, but never wait or read  */
#define WSTR_STAGE_READBACK 4  /* + GX2DrawDone and GX2Invalidate           */
#define WSTR_STAGE_FULL     5  /* + downscale, encode and send frames       */

/* STATUS payload: what the console is actually managing, so the UI can show
 * the difference between "asked for 30fps" and "getting 30fps". */
typedef struct {
    uint16_t src_width;     /* the game's real framebuffer size             */
    uint16_t src_height;
    uint16_t out_width;     /* what it is encoding at                       */
    uint16_t out_height;
    uint8_t  fps_actual;    /* frames encoded in the last second            */
    uint8_t  quality;
    uint8_t  audio_on;
    uint8_t  source;
    uint32_t encode_us;     /* mean microseconds per encode, last second    */
    uint32_t capture_us;    /* mean microseconds per GPU readback           */
} __attribute__((packed)) wstr_status_t;

/* Audio is raw interleaved 16-bit signed stereo, BIG-ENDIAN like every other
 * field in this protocol - the console is big-endian, so leaving the samples
 * in native order costs it nothing and the PC swaps them for free.
 *
 * Worth stating loudly because getting it wrong is not subtle and does not
 * look like an endianness bug: a quiet sample of 157 (0x009D) read the wrong
 * way round becomes 0x9D00, or -25344. The result is full-scale noise with the
 * real audio faintly audible underneath it, which reads as a broken capture
 * rather than a byte order mistake. Uncompressed is
 * ~1.5 Mbit/s, which is nothing next to the video, and it saves putting a
 * codec on a 1.24 GHz PowerPC that is already spending its time on DCTs. */
#define WSTR_AUDIO_RATE      48000
#define WSTR_AUDIO_CHANNELS  2
/* 15 ms per packet: 720*2*2 = 2880 bytes, which still fits in a single
 * datagram at the size the console settled on.
 *
 * This was 5 ms, which is a fine latency figure and a terrible send count:
 * 200 packets a second, each one a synchronous trip into the console's network
 * stack at about 1.4 ms, is 280 ms of every second spent on audio alone. It
 * showed up as the video collapsing to 9 fps whenever audio was switched on. */
#define WSTR_AUDIO_FRAMES    720

#endif /* WSTR_PROTO_H */
