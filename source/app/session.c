// See session.h for the contract and for the defect this file exists for.
#include "app/session.h"

#include "world/registry.h"

void sessionBegin(void)
{
	// The whole table, not just the frozen flag. An unfreeze on its own would have fixed
	// half of it and left the harder half: registryRemoteApply() also refuses any batch
	// that does not start at the table's own next free slot, and a single-player world's
	// registry.bin rows had already moved that slot past REG_ID_DYN_LO. The server's
	// batch always starts at REG_ID_DYN_LO, so it would still have been refused — and
	// the previous world's rows would still have been sitting under the ids the next
	// world means to use. registryInitCore() is the one call that puts all three back:
	// the rows, the next-free cursor and the freeze.
	//
	// Safe to call here and only here. The table is read without locks by the worker
	// thread from registryFreeze() onwards (world/registry.h), and this runs on the
	// session lap — after workerStop() has joined that thread and worldExit() has freed
	// the world it was writing into, and before genStart() starts the next one.
	registryInitCore();
}
