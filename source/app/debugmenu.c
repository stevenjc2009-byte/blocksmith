#include "app/debugmenu.h"
#include <stddef.h>

static DebugEntry s_pool[DEBUG_MENU_MAX_ENTRIES];
static int s_count;
static int s_head = -1;

void debugMenuReset(void)
{
	for (int i = 0; i < DEBUG_MENU_MAX_ENTRIES; i++) {
		s_pool[i].name     = NULL;
		s_pool[i].next     = -1;
		s_pool[i].available = true;
	}
	s_count = 0;
	s_head  = -1;
}

DebugEntry* debugMenuRegister(void)
{
	if (s_count >= DEBUG_MENU_MAX_ENTRIES) return NULL;

	int idx = s_count++;
	DebugEntry* e = &s_pool[idx];

	// Clear the slot so a stale entry from a previous test run cannot leak.
	e->name        = NULL;
	e->kind        = DEBUG_TOGGLE;
	e->available   = true;
	e->getBool     = NULL;
	e->setBool     = NULL;
	e->getInt      = NULL;
	e->setInt      = NULL;
	e->slider_min  = 0;
	e->slider_max  = 0;
	e->action      = NULL;
	e->info        = NULL;
	e->ctx         = NULL;
	e->next        = -1;

	// Append to the tail of the linked list.
	if (s_head < 0) {
		s_head = idx;
	} else {
		int cur = s_head;
		while (s_pool[cur].next >= 0)
			cur = s_pool[cur].next;
		s_pool[cur].next = idx;
	}

	return e;
}

int debugMenuCount(void) { return s_count; }

DebugEntry* debugMenuEntry(int index)
{
	if (index < 0 || index >= s_count) return NULL;
	return &s_pool[index];
}

int debugMenuHead(void) { return s_head; }

int debugMenuNext(int index)
{
	if (index < 0 || index >= s_count) return -1;
	return s_pool[index].next;
}
