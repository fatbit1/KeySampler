/*
 * grpc_lite.c - just enough gRPC (HTTP/2 + protobuf) for KeySampler to talk to
 * sushi's control API on localhost:51051, the same API dspd uses.
 * Unary calls only, plaintext HTTP/2, no compression. Used from the loader
 * thread only (never from the audio thread).
 *
 * Message layouts are taken from the sushi_rpc.proto descriptor embedded in
 * the DSP Card firmware 3.1.12 (dspd).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

typedef struct {
    int      fd;
    uint32_t next_stream;
    char     host[64];
    int      port;
} GrpcConn;

/* ---------------- socket helpers */

static int g_write_all(int fd, const uint8_t* p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int g_read_all(int fd, uint8_t* p, size_t n)
{
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int g_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream, const uint8_t* pl, uint32_t len)
{
    uint8_t h[9] = { (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type, flags,
                     (uint8_t)(stream >> 24) & 0x7F, (uint8_t)(stream >> 16), (uint8_t)(stream >> 8), (uint8_t)stream };
    if (g_write_all(fd, h, 9)) return -1;
    return len ? g_write_all(fd, pl, len) : 0;
}

static void grpc_close(GrpcConn* c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int grpc_open(GrpcConn* c)
{
    if (c->fd >= 0) return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 0, 500000 }; /* 0.5 s per read/write */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)c->port);
    a.sin_addr.s_addr = inet_addr(c->host);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    /* our SETTINGS: a large initial window so responses never stall */
    uint8_t settings[6] = { 0x00, 0x04, 0x00, 0x10, 0x00, 0x00 }; /* INITIAL_WINDOW_SIZE = 1 MB */
    uint8_t wu[4] = { 0x00, 0x0F, 0x00, 0x00 };                   /* connection window +983040 */
    if (g_write_all(fd, (const uint8_t*)preface, sizeof preface - 1) ||
        g_frame(fd, 0x4, 0, 0, settings, 6) || g_frame(fd, 0x8, 0, 0, wu, 4)) { close(fd); return -1; }
    c->fd = fd;
    c->next_stream = 1;
    return 0;
}

static size_t hp_lit(uint8_t* p, uint8_t first, const char* v)
{
    size_t n = strlen(v), k = 0;
    p[k++] = first;
    p[k++] = (uint8_t)n; /* < 127, no Huffman */
    memcpy(p + k, v, n);
    return k + n;
}

/* One unary call. resp gets the protobuf payload (without the 5-byte prefix).
 * Returns payload length, or -1 on any error. */
static int grpc_call(GrpcConn* c, const char* path, const uint8_t* req, uint32_t req_len,
                     uint8_t* resp, uint32_t resp_cap)
{
    if (grpc_open(c)) return -1;
    uint32_t sid = c->next_stream;
    c->next_stream += 2;

    uint8_t hb[512];
    size_t k = 0;
    hb[k++] = 0x83;                                   /* :method POST */
    hb[k++] = 0x86;                                   /* :scheme http */
    k += hp_lit(hb + k, 0x04, path);                  /* :path (literal, indexed name 4) */
    char auth[96];
    snprintf(auth, sizeof auth, "%s:%d", c->host, c->port);
    k += hp_lit(hb + k, 0x01, auth);                  /* :authority (indexed name 1) */
    hb[k++] = 0x0F; hb[k++] = 0x10;                   /* content-type (indexed name 31) */
    hb[k++] = 16; memcpy(hb + k, "application/grpc", 16); k += 16;
    hb[k++] = 0x00;                                   /* te: trailers (new name) */
    hb[k++] = 2; memcpy(hb + k, "te", 2); k += 2;
    hb[k++] = 8; memcpy(hb + k, "trailers", 8); k += 8;

    uint8_t db[1024];
    if (req_len + 5 > sizeof db) return -1;
    db[0] = 0;
    db[1] = (uint8_t)(req_len >> 24); db[2] = (uint8_t)(req_len >> 16); db[3] = (uint8_t)(req_len >> 8); db[4] = (uint8_t)req_len;
    memcpy(db + 5, req, req_len);
    if (g_frame(c->fd, 0x1, 0x4, sid, hb, (uint32_t)k) ||          /* HEADERS, END_HEADERS */
        g_frame(c->fd, 0x0, 0x1, sid, db, req_len + 5)) {          /* DATA, END_STREAM */
        grpc_close(c); return -1;
    }

    static uint8_t body[65536];
    uint32_t got = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        uint8_t h[9];
        if (g_read_all(c->fd, h, 9)) { grpc_close(c); return -1; }
        uint32_t len = (uint32_t)h[0] << 16 | (uint32_t)h[1] << 8 | h[2];
        uint8_t type = h[3], flags = h[4];
        uint32_t st = ((uint32_t)(h[5] & 0x7F) << 24) | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 8 | h[8];
        static uint8_t pl[65536];
        if (len > sizeof pl) { grpc_close(c); return -1; }
        if (len && g_read_all(c->fd, pl, len)) { grpc_close(c); return -1; }

        if (type == 0x0) {                                       /* DATA */
            if (st == sid) {
                uint32_t off = (flags & 0x8) ? 1u + pl[0] : 0u;  /* PADDED */
                uint32_t pad = (flags & 0x8) ? pl[0] : 0u;
                uint32_t n = len - off - pad;
                if (got + n <= sizeof body) { memcpy(body + got, pl + off, n); got += n; }
            }
            if (len) { /* give the flow-control credit back */
                uint8_t inc[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
                g_frame(c->fd, 0x8, 0, 0, inc, 4);
            }
            if (st == sid && (flags & 0x1)) break;
        } else if (type == 0x1) {                                /* HEADERS (response / trailers) */
            if (st == sid && (flags & 0x1)) break;
        } else if (type == 0x3) {                                /* RST_STREAM */
            if (st == sid) return -1;
        } else if (type == 0x4) {                                /* SETTINGS */
            if (!(flags & 0x1)) g_frame(c->fd, 0x4, 0x1, 0, NULL, 0);
        } else if (type == 0x6) {                                /* PING */
            if (!(flags & 0x1)) g_frame(c->fd, 0x6, 0x1, 0, pl, len);
        } else if (type == 0x7) {                                /* GOAWAY */
            grpc_close(c); return -1;
        }
    }
    if (got < 5) return -1; /* trailers-only reply = error status */
    uint32_t mlen = (uint32_t)body[1] << 24 | (uint32_t)body[2] << 16 | (uint32_t)body[3] << 8 | body[4];
    if (body[0] != 0 || mlen + 5 > got || mlen > resp_cap) return -1;
    memcpy(resp, body + 5, mlen);
    return (int)mlen;
}

/* ---------------- protobuf helpers */

static size_t pb_varint(uint8_t* p, uint64_t v)
{
    size_t n = 0;
    do { uint8_t b = v & 0x7F; v >>= 7; p[n++] = b | (v ? 0x80 : 0); } while (v);
    return n;
}

static int pb_read_varint(const uint8_t* p, size_t len, size_t* pos, uint64_t* out)
{
    uint64_t r = 0; int s = 0;
    while (*pos < len && s < 64) {
        uint8_t b = p[(*pos)++];
        r |= (uint64_t)(b & 0x7F) << s; s += 7;
        if (!(b & 0x80)) { *out = r; return 0; }
    }
    return -1;
}

/* iterate fields: returns 0 and fills field/wire/value or sub-slice; -1 at end/error */
typedef struct { int field, wire; uint64_t v; const uint8_t* sub; size_t sublen; float f; } PbField;

static int pb_next(const uint8_t* p, size_t len, size_t* pos, PbField* f)
{
    uint64_t tag;
    if (*pos >= len || pb_read_varint(p, len, pos, &tag)) return -1;
    f->field = (int)(tag >> 3); f->wire = (int)(tag & 7); f->sub = NULL; f->sublen = 0;
    switch (f->wire) {
    case 0: return pb_read_varint(p, len, pos, &f->v);
    case 1: if (*pos + 8 > len) return -1; *pos += 8; return 0;
    case 2: {
        uint64_t l;
        if (pb_read_varint(p, len, pos, &l) || *pos + l > len) return -1;
        f->sub = p + *pos; f->sublen = (size_t)l; *pos += (size_t)l; return 0;
    }
    case 5: if (*pos + 4 > len) return -1; memcpy(&f->f, p + *pos, 4); f->v = 0; *pos += 4; return 0;
    }
    return -1;
}

/* ParameterIdentifier { int32 processor_id = 1; int32 parameter_id = 2; } */
static size_t pb_param_id(uint8_t* p, int proc, int param)
{
    size_t n = 0;
    p[n++] = 0x08; n += pb_varint(p + n, (uint64_t)(uint32_t)proc);
    p[n++] = 0x10; n += pb_varint(p + n, (uint64_t)(uint32_t)param);
    return n;
}
