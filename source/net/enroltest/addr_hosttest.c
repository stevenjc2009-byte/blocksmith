/* addr_hosttest.c — host test for how the client finds its server address.
 *
 * The bug this exists for: a console with no sdmc:/blocksmith/server.txt shows
 * "Server address is not valid - check server.txt" on every CONNECT, and there
 * is no server.txt to check, because nothing had ever created one. The player
 * is told to look at a file that does not exist, in a directory the game never
 * names, in a format the game never states.
 *
 * Every check here is written so it can go red. The red-arm procedure is in the
 * log entry; the short version is that reverting skip_bom() must fail case 4,
 * reverting write_addr_template() must fail cases 1 and 2, and collapsing the
 * switch in begin_connect() back to one string must fail cases 1, 2 and 3.
 *
 * bsnet.c hardcodes "sdmc:/blocksmith/server.txt" rather than taking BS_NET_DIR,
 * so this runs with its cwd set to a scratch directory that contains a real
 * directory named "sdmc:" — legal on Linux, and it means the shipping path is
 * exercised verbatim instead of being #ifdef'd for the test.
 */

#include "net/bsnet.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ADDR_WORKDIR
#define ADDR_WORKDIR "/tmp/bsnet_addrtest"
#endif

#ifndef BS_NET_DIR
#define BS_NET_DIR "/tmp/bsnet_enroltest/client"
#endif

#define SD_DIR   "sdmc:/blocksmith"
#define SD_FILE  SD_DIR "/server.txt"

static int checks = 0;
static int failures = 0;

static void ok(bool cond, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (!cond) failures++;
    printf(cond ? "  ok   " : "  FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void run(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd) != 0) {
        fprintf(stderr, "setup command failed: %s\n", cmd);
        exit(2);
    }
}

/* Writes bytes verbatim — no trailing newline is added, because "what exactly
 * is in the file" is the whole subject of this test. */
static void write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "short write on %s\n", path);
        exit(2);
    }
    fclose(f);
}

#ifndef BAKED_ADDR_TEST
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len != NULL) *out_len = n;
    return buf;
}
#endif

/* One clean slate per case: no server.txt, no client identity, nothing carried
 * over from the case before.
 *
 * BS_NET_DIR is an absolute path baked into bsnet_transport.o and is shared with
 * the enrolment suite, which leaves a server.pub and a network.psk behind in it.
 * `make run` runs that suite first, so without this wipe the key checks below
 * read another test's leftovers and pass no matter what this build contains —
 * which is exactly what happened the first time they were written. */
static void reset_sd(void)
{
    run("rm -rf '%s' '%s'", ADDR_WORKDIR, BS_NET_DIR);
    run("mkdir -p '%s/sdmc:/blocksmith' '%s/client' '%s'",
        ADDR_WORKDIR, ADDR_WORKDIR, BS_NET_DIR);
    if (chdir(ADDR_WORKDIR) != 0) {
        fprintf(stderr, "cannot chdir to %s: %s\n", ADDR_WORKDIR, strerror(errno));
        exit(2);
    }
}

/* ------------------------------------------------------------------ cases -- */
#ifndef BAKED_ADDR_TEST

/* The exact failure steve hit: a card with no server.txt on it. */
static void case_missing_file(void)
{
    printf("\ncase 1: no server.txt at all\n");
    reset_sd();

    ok(access(SD_FILE, F_OK) != 0, "precondition: %s does not exist", SD_FILE);
    ok(netInit(), "netInit() succeeds with no address configured");

    /* netConnect() must refuse, and must say something the player can act on
     * without a PC. */
    ok(!netConnect(), "netConnect() refuses an unconfigured address");
    printf("       error text: \"%s\"\n", netErrorText());
    ok(strcmp(netErrorText(), "Created /blocksmith/server.txt - add host:port") == 0,
       "error names the file it just created and the format");

    /* The template is the actual fix — the message is only useful if the file
     * is really sitting on the card afterwards. */
    ok(access(SD_FILE, F_OK) == 0, "%s now exists", SD_FILE);

    size_t len = 0;
    const char *body = read_file(SD_FILE, &len);
    ok(body != NULL && len > 0, "the template is not empty (%zu bytes)", len);
    ok(body != NULL && strstr(body, "host:port") != NULL,
       "the template states the host:port format");
    ok(body != NULL && strstr(body, "at.ply.gg") != NULL,
       "the template carries a worked example");
    ok(body != NULL && strstr(body, "\r\n") != NULL,
       "the template uses CRLF so Notepad renders it as lines");

    /* A template full of comments is still not an address: the second run must
     * not claim the file was just created, or the advice loops forever. */
    netExit();
    ok(netInit(), "netInit() succeeds on the second run");
    ok(!netConnect(), "netConnect() still refuses");
    printf("       error text: \"%s\"\n", netErrorText());
    ok(strcmp(netErrorText(), "server.txt has no address - add a host:port line") == 0,
       "second run gives the next instruction, not the same one");
    netExit();
}

/* A file that exists but holds only comments — what the template leaves behind,
 * and what a player who deleted their address by hand would have. */
static void case_comments_only(void)
{
    printf("\ncase 2: server.txt exists but has no address line\n");
    reset_sd();

    static const char BODY[] = "# nothing here yet\r\n\r\n#127.0.0.1:41234\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    ok(!netConnect(), "netConnect() refuses");
    printf("       error text: \"%s\"\n", netErrorText());
    ok(strcmp(netErrorText(), "server.txt has no address - add a host:port line") == 0,
       "error says the file is there but empty of addresses");

    size_t len = 0;
    const char *body = read_file(SD_FILE, &len);
    ok(body != NULL && strcmp(body, BODY) == 0,
       "the player's own file was NOT overwritten by the template");
    netExit();
}

/* A hostname with no port. Rejecting this is deliberate (see split_addr), so
 * the message has to say what is missing rather than blame the file. */
static void case_no_port(void)
{
    printf("\ncase 3: address with no port\n");
    reset_sd();

    static const char BODY[] = "my-world.at.ply.gg\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    ok(strcmp(netServerAddress(), "my-world.at.ply.gg") == 0,
       "the line was read: address is \"%s\"", netServerAddress());
    ok(!netConnect(), "netConnect() refuses");
    printf("       error text: \"%s\"\n", netErrorText());
    ok(strcmp(netErrorText(), "Address needs to be host:port - fix server.txt") == 0,
       "error names the missing part, not the file");
    netExit();
}

/* Notepad's default save. The three BOM bytes are invisible in every editor
 * that writes them, so without skip_bom() this looks like a working address
 * that mysteriously cannot resolve. */
static void case_utf8_bom(void)
{
    printf("\ncase 4: UTF-8 BOM in front of a good address\n");
    reset_sd();

    static const char BODY[] = "\xEF\xBB\xBF" "my-world.at.ply.gg:41234\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    printf("       address read: \"%s\"\n", netServerAddress());
    ok(strcmp(netServerAddress(), "my-world.at.ply.gg:41234") == 0,
       "the BOM is gone and the hostname starts at 'm'");
    ok((unsigned char)netServerAddress()[0] == 'm',
       "first byte is 'm' (0x6D), not 0xEF");
    netExit();
}

/* The ordinary case, kept as a control: it must stay green in every red-arm
 * run, otherwise a red result only proves the harness broke. */
static void case_plain_crlf(void)
{
    printf("\ncase 5 (control): ordinary CRLF address\n");
    reset_sd();

    static const char BODY[] = "# my server\r\n127.0.0.1:41234\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    printf("       address read: \"%s\"\n", netServerAddress());
    ok(strcmp(netServerAddress(), "127.0.0.1:41234") == 0,
       "comment skipped, CRLF trimmed, address intact");
    ok(strcmp(netErrorText(), "") == 0, "no error before connecting");
    netExit();
}

#endif /* !BAKED_ADDR_TEST */

/* --------------------------------------------- the shipping configuration -- */
#ifdef BAKED_ADDR_TEST

/* Compiled against bsnet.c's own BS_SERVER_ADDR default — i.e. exactly what a
 * released CIA contains. The point of these cases is that a console anywhere in
 * the world, with a blank SD card, is already pointed at the right server. */

/* Kept as a literal rather than pulled from the header: if somebody edits the
 * baked address, this test has to notice, not follow it. */
static const char BAKED[] = "reminded-sutton.tun.ply.gg:38783";

static void case_baked_no_sd_file(void)
{
    printf("\ncase B1: shipping build, blank SD card\n");
    reset_sd();

    ok(access(SD_FILE, F_OK) != 0, "precondition: no server.txt on the card");
    ok(netInit(), "netInit() succeeds");
    printf("       address: \"%s\"\n", netServerAddress());
    ok(strcmp(netServerAddress(), BAKED) == 0,
       "the playit tunnel address is already configured");
    ok(strchr(netServerAddress(), ':') != NULL,
       "it carries a port, so CONNECT will not be refused");

    /* The template exists to teach an unconfigured build's owner what to do.
     * A build that already knows its server must not leave one lying around. */
    ok(access(SD_FILE, F_OK) != 0,
       "no server.txt template written - nothing for the player to fill in");
    netExit();
}

static void case_baked_sd_override(void)
{
    printf("\ncase B2: SD card still overrides the baked address\n");
    reset_sd();

    static const char BODY[] = "10.0.0.7:41234\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    printf("       address: \"%s\"\n", netServerAddress());
    ok(strcmp(netServerAddress(), "10.0.0.7:41234") == 0,
       "server.txt wins, so a test server needs no rebuild");
    netExit();
}

/* Control: a shipping build with a broken server.txt must fall back to being
 * usable, not strand the player on a bad line they may not have written. */
static void case_baked_bad_line(void)
{
    printf("\ncase B3 (control): shipping build, unusable server.txt\n");
    reset_sd();

    static const char BODY[] = "# all commented out\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(netInit(), "netInit() succeeds");
    printf("       address: \"%s\"\n", netServerAddress());
    ok(strcmp(netServerAddress(), BAKED) == 0,
       "falls back to the baked address rather than to (not configured)");
    netExit();
}

/* The second half of "comes configured": an address is no use if the handshake
 * material is still missing. This is the error steve hit after the address was
 * baked in — "This build has no server key / network PSK configured".
 *
 * Deliberately NOT aimed at the real server: the address is overridden to a
 * local discard port, so this proves the keys were accepted without putting a
 * test handshake on the live gate. */
static void case_baked_keys_present(void)
{
    printf("\ncase B4: baked keys satisfy the handshake-material check\n");
    reset_sd();

    static const char BODY[] = "127.0.0.1:9\r\n";
    write_file(SD_FILE, BODY, sizeof BODY - 1);

    ok(access("sdmc:/blocksmith/server.pub", F_OK) != 0,
       "precondition: no server.pub on the card");
    ok(access("sdmc:/blocksmith/network.psk", F_OK) != 0,
       "precondition: no network.psk on the card");

    ok(netInit(), "netInit() succeeds");
    bool started = netConnect();
    printf("       netConnect() -> %s, error \"%s\"\n",
           started ? "true" : "false", netErrorText());
    ok(strstr(netErrorText(), "no server key") == NULL,
       "CONNECT is not blocked for missing server key / PSK");
    ok(started, "the connection attempt actually starts");

    netDisconnect();
    netExit();
}

int main(void)
{
    printf("blocksmith server-address host test (shipping configuration)\n");

    case_baked_no_sd_file();
    case_baked_sd_override();
    case_baked_bad_line();
    case_baked_keys_present();

    printf("\n%s %d checks, %d failed\n",
           failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}

#else

int main(void)
{
    printf("blocksmith server-address host test (unconfigured build)\n");

    case_missing_file();
    case_comments_only();
    case_no_port();
    case_utf8_bom();
    case_plain_crlf();

    printf("\n%s %d checks, %d failed\n",
           failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}

#endif
