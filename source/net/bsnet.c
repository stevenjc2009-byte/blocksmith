/* bsnet.c — public face of the multiplayer client.
 *
 * Holds the state machine and everything the menu reads back. The actual
 * socket work and Noise handshake live behind netTransport*() in
 * bsnet_transport.c, so this file stays readable and the menu never has to
 * know that any of that exists.
 *
 * Nothing here blocks. netUpdate() does a bounded amount of work and returns.
 */

#include "net/bsnet.h"
#include "net/bsnet_transport.h"
#include "net/networld.h"

#include <hydrogen.h>
#include <stdio.h>
#include <string.h>

/* Address baked into the build. Overridable without editing this file:
 *   make EXTRA_CFLAGS='-DBS_SERVER_ADDR=\"my-world.at.ply.gg:41234\"'
 *
 * Left empty by default on purpose. An empty address makes the multiplayer
 * screen say "(not configured)", which is the honest thing for a build that
 * was never pointed at a server — far better than silently trying to reach
 * a hostname somebody else owns. */
#ifndef BS_SERVER_ADDR
#define BS_SERVER_ADDR ""
#endif

/* Read in preference to the compiled-in address so a new playit hostname does
 * not mean a new CIA. One line, "host:port", '#' comments and blanks skipped. */
#define NET_ADDR_FILE  "sdmc:/blocksmith/server.txt"

struct net_player {
    char name[NET_NAME_MAX];
    bool local;
};

static struct {
    bool      inited;
    NetStatus status;

    char addr[NET_ADDR_MAX];
    char status_text[64];
    char error_text[96];

    struct net_player player[NET_MAX_PLAYERS];
    int               player_count;

    int ping_ms;

    /* This console's public key, resolved once in netInit() and cached as
     * plain lowercase hex — the draw code just wants a pointer, the same way
     * it wants status_text/error_text as ready-to-print strings. */
    char key_hex[2 * 32 + 1];
} s_net;

/* ------------------------------------------------------------ status text */

static void set_error(const char *msg)
{
    snprintf(s_net.error_text, sizeof s_net.error_text, "%s", msg);
}

/* Rebuilt whenever the status or the player count changes rather than
 * formatted on demand, so netStatusText() is a pointer read from draw code. */
static void refresh_status_text(void)
{
    switch (s_net.status) {
    case NET_IDLE:
        snprintf(s_net.status_text, sizeof s_net.status_text, "Not connected");
        break;
    case NET_CONNECTING:
        snprintf(s_net.status_text, sizeof s_net.status_text, "Connecting...");
        break;
    case NET_CONNECTED:
        snprintf(s_net.status_text, sizeof s_net.status_text,
                 "Connected - %d player%s", s_net.player_count,
                 s_net.player_count == 1 ? "" : "s");
        break;
    case NET_FAILED:
        snprintf(s_net.status_text, sizeof s_net.status_text,
                 "Could not connect");
        break;
    }
}

static void set_status(NetStatus st)
{
    s_net.status = st;
    refresh_status_text();
}

/* ------------------------------------------------------- address loading */

/* Trims trailing CR/LF and spaces in place. server.txt will be edited on a PC
 * more often than not, so CRLF is the common case, not the odd one. */
static void trim_tail(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'
                     || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool load_addr_from_sd(char *out, size_t cap)
{
    FILE *f = fopen(NET_ADDR_FILE, "r");
    if (f == NULL) return false;

    char line[NET_ADDR_MAX];
    bool got = false;

    while (!got && fgets(line, sizeof line, f) != NULL) {
        trim_tail(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        snprintf(out, cap, "%s", line);
        got = true;
    }

    fclose(f);
    return got;
}

/* Splits "host:port". Rejects anything without a port rather than guessing a
 * default: a silently wrong port looks exactly like an unreachable server. */
static bool split_addr(const char *addr, char *host, size_t host_cap, uint16_t *port)
{
    const char *colon = strrchr(addr, ':');
    if (colon == NULL || colon == addr) return false;

    size_t host_len = (size_t)(colon - addr);
    if (host_len >= host_cap) return false;

    memcpy(host, addr, host_len);
    host[host_len] = '\0';

    long p = 0;
    for (const char *c = colon + 1; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') return false;
        p = p * 10 + (*c - '0');
        if (p > 65535) return false;
    }
    if (p < 1) return false;

    *port = (uint16_t)p;
    return true;
}

/* ------------------------------------------------------------- lifecycle */

bool netInit(void)
{
    memset(&s_net, 0, sizeof s_net);
    set_status(NET_IDLE);
    s_net.ping_ms = -1;
    snprintf(s_net.key_hex, sizeof s_net.key_hex, "(unavailable)");

    if (!load_addr_from_sd(s_net.addr, sizeof s_net.addr)) {
        snprintf(s_net.addr, sizeof s_net.addr, "%s", BS_SERVER_ADDR);
    }
    if (s_net.addr[0] == '\0') {
        snprintf(s_net.addr, sizeof s_net.addr, "(not configured)");
    }

    if (!netTransportInit()) {
        set_error(netTransportError());
        return false;
    }

    /* Best-effort: a missing identity here just means the multiplayer screen
     * shows "(unavailable)" instead of a key, same posture as the seed-persist
     * and client.pub-write failures one layer down — never fails netInit(). */
    uint8_t pk[32];
    if (netTransportLocalPublicKey(pk)) {
        hydro_bin2hex(s_net.key_hex, sizeof s_net.key_hex, pk, sizeof pk);
    }

    s_net.inited = true;
    return true;
}

void netExit(void)
{
    if (s_net.inited) {
        netTransportDisconnect();
        netTransportExit();
    }
    memset(&s_net, 0, sizeof s_net);
}

/* --------------------------------------------------------------- session */

bool netConnect(void)
{
    if (!s_net.inited)                 return false;
    if (s_net.status == NET_CONNECTING) return false;
    if (s_net.status == NET_CONNECTED)  return false;

    char host[NET_ADDR_MAX];
    uint16_t port = 0;
    if (!split_addr(s_net.addr, host, sizeof host, &port)) {
        set_error("Server address is not valid - check server.txt");
        set_status(NET_FAILED);
        return false;
    }

    s_net.error_text[0] = '\0';
    s_net.player_count  = 0;
    s_net.ping_ms       = -1;

    if (!netTransportConnect(host, port)) {
        set_error(netTransportError());
        set_status(NET_FAILED);
        return false;
    }

    set_status(NET_CONNECTING);
    return true;
}

void netDisconnect(void)
{
    if (!s_net.inited) return;

    netTransportDisconnect();
    networldResetRemotes();
    s_net.player_count = 0;
    s_net.ping_ms      = -1;
    set_status(NET_IDLE);
}

void netUpdate(void)
{
    if (!s_net.inited) return;

    netTransportUpdate();

    NetTransportState ts = netTransportState();

    switch (ts) {
    case NET_TRANSPORT_IDLE:
        if (s_net.status != NET_FAILED) set_status(NET_IDLE);
        break;

    case NET_TRANSPORT_HANDSHAKING:
        set_status(NET_CONNECTING);
        break;

    case NET_TRANSPORT_ESTABLISHED: {
        s_net.ping_ms = netTransportPingMs();

        /* The server sends no roster and no join/leave notification — the
         * sid in a pose update is the only thing it ever tells a peer about
         * another player — so the list here is rebuilt every frame from
         * networld's remote table rather than from anything the server
         * labelled. Index 0 is always the local player; indices 1..n mirror
         * networldRemoteGet() order, named from the low 16 bits of the sid
         * since that is the only honest, collision-cheap tag available. */
        snprintf(s_net.player[0].name, NET_NAME_MAX, "%s", "You");
        s_net.player[0].local = true;

        int remote_count = networldRemoteCount();
        if (remote_count > NET_MAX_PLAYERS - 1) remote_count = NET_MAX_PLAYERS - 1;

        int n = 1;
        for (int i = 0; i < remote_count; i++) {
            NetworldRemote r;
            if (!networldRemoteGet(i, &r)) break;
            snprintf(s_net.player[n].name, NET_NAME_MAX, "Player %04x",
                     (unsigned int)(r.sid & 0xffffu));
            s_net.player[n].local = false;
            n++;
        }
        s_net.player_count = n;

        /* set_status() calls refresh_status_text() unconditionally, which is
         * what keeps "Connected - N players" tracking player_count above
         * even though this whole case runs every established frame. */
        set_status(NET_CONNECTED);
        break;
    }

    case NET_TRANSPORT_FAILED:
        set_error(netTransportError());
        networldResetRemotes();
        s_net.player_count = 0;
        s_net.ping_ms      = -1;
        set_status(NET_FAILED);
        break;
    }
}

/* ---------------------------------------------------------------- status */

NetStatus   netStatus(void)        { return s_net.status; }
const char *netStatusText(void)    { return s_net.status_text; }
const char *netErrorText(void)     { return s_net.error_text; }
const char *netServerAddress(void) { return s_net.addr; }
int         netPingMs(void)        { return s_net.ping_ms; }
int         netPlayerCount(void)   { return s_net.player_count; }

const char *netPlayerName(int index)
{
    if (index < 0 || index >= s_net.player_count) return NULL;
    return s_net.player[index].name;
}

bool netPlayerIsLocal(int index)
{
    if (index < 0 || index >= s_net.player_count) return false;
    return s_net.player[index].local;
}

/* --------------------------------------------------------------- identity */

const char *netLocalKeyHex(void) { return s_net.key_hex; }
