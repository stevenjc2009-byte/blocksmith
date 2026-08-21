/* inv_bridge_test — host unit test for net/inv_bridge.c, the seam between world/inventory.h
 * and the wire.
 *
 * Same shape as net/networld_test.c beside it: "ok"/"FAIL" lines per check, a final
 * "PASS/FAIL N checks, M failed" summary, non-zero exit on any failure, and the same
 * link-seam fakes for net/bsnet_transport.h and net/bsnet_sock.h (neither is host-portable;
 * see networld_test.c's own header for the full reasoning, which applies here unchanged).
 * The REAL networld.c, inventory.c and crafting.c are linked, so every check below runs the
 * actual encode path and the actual inventory rules rather than a restatement of them.
 *
 * What this file is for, specifically: inv_bridge.c's whole job is deciding WHAT NUMBER goes
 * on the wire after a local inventory operation, and every one of those decisions is
 * invisible on a console. A bridge that sent the units the player *asked* to move rather
 * than the units that actually moved would look completely correct in play — the console
 * shows the right stacks, because the local call is unchanged — and would only surface as
 * the server's copy drifting further out of step with every partial move, on a rejoin, days
 * later. That is exactly the class of bug a host test costs a second to rule out and a
 * playtest cannot rule out at all.
 *
 * The __3DS__ guard is load-bearing, not tidy — mc/Makefile globs every .c under source/net
 * into the console build, so without it this file's main() collides with source/main.c's.
 */
#ifndef __3DS__

#include "net/inv_bridge.h"

#include <stdio.h>
#include <string.h>

#include "net/bsnet_sock.h"
#include "net/bsnet_transport.h"
#include "world/block.h"
#include "world/crafting.h"

#include "proto/bs_proto.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* ------------------------------------------------------- fake transport ------------------ */
/* networld.o references netTransportRecv() and netTransportSend() as extern symbols; these
 * definitions satisfy the linker instead of the real bsnet_transport.o. Unlike
 * networld_test.c's version this one keeps EVERY payload sent, not just the last: several
 * scenarios below are about a single call producing exactly one packet (or none), and "the
 * last packet looks right" cannot tell one packet from three. */

#define FAKE_SENT_MAX 16

static uint8_t fake_sent[FAKE_SENT_MAX][NET_MAX_PAYLOAD];
static size_t  fake_sent_len[FAKE_SENT_MAX];
static int     fake_sent_calls;

static void fakeTransportReset(void)
{
    fake_sent_calls = 0;
    memset(fake_sent,     0, sizeof fake_sent);
    memset(fake_sent_len, 0, sizeof fake_sent_len);
}

int netTransportRecv(uint8_t *out, size_t cap)
{
    (void)out; (void)cap;
    return 0;   /* nothing below drives the receive pump; INV_STATE goes in via
                 * networldApplyPayload(), the same entry point networld_test.c uses. */
}

bool netTransportSend(const uint8_t *payload, size_t len)
{
    if (fake_sent_calls < FAKE_SENT_MAX && len <= NET_MAX_PAYLOAD) {
        memcpy(fake_sent[fake_sent_calls], payload, len);
        fake_sent_len[fake_sent_calls] = len;
    }
    fake_sent_calls++;
    return true;
}

/* Link-time double for net/bsnet_sock.h's clock, for the same reason networld_test.c has one:
 * bsnet_sock.c is not linked here either. Nothing below depends on time advancing. */
uint64_t bsSockNowMs(void) { return 0; }

/* ------------------------------------------------------- helpers -------------------------- */

/* Arms networld.c's capability probe by delivering one well-formed BS_APP_INV_STATE, so the
 * scenarios that are about what gets SENT can send at all. `inv` receives whatever that
 * snapshot said, via the same invBridgeApplyState() the real main.c hook calls.
 *
 * Deliberately built out of an all-empty inventory: a scenario that wants specific contents
 * sets them on `inv` afterwards, so the starting state of each test is written in that test
 * rather than hidden in here. */
static void armSession(Inventory *inv)
{
    networldInit();

    uint8_t msg[BS_INV_STATE_BYTES];
    memset(msg, 0, sizeof msg);
    msg[0] = BS_APP_INV_STATE;
    msg[1] = 0;
    networldApplyPayload(msg, sizeof msg);

    inventoryInit(inv);
    fakeTransportReset();
}

/* The op byte and three parameters of packet `n`, or false if there was no such packet or it
 * is not the size bs_proto.h fixes INV_ACTION at. Every "what went on the wire" check below
 * goes through this rather than indexing fake_sent directly, so a packet of the wrong length
 * fails the check instead of being read past. */
static bool sentAction(int n, uint8_t *op, uint8_t *a, uint8_t *b, uint8_t *c)
{
    if (n < 0 || n >= fake_sent_calls || n >= FAKE_SENT_MAX) return false;
    if (fake_sent_len[n] != BS_INV_ACTION_BYTES)             return false;
    if (fake_sent[n][0] != BS_APP_INV_ACTION)                return false;

    *op = fake_sent[n][1];
    *a  = fake_sent[n][2];
    *b  = fake_sent[n][3];
    *c  = fake_sent[n][4];
    return true;
}

/* ------------------------------------------------------------------ scenarios ------------- */

/* The single-player case, and the old-server case, which are the same case. This is the one
 * that has to hold before any other check matters: every one of these wrappers is on a path
 * the game takes with no session at all. */
static void test_offline_sends_nothing_but_still_mutates(void)
{
    puts("with no session, every wrapper performs its local op and sends nothing");

    networldInit();          /* no INV_STATE delivered: the probe stays disarmed */
    fakeTransportReset();

    Inventory inv;
    inventoryInit(&inv);
    inv.slots[0].item  = BLOCK_STONE;
    inv.slots[0].count = 10;

    check(invBridgeMoveUnits(&inv, 0, 5, 4) == 4, "move still moved locally");
    invBridgeSwapSlots(&inv, 0, 5);
    invBridgeSelectHotbar(&inv, 3);
    check(invBridgeAdd(&inv, BLOCK_DIRT, 7, NULL) == INV_ADD_OK, "add still added locally");
    check(invBridgeRemove(&inv, BLOCK_DIRT, 7) == 7, "remove still removed locally");

    check(inv.selected_hotbar == 3, "the local hotbar selection took effect");
    check(fake_sent_calls == 0, "nothing was handed to the transport");
}

/* The bug this whole file exists to rule out: a partial move must report what MOVED, not what
 * was requested. Asking to move 40 into a stack with room for 4 moves 4. */
static void test_move_reports_units_actually_moved(void)
{
    puts("a partial move reports the units that moved, not the units requested");

    Inventory inv;
    armSession(&inv);

    inv.slots[0].item  = BLOCK_STONE;
    inv.slots[0].count = 40;
    inv.slots[3].item  = BLOCK_STONE;
    inv.slots[3].count = (uint8_t)(INV_STACK_MAX - 4);

    const uint8_t moved = invBridgeMoveUnits(&inv, 0, 3, 40);
    check(moved == 4, "only the 4 units that fit were moved locally");

    uint8_t op, a, b, c;
    check(fake_sent_calls == 1, "exactly one packet went out");
    check(sentAction(0, &op, &a, &b, &c), "it is a well-formed INV_ACTION");
    check(op == BS_INV_OP_MOVE, "op is MOVE");
    check(a == 0 && b == 3, "src and dst slots are on the wire");
    check(c == 4, "the count on the wire is 4 (moved), not 40 (requested)");
}

/* A refused move is not a state change, so there is nothing for the server to apply. */
static void test_refused_move_sends_nothing(void)
{
    puts("a move refused by a different item in the destination sends nothing");

    Inventory inv;
    armSession(&inv);

    inv.slots[0].item  = BLOCK_STONE;
    inv.slots[0].count = 10;
    inv.slots[3].item  = BLOCK_DIRT;    /* different item: inventoryMoveUnits refuses outright */
    inv.slots[3].count = 10;

    check(invBridgeMoveUnits(&inv, 0, 3, 10) == 0, "the move was refused locally");
    check(fake_sent_calls == 0, "no packet went out for a move that did not happen");
}

static void test_swap_puts_both_slots_on_the_wire(void)
{
    puts("a swap sends both slot indices");

    Inventory inv;
    armSession(&inv);

    inv.slots[1].item  = BLOCK_STONE;
    inv.slots[1].count = 5;
    inv.slots[20].item  = BLOCK_DIRT;
    inv.slots[20].count = 9;

    invBridgeSwapSlots(&inv, 1, 20);

    check(inv.slots[1].item == BLOCK_DIRT && inv.slots[20].item == BLOCK_STONE,
          "the local swap happened");

    uint8_t op, a, b, c;
    check(fake_sent_calls == 1, "exactly one packet went out");
    check(sentAction(0, &op, &a, &b, &c), "it is a well-formed INV_ACTION");
    check(op == BS_INV_OP_SWAP && a == 1 && b == 20, "op is SWAP with both slots");
    check(c == 0, "the unused third parameter is 0, as bs_proto.h requires");
}

/* inventorySelectHotbar clamps out-of-range to the last hotbar slot. If the raw value went on
 * the wire instead of the clamped one, the two copies would clamp independently and could
 * disagree the day either side's INV_HOTBAR_SLOTS changes. */
static void test_select_sends_the_clamped_value(void)
{
    puts("select sends the clamped hotbar index, not the raw one");

    Inventory inv;
    armSession(&inv);

    invBridgeSelectHotbar(&inv, 200);

    check(inv.selected_hotbar == INV_HOTBAR_SLOTS - 1, "the local selection clamped");

    uint8_t op, a, b, c;
    check(sentAction(0, &op, &a, &b, &c), "a well-formed INV_ACTION went out");
    check(op == BS_INV_OP_SELECT, "op is SELECT");
    check(a == INV_HOTBAR_SLOTS - 1, "the wire carries the clamped index, not 200");
}

/* The one deliberate no-op-still-sends in this file. See inv_bridge.h: SELECT is idempotent,
 * so re-sending it repairs a server whose earlier SELECT was dropped, whereas skipping it
 * leaves a stale selection that the server's next snapshot pushes back onto this console. */
static void test_select_resends_even_when_unchanged(void)
{
    puts("select sends every time, including when the selection did not change");

    Inventory inv;
    armSession(&inv);

    invBridgeSelectHotbar(&inv, 4);
    invBridgeSelectHotbar(&inv, 4);

    check(fake_sent_calls == 2, "both selects went out, not just the one that changed state");

    uint8_t op, a, b, c;
    check(sentAction(1, &op, &a, &b, &c) && op == BS_INV_OP_SELECT && a == 4,
          "the second packet is a SELECT carrying the same index");
}

static void test_craft_sends_only_on_success(void)
{
    puts("craft sends only when the recipe actually ran");

    Inventory inv;
    armSession(&inv);

    /* Nothing in the inventory: every recipe's ingredients are missing. */
    check(!invBridgeCraft(&inv, RECIPE_WOOD_TO_PLANKS), "the craft was refused locally");
    check(fake_sent_calls == 0, "no packet went out for a craft that did not happen");

    inv.slots[0].item  = BLOCK_WOOD;
    inv.slots[0].count = 1;

    check(invBridgeCraft(&inv, RECIPE_WOOD_TO_PLANKS), "the craft succeeded locally");
    check(inventoryCount(&inv, BLOCK_PLANKS) == 4, "one log became four planks");

    uint8_t op, a, b, c;
    check(fake_sent_calls == 1, "exactly one packet went out");
    check(sentAction(0, &op, &a, &b, &c), "it is a well-formed INV_ACTION");
    check(op == BS_INV_OP_CRAFT && a == RECIPE_WOOD_TO_PLANKS, "op is CRAFT with the recipe index");
}

/* The pickup half of the same "report what landed" rule the move check covers. */
static void test_pickup_reports_what_landed(void)
{
    puts("a partial pickup reports the units the inventory took");

    Inventory inv;
    armSession(&inv);

    /* Every slot full of stone except one, which is 2 short of the cap: an add of 9 has room
     * for exactly 2. */
    for (int i = 0; i < INV_SLOT_COUNT; i++) {
        inv.slots[i].item  = BLOCK_STONE;
        inv.slots[i].count = INV_STACK_MAX;
    }
    inv.slots[7].count = (uint8_t)(INV_STACK_MAX - 2);

    uint8_t leftover = 0;
    const InvAddResult r = invBridgeAdd(&inv, BLOCK_STONE, 9, &leftover);
    check(r == INV_ADD_PARTIAL, "the add was partial");
    check(leftover == 7, "7 units did not fit");

    uint8_t op, a, b, c;
    check(fake_sent_calls == 1, "exactly one packet went out");
    check(sentAction(0, &op, &a, &b, &c), "it is a well-formed INV_ACTION");
    check(op == BS_INV_OP_PICKUP && a == BLOCK_STONE, "op is PICKUP with the item id");
    check(b == 2, "the count on the wire is 2 (landed), not 9 (offered)");
}

static void test_refused_pickup_sends_nothing(void)
{
    puts("a pickup refused by a full inventory sends nothing");

    Inventory inv;
    armSession(&inv);

    for (int i = 0; i < INV_SLOT_COUNT; i++) {
        inv.slots[i].item  = BLOCK_STONE;
        inv.slots[i].count = INV_STACK_MAX;
    }

    check(invBridgeAdd(&inv, BLOCK_DIRT, 1, NULL) == INV_ADD_REFUSED, "the add was refused");
    check(fake_sent_calls == 0, "no packet went out for a block left on the floor");
}

static void test_consume_reports_what_was_removed(void)
{
    puts("a consume reports the units actually removed");

    Inventory inv;
    armSession(&inv);

    inv.slots[2].item  = BLOCK_SAND;
    inv.slots[2].count = 2;

    check(invBridgeRemove(&inv, BLOCK_SAND, 5) == 2, "only the 2 held units were removed");

    uint8_t op, a, b, c;
    check(fake_sent_calls == 1, "exactly one packet went out");
    check(sentAction(0, &op, &a, &b, &c), "it is a well-formed INV_ACTION");
    check(op == BS_INV_OP_CONSUME && a == BLOCK_SAND, "op is CONSUME with the item id");
    check(b == 2, "the count on the wire is 2 (removed), not 5 (requested)");
}

static void test_consume_of_nothing_sends_nothing(void)
{
    puts("consuming an item the inventory does not hold sends nothing");

    Inventory inv;
    armSession(&inv);

    check(invBridgeRemove(&inv, BLOCK_SAND, 1) == 0, "nothing was removed");
    check(fake_sent_calls == 0, "no packet went out");
}

/* The server is authoritative: a snapshot replaces the inventory outright, including
 * emptying a slot this console believed had something in it. A merge would preserve exactly
 * the divergence the snapshot exists to erase. */
static void test_apply_state_overwrites_wholesale(void)
{
    puts("a snapshot replaces the whole inventory, including clearing local slots");

    Inventory inv;
    armSession(&inv);

    inv.slots[0].item  = BLOCK_STONE;
    inv.slots[0].count = 50;
    inv.slots[9].item  = BLOCK_DIRT;
    inv.slots[9].count = 3;
    inv.selected_hotbar = 6;

    NetworldInvState st;
    memset(&st, 0, sizeof st);
    st.selected_hotbar   = 2;
    st.slots[9].item     = BLOCK_WOOD;
    st.slots[9].count    = 11;

    check(invBridgeApplyState(&inv, &st), "the snapshot was accepted");
    check(inv.slots[0].item == ITEM_NONE && inv.slots[0].count == 0,
          "a slot the server says is empty was cleared locally");
    check(inv.slots[9].item == BLOCK_WOOD && inv.slots[9].count == 11,
          "a slot the server says holds wood now holds wood");
    check(inv.selected_hotbar == 2, "the selection came from the server");
}

/* All-or-nothing, and the bad slot is deliberately the LAST one: an implementation that
 * validated and wrote in a single pass would already have overwritten slots 0..22 before it
 * reached the id it cannot represent, and slot 0 is what catches that. */
static void test_apply_state_rejects_unknown_item_without_touching_anything(void)
{
    puts("a snapshot naming an item this build does not have is rejected whole");

    Inventory inv;
    armSession(&inv);

    inv.slots[0].item   = BLOCK_STONE;
    inv.slots[0].count  = 50;
    inv.selected_hotbar = 6;

    NetworldInvState st;
    memset(&st, 0, sizeof st);
    st.selected_hotbar = 2;
    st.slots[0].item   = BLOCK_DIRT;
    st.slots[0].count  = 1;
    st.slots[NETWORLD_INV_SLOT_COUNT - 1].item  = BLOCK_COUNT;   /* one past the last real id */
    st.slots[NETWORLD_INV_SLOT_COUNT - 1].count = 1;

    check(!invBridgeApplyState(&inv, &st), "the snapshot was rejected");
    check(inv.slots[0].item == BLOCK_STONE && inv.slots[0].count == 50,
          "slot 0 was not overwritten on the way to the bad slot");
    check(inv.selected_hotbar == 6, "the selection was not touched either");
}

/* Unlike an unknown item id, an out-of-range selection cannot index past the array — it can
 * only ever name a hotbar slot — so it is clamped rather than being grounds to drop a whole
 * otherwise-valid snapshot. */
static void test_apply_state_clamps_the_selection(void)
{
    puts("a snapshot with an out-of-range selection is clamped, not rejected");

    Inventory inv;
    armSession(&inv);

    NetworldInvState st;
    memset(&st, 0, sizeof st);
    st.selected_hotbar = 200;

    check(invBridgeApplyState(&inv, &st), "the snapshot was still accepted");
    check(inv.selected_hotbar == INV_HOTBAR_SLOTS - 1, "the selection clamped to the last hotbar slot");
}

/* Applying a snapshot must not itself put anything on the wire. If it did, every snapshot
 * would provoke a reply, and a reply that the server applied would provoke another snapshot:
 * a feedback loop that nothing in the protocol damps, on a 10 Hz tick. */
static void test_apply_state_sends_nothing(void)
{
    puts("applying a snapshot sends nothing back");

    Inventory inv;
    armSession(&inv);

    NetworldInvState st;
    memset(&st, 0, sizeof st);
    st.selected_hotbar = 3;
    st.slots[1].item   = BLOCK_GRASS;
    st.slots[1].count  = 8;

    check(invBridgeApplyState(&inv, &st), "the snapshot was accepted");
    check(fake_sent_calls == 0, "nothing was handed to the transport");
}

int main(void)
{
    test_offline_sends_nothing_but_still_mutates();
    test_move_reports_units_actually_moved();
    test_refused_move_sends_nothing();
    test_swap_puts_both_slots_on_the_wire();
    test_select_sends_the_clamped_value();
    test_select_resends_even_when_unchanged();
    test_craft_sends_only_on_success();
    test_pickup_reports_what_landed();
    test_refused_pickup_sends_nothing();
    test_consume_reports_what_was_removed();
    test_consume_of_nothing_sends_nothing();
    test_apply_state_overwrites_wholesale();
    test_apply_state_rejects_unknown_item_without_touching_anything();
    test_apply_state_clamps_the_selection();
    test_apply_state_sends_nothing();

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
