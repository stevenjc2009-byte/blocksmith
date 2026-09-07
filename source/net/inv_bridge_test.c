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
#include <stdlib.h>   /* abort(), in the netTransportRefuse() double below */
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

/* v1.8.7. networld.c's new networldSessionActive() reads this, so the link needs it too — the
 * same link-time double, for the same reason, as the send above. ESTABLISHED: every case here
 * is about a client with a live session mirroring its bag to a server. */
NetTransportState netTransportState(void)
{
    return NET_TRANSPORT_ESTABLISHED;
}

/* v1.8.7. networld.c's registry verdict can call this, so the link needs it. Nothing in this
 * file arms that verdict — no scenario here sends a BS_APP_WORLD_INFO or a REGISTRY_INFO, so
 * networld.c's gate is never armed and registryVerdictTick() returns before it could reach
 * here — and the abort() says so out loud rather than letting a silently refused session make
 * some later inventory check fail for a reason that has nothing to do with inventories. */
void netTransportRefuse(const char *why)
{
    fprintf(stderr, "inv_bridge_test: unexpected session refusal: %s\n", why ? why : "(null)");
    abort();
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

/* main.c's own inventory hook, reproduced: all it does is hand the snapshot to the bridge. */
static void hookApply(void *ud, const NetworldInvState *state)
{
    (void)invBridgeApplyState((Inventory *)ud, state);
}

/* The bug that made the whole of v1.3.0's feature a no-op on a real console, as a test.
 *
 * The real sequence is not "register a hook, then receive a snapshot". JOIN completes inside
 * main.c's runTitleScreen(), which pumps networldUpdate() itself, so the server's unprompted
 * join-time INV_STATE arrives while the player is still on the title screen — before a world has
 * been entered and therefore before there is any Inventory for a hook to point at. The hook can
 * only be registered afterwards, and in v1.3.0 the snapshot was long gone by then.
 *
 * This walks that exact order, including the inventoryInit() a server session performs at world
 * entry, which is the second thing that used to destroy the server's answer. */
static void test_snapshot_that_arrived_before_the_hook_is_still_delivered(void)
{
    puts("a snapshot that arrived before the hook existed is replayed on registration");

    networldInit();

    /* --- title screen: JOIN, and the server volunteers a snapshot. No hook exists yet. --- */
    uint8_t msg[BS_INV_STATE_BYTES];
    memset(msg, 0, sizeof msg);
    msg[0] = BS_APP_INV_STATE;
    msg[1] = 2;                                   /* selected hotbar slot */
    msg[BS_APP_HDR_BYTES + 1 + 0] = BLOCK_WOOD;   /* slot 0: 7 wood, as if mined last session */
    msg[BS_APP_HDR_BYTES + 1 + 1] = 7;
    networldApplyPayload(msg, sizeof msg);

    /* --- world entry: a server session initialises an EMPTY inventory, then registers. --- */
    Inventory inv;
    inventoryInit(&inv);
    check(inv.slots[0].count == 0, "the inventory really is empty before the hook is registered");

    networldSetInvHook(hookApply, &inv);

    check(inv.slots[0].item == BLOCK_WOOD, "slot 0 holds the wood the server was keeping");
    check(inv.slots[0].count == 7, "and all 7 units of it");
    check(inv.selected_hotbar == 2, "the server's selected hotbar slot came across too");

    networldSetInvHook(NULL, NULL);
}

/* The other half of the ordering: registration must not fire when nothing has arrived, or a
 * single-player session would have its loaded inventory wiped by a replay of nothing. */
static void test_registration_without_a_snapshot_leaves_the_inventory_alone(void)
{
    puts("registering a hook with no snapshot received leaves the inventory untouched");

    networldInit();

    Inventory inv;
    inventoryInit(&inv);
    inv.slots[0].item  = BLOCK_STONE;   /* stands in for a single-player inventoryLoad() */
    inv.slots[0].count = 12;

    networldSetInvHook(hookApply, &inv);

    check(inv.slots[0].item == BLOCK_STONE && inv.slots[0].count == 12,
          "the locally loaded inventory survived registration");

    networldSetInvHook(NULL, NULL);
}

/* ------------------------------------------------- v1.9.0 SPLIT: lift + quick-move --------- */

/* Sets slot `n` by hand; every SPLIT scenario starts from an armed session and writes its own
 * starting contents, same as the scenarios above. */
static void setSlot(Inventory *inv, int n, ItemId item, uint8_t count)
{
    inv->slots[n].item  = item;
    inv->slots[n].count = count;
}

static bool slotIs(const Inventory *inv, int n, ItemId item, uint8_t count)
{
    return inv->slots[n].item == item && inv->slots[n].count == count;
}

/* Packet `n` is MOVE(a, b, c). */
static bool sentMove(int n, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t op, pa, pb, pc;
    if (!sentAction(n, &op, &pa, &pb, &pc)) return false;
    return op == BS_INV_OP_MOVE && pa == a && pb == b && pc == c;
}

static void test_split_refuses_when_there_is_no_half(void)
{
    puts("a split refuses a stack of 1, an empty slot, a bad index and a NULL, writing nothing");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 1);

    InvSlot lift = { BLOCK_DIRT, 9 };   /* a sentinel: a refusal must not touch it */
    check(!invBridgeSplitStack(&inv, 0, &lift),  "a stack of 1 refuses");
    check(lift.item == BLOCK_DIRT && lift.count == 9, "the lift was not written by the refusal");
    check(slotIs(&inv, 0, BLOCK_STONE, 1),         "the slot of 1 is unchanged");
    check(!invBridgeSplitStack(&inv, 5, &lift),  "an empty slot refuses");
    check(!invBridgeSplitStack(&inv, -1, &lift), "a negative slot refuses");
    check(!invBridgeSplitStack(&inv, 0, NULL),   "a NULL lift refuses");
    check(!invBridgeSplitStack(&inv, INV_SLOT_COUNT, &lift), "an out-of-range slot refuses");
    check(fake_sent_calls == 0,                   "no refusal sent anything");
}

static void test_split_lifts_the_ceiling_half(void)
{
    puts("a split lifts ceil(n/2) and leaves floor(n/2): 2, even, odd, and the cap");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 2);
    setSlot(&inv, 1, BLOCK_STONE, 10);
    setSlot(&inv, 2, BLOCK_STONE, 7);
    setSlot(&inv, 3, BLOCK_DIRT,  INV_STACK_MAX);

    InvSlot lift = { ITEM_NONE, 0 };
    check(invBridgeSplitStack(&inv, 0, &lift) && lift.item == BLOCK_STONE && lift.count == 1,
          "2 -> lift 1");
    check(slotIs(&inv, 0, BLOCK_STONE, 1), "2 -> slot keeps 1");

    check(invBridgeSplitStack(&inv, 1, &lift) && lift.item == BLOCK_STONE && lift.count == 5,
          "10 -> lift 5");
    check(slotIs(&inv, 1, BLOCK_STONE, 5), "10 -> slot keeps 5");

    check(invBridgeSplitStack(&inv, 2, &lift) && lift.item == BLOCK_STONE && lift.count == 4,
          "7 -> lift 4 (the lift gets the odd unit)");
    check(slotIs(&inv, 2, BLOCK_STONE, 3), "7 -> slot keeps 3");

    check(invBridgeSplitStack(&inv, 3, &lift) && lift.item == BLOCK_DIRT && lift.count == 50,
          "99 -> lift 50");
    check(slotIs(&inv, 3, BLOCK_DIRT, 49), "99 -> slot keeps 49");

    check(fake_sent_calls == 0, "a split sends nothing: the server's copy has not changed");
}

static void test_place_lift_exact_fit_reports_a_move_from_the_origin(void)
{
    puts("placing a lift into an empty slot takes it whole and sends MOVE(origin, dst, placed)");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 10);

    InvSlot lift;
    check(invBridgeSplitStack(&inv, 0, &lift), "split 10 (lift 5, slot 5)");

    const uint8_t placed = invBridgePlaceLift(&inv, 0, 9, &lift);
    check(placed == 5,                          "5 units placed");
    check(slotIs(&inv, 9, BLOCK_STONE, 5),      "slot 9 holds the 5");
    check(lift.item == ITEM_NONE && lift.count == 0, "the lift is empty and normalised");
    check(slotIs(&inv, 0, BLOCK_STONE, 5),      "the origin still holds its 5");
    check(fake_sent_calls == 1,                 "exactly one packet went out");
    check(sentMove(0, 0, 9, 5),                 "it is MOVE(origin 0, dst 9, 5)");
}

static void test_place_lift_overflow_keeps_the_remainder_lifted(void)
{
    puts("placing a lift onto a nearly-full same-item stack tops it up and keeps the rest lifted");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 20);
    setSlot(&inv, 9, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 4));

    InvSlot lift;
    check(invBridgeSplitStack(&inv, 0, &lift) && lift.count == 10, "split 20 (lift 10)");

    check(invBridgePlaceLift(&inv, 0, 9, &lift) == 4, "only the 4 that fit were placed");
    check(slotIs(&inv, 9, BLOCK_STONE, INV_STACK_MAX), "slot 9 is at the cap");
    check(lift.item == BLOCK_STONE && lift.count == 6, "6 units are still lifted");
    check(fake_sent_calls == 1 && sentMove(0, 0, 9, 4), "one packet: MOVE(0, 9, 4), the placed count");

    check(invBridgePlaceLift(&inv, 0, 9, &lift) == 0 && lift.count == 6 && fake_sent_calls == 1,
          "onto the now-full stack again: nothing placed, lift intact, no packet");

    check(invBridgePlaceLift(&inv, 0, 10, &lift) == 6, "the remainder places whole into an empty slot");
    check(lift.item == ITEM_NONE && slotIs(&inv, 10, BLOCK_STONE, 6), "lift empty, slot 10 holds 6");
    check(fake_sent_calls == 2 && sentMove(1, 0, 10, 6), "second packet: MOVE(0, 10, 6)");
    check(inv.slots[0].count + inv.slots[9].count + inv.slots[10].count == 20 + INV_STACK_MAX - 4,
          "no unit was created or lost across the split and both places");
}

static void test_place_lift_onto_a_different_item_refuses(void)
{
    puts("placing a lift onto a different item refuses, keeps the lift, sends nothing");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 6);
    setSlot(&inv, 9, BLOCK_DIRT,  2);

    InvSlot lift;
    check(invBridgeSplitStack(&inv, 0, &lift) && lift.count == 3, "split 6 (lift 3)");
    check(invBridgePlaceLift(&inv, 0, 9, &lift) == 0, "nothing placed");
    check(lift.item == BLOCK_STONE && lift.count == 3, "the lift is intact");
    check(slotIs(&inv, 9, BLOCK_DIRT, 2),              "the dirt is untouched");
    check(fake_sent_calls == 0,                        "no packet");
}

static void test_place_lift_back_onto_its_origin_sends_nothing(void)
{
    puts("placing a lift back onto the slot it was split from restores it and sends nothing");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 7);

    InvSlot lift;
    check(invBridgeSplitStack(&inv, 0, &lift) && lift.count == 4, "split 7 (lift 4)");
    check(invBridgePlaceLift(&inv, 0, 0, &lift) == 4, "all 4 go back");
    check(slotIs(&inv, 0, BLOCK_STONE, 7) && lift.item == ITEM_NONE, "the slot is 7 again, lift empty");
    check(fake_sent_calls == 0, "the server's copy never changed, so nothing is sent");
}

static void test_return_lift_goes_to_the_origin_or_anywhere_free(void)
{
    puts("returning a lift merges into the origin, or anywhere free if the origin changed under it");

    Inventory inv;
    armSession(&inv);

    /* The ordinary cancel. */
    setSlot(&inv, 0, BLOCK_STONE, 7);
    InvSlot lift;
    check(invBridgeSplitStack(&inv, 0, &lift), "split 7");
    check(invBridgeReturnLift(&inv, 0, &lift), "the return reports the lift empty");
    check(slotIs(&inv, 0, BLOCK_STONE, 7) && lift.item == ITEM_NONE, "the slot is 7 again");
    check(fake_sent_calls == 0, "a return sends nothing");

    /* A snapshot rewrote the origin while the half was lifted. */
    setSlot(&inv, 0, BLOCK_STONE, 8);
    check(invBridgeSplitStack(&inv, 0, &lift) && lift.count == 4, "split 8 (lift 4)");
    setSlot(&inv, 0, BLOCK_DIRT, 1);
    check(invBridgeReturnLift(&inv, 0, &lift), "the lift still went somewhere");
    check(slotIs(&inv, 0, BLOCK_DIRT, 1),   "the dirt in the origin was not disturbed");
    check(inventoryCount(&inv, BLOCK_STONE) == 4, "the 4 stone are back in the bag elsewhere");
    check(fake_sent_calls == 0, "and still nothing sent: the server never saw the detach");

    /* Nowhere at all: the only case that can leave units in the lift. */
    for (int i = 0; i < INV_SLOT_COUNT; i++) setSlot(&inv, i, BLOCK_DIRT, INV_STACK_MAX);
    lift.item = BLOCK_STONE; lift.count = 3;
    check(!invBridgeReturnLift(&inv, 0, &lift), "a full bag of another item cannot take it back");
    check(lift.item == BLOCK_STONE && lift.count == 3, "and the units stay in the lift, not lost");
}

static void test_quick_move_hotbar_to_main_and_back(void)
{
    puts("quick-move sends a hotbar stack to the first free main slot, and a main stack back");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 10);

    check(invBridgeQuickMove(&inv, 0) == 10,     "hotbar -> main: 10 moved");
    check(slotIs(&inv, 0, ITEM_NONE, 0),          "the hotbar slot is empty");
    check(slotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, 10), "the first main slot holds the 10");
    check(fake_sent_calls == 1 && sentMove(0, 0, INV_HOTBAR_SLOTS, 10), "one packet: MOVE(0, 8, 10)");

    fakeTransportReset();
    check(invBridgeQuickMove(&inv, INV_HOTBAR_SLOTS) == 10, "main -> hotbar: 10 moved");
    check(slotIs(&inv, 0, BLOCK_STONE, 10),       "the first hotbar slot holds the 10 again");
    check(fake_sent_calls == 1 && sentMove(0, INV_HOTBAR_SLOTS, 0, 10), "one packet: MOVE(8, 0, 10)");
}

static void test_quick_move_tops_up_same_item_first_then_spills(void)
{
    puts("quick-move tops up every same-item stack in the other strip first, then the first empty");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0,  BLOCK_STONE, 30);
    setSlot(&inv, 9,  BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 4));   /* room for 4 */
    setSlot(&inv, 10, BLOCK_DIRT,  1);                              /* must be skipped */
    setSlot(&inv, 11, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 9));   /* room for 9 */
    /* slot 8 is the first EMPTY main slot; it must come AFTER the two top-ups */

    check(invBridgeQuickMove(&inv, 0) == 30,      "all 30 moved");
    check(slotIs(&inv, 0, ITEM_NONE, 0),           "the source emptied");
    check(slotIs(&inv, 9,  BLOCK_STONE, INV_STACK_MAX), "slot 9 topped up to the cap");
    check(slotIs(&inv, 11, BLOCK_STONE, INV_STACK_MAX), "slot 11 topped up to the cap");
    check(slotIs(&inv, 8,  BLOCK_STONE, 17),       "the remaining 17 landed in the first empty slot");
    check(slotIs(&inv, 10, BLOCK_DIRT, 1),         "the dirt was skipped");
    check(fake_sent_calls == 3,                    "three pieces, three packets");
    check(sentMove(0, 0, 9, 4),                    "packet 0: MOVE(0, 9, 4)");
    check(sentMove(1, 0, 11, 9),                   "packet 1: MOVE(0, 11, 9)");
    check(sentMove(2, 0, 8, 17),                   "packet 2: MOVE(0, 8, 17)");
}

static void test_quick_move_to_a_full_strip_refuses(void)
{
    puts("quick-move into a strip with no room refuses, changes nothing, sends nothing");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 5);
    for (int i = INV_HOTBAR_SLOTS; i < INV_SLOT_COUNT; i++) setSlot(&inv, i, BLOCK_DIRT, INV_STACK_MAX);

    check(invBridgeQuickMove(&inv, 0) == 0, "a strip full of another item: 0 moved");
    check(slotIs(&inv, 0, BLOCK_STONE, 5),  "the source is untouched");
    bool all_dirt = true;
    for (int i = INV_HOTBAR_SLOTS; i < INV_SLOT_COUNT; i++)
        all_dirt = all_dirt && slotIs(&inv, i, BLOCK_DIRT, INV_STACK_MAX);
    check(all_dirt,             "every main slot is untouched");
    check(fake_sent_calls == 0, "no packet");

    for (int i = INV_HOTBAR_SLOTS; i < INV_SLOT_COUNT; i++) setSlot(&inv, i, BLOCK_STONE, INV_STACK_MAX);
    check(invBridgeQuickMove(&inv, 0) == 0, "a strip full of the SAME item at the cap: 0 moved");
    check(fake_sent_calls == 0,             "still no packet");
}

static void test_quick_move_moves_only_what_fits(void)
{
    puts("quick-move into a strip with room for 7 moves 7 and leaves the rest where it was");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 20);
    for (int i = INV_HOTBAR_SLOTS; i < INV_SLOT_COUNT; i++) setSlot(&inv, i, BLOCK_STONE, INV_STACK_MAX);
    setSlot(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 7));

    check(invBridgeQuickMove(&inv, 0) == 7,   "7 moved");
    check(slotIs(&inv, 0, BLOCK_STONE, 13),   "13 stayed in the source");
    check(slotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, INV_STACK_MAX), "the one slot with room is full");
    check(fake_sent_calls == 1 && sentMove(0, 0, INV_HOTBAR_SLOTS, 7), "one packet: MOVE(0, 8, 7)");
}

static void test_plan_quick_move_describes_without_touching(void)
{
    puts("the plan describes the moves a quick-move would make, touches nothing, and applies cleanly");

    Inventory inv;
    armSession(&inv);
    setSlot(&inv, 0,  BLOCK_STONE, 30);
    setSlot(&inv, 9,  BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 4));
    setSlot(&inv, 10, BLOCK_DIRT,  1);
    setSlot(&inv, 11, BLOCK_STONE, (uint8_t)(INV_STACK_MAX - 9));
    setSlot(&inv, 12, BLOCK_STONE, INV_STACK_MAX);   /* same item, no room: must describe NO move */

    Inventory before = inv;
    InvBridgeMove plan[INV_BRIDGE_PLAN_MAX];
    const int n = invBridgePlanQuickMove(&inv, 0, plan, INV_BRIDGE_PLAN_MAX);
    check(n == 3,                                     "three moves described");
    check(memcmp(&before, &inv, sizeof inv) == 0,     "the inventory is byte-identical afterwards");
    check(plan[0].op == BS_INV_OP_MOVE && plan[0].a == 0 && plan[0].b == 9  && plan[0].c == 4,
          "move 0 is MOVE(0, 9, 4)");
    check(plan[1].op == BS_INV_OP_MOVE && plan[1].a == 0 && plan[1].b == 11 && plan[1].c == 9,
          "move 1 is MOVE(0, 11, 9)");
    check(plan[2].op == BS_INV_OP_MOVE && plan[2].a == 0 && plan[2].b == 8  && plan[2].c == 17,
          "move 2 is MOVE(0, 8, 17)");
    check(fake_sent_calls == 0,                       "planning sends nothing");

    InvBridgeMove one;
    check(invBridgePlanQuickMove(&inv, 0, &one, 1) == 1 && one.b == 9 && one.c == 4,
          "a cap of 1 describes the first move only");
    check(invBridgePlanQuickMove(&inv, 5, plan, INV_BRIDGE_PLAN_MAX) == 0, "an empty slot plans nothing");

    /* The single-player path over the same plan: each applied move reports its described count. */
    uint8_t total = 0;
    for (int i = 0; i < n; i++) total = (uint8_t)(total + invBridgeApplyMove(&inv, &plan[i]));
    check(total == 30 && slotIs(&inv, 8, BLOCK_STONE, 17) && slotIs(&inv, 0, ITEM_NONE, 0),
          "applying the plan lands exactly what it described");
    check(fake_sent_calls == 3 && sentMove(2, 0, 8, 17), "and each applied move went out as its own packet");

    /* From a slot that HAS units into an empty one, so a bridge that ignored the op byte and
     * moved anyway would move 1 and send — a refusal from an empty source proves nothing. */
    InvBridgeMove bad = { BS_INV_OP_SWAP, 8, 0, 1 };
    check(invBridgeApplyMove(&inv, &bad) == 0 && fake_sent_calls == 3 && slotIs(&inv, 8, BLOCK_STONE, 17),
          "an op the bridge does not describe is refused, moves nothing and is not sent");
}

static void test_lift_and_quick_move_offline_still_mutate(void)
{
    puts("with no session, quick-move and place-lift perform their local op and send nothing");

    networldInit();          /* no INV_STATE delivered: the probe stays disarmed */
    fakeTransportReset();

    Inventory inv;
    inventoryInit(&inv);
    setSlot(&inv, 0, BLOCK_STONE, 10);
    setSlot(&inv, 1, BLOCK_STONE, 8);

    check(invBridgeQuickMove(&inv, 0) == 10 && slotIs(&inv, INV_HOTBAR_SLOTS, BLOCK_STONE, 10),
          "quick-move still moved locally");
    InvSlot lift;
    check(invBridgeSplitStack(&inv, 1, &lift) && invBridgePlaceLift(&inv, 1, 2, &lift) == 4,
          "split and place still worked locally");
    check(slotIs(&inv, 2, BLOCK_STONE, 4) && slotIs(&inv, 1, BLOCK_STONE, 4), "4 and 4");
    check(fake_sent_calls == 0, "nothing was handed to the transport");
}

int main(void)
{
    test_snapshot_that_arrived_before_the_hook_is_still_delivered();
    test_registration_without_a_snapshot_leaves_the_inventory_alone();
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

    /* v1.9.0 SPLIT */
    test_split_refuses_when_there_is_no_half();
    test_split_lifts_the_ceiling_half();
    test_place_lift_exact_fit_reports_a_move_from_the_origin();
    test_place_lift_overflow_keeps_the_remainder_lifted();
    test_place_lift_onto_a_different_item_refuses();
    test_place_lift_back_onto_its_origin_sends_nothing();
    test_return_lift_goes_to_the_origin_or_anywhere_free();
    test_quick_move_hotbar_to_main_and_back();
    test_quick_move_tops_up_same_item_first_then_spills();
    test_quick_move_to_a_full_strip_refuses();
    test_quick_move_moves_only_what_fits();
    test_plan_quick_move_describes_without_touching();
    test_lift_and_quick_move_offline_still_mutate();

    /* ---- check-count guard -----------------------------------------------------------------
     *
     * This suite counts failures, and until 2026-08-25 that was ALL it counted. A suite that
     * only counts failures cannot notice checks that never ran. Measured on net/networld_test.c
     * the same day: shrinking one production constant took it from "PASS 326 checks, 0 failed"
     * to "PASS 318 checks, 0 failed" — both green, exit 0, eight checks silently DELETED rather
     * than failed.
     *
     * Said plainly, because it is a real difference between this file and its neighbours: this
     * suite has no loop and no conditional that emits a check, so as it stands today no
     * production sabotage can DELETE a check from it — every check() call is straight-line and
     * unconditional, and the INV_SLOT_COUNT loops in it only fill slots. Measured, 2026-08-25:
     * INV_HOTBAR_SLOTS 8 -> 4 does not even link, because net/inv_bridge.c:14 already carries a
     * _Static_assert against the wire slot count. That is a fact about the file's shape TODAY,
     * not a property anyone maintains, and it is exactly the sort of thing a later edit removes
     * without noticing. The guard is therefore here for the two things that can still happen:
     * a test_*() call quietly dropped from the list above, and a check added without the pin
     * being updated.
     *
     * The number below is the count of checks that must already have run by the time control
     * reaches this line. It is a naked literal on purpose — it is the one number in this file
     * that is not derived from anything the tests themselves compute, which is precisely what
     * lets it notice them vanishing. A count derived from INV_SLOT_COUNT would move with the
     * very code it is supposed to be watching; that self-reference is the bug that let
     * networld_test.c's 326 -> 318 hide.
     *
     * HOW TO UPDATE IT WHEN YOU ADD OR REMOVE CHECKS — read this before changing the number:
     *   Work out the delta from what you actually changed (checks added minus checks removed)
     *   and ADD THAT DELTA to the number below. Do NOT paste whatever the failing run printed.
     *   Pasting the observed count is the single failure mode this guard exists to catch: if a
     *   production change silently deleted checks, the printed count is the SYMPTOM, and copying
     *   it in here re-arms the trap and throws away the only evidence you had. If your
     *   recomputed delta and the observed count disagree, that disagreement is a bug report — go
     *   and find out which checks stopped running, and why.
     *
     *   Note the number is the count BEFORE this guard itself, so the summary line prints one
     *   more than it (158 here, 159 on the PASS line). That off-by-one is deliberate: it means
     *   blind-pasting the number off the PASS line lands you a red, not a false green.
     *
     *   Latched into `ran` first, and compared through that, so the pin means "checks before
     *   this line" no matter how the check machinery is spelled. It matters: the macro-based
     *   suites in this fleet (world/region_growth_test.c, app/session_test.c) increment the
     *   counter BEFORE evaluating the condition, and a guard written against the live counter
     *   there silently wants a number one higher. Same latch everywhere, same meaning
     *   everywhere, and it stays correct if this file's check() is ever turned into a macro. */
    const int ran = g_checks;
    if (ran != 158)
        printf("\nCHECK-COUNT GUARD: %d checks ran, %d expected.\n"
               "  %s\n"
               "  This is NOT an ordinary assertion failure.\n"
               "  Read the comment above this guard in net/inv_bridge_test.c before"
               " touching the pinned number.\n",
               ran, 158,
               ran < 158
                   ? "Checks went MISSING: checks that should have run never ran at all."
                   : "Extra checks appeared: either you added checks and did not update the"
                     " pin, or something is emitting checks it should not.");
    check(ran == 158,
          "check-count guard: every check in this suite actually ran (158 before this line)");

    printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
