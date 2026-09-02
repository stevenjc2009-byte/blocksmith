#include "scene/ringorder.h"

#include <stdbool.h>

static RingOffset s_order[RING_ORDER_COUNT];
static bool       s_built;

static int key(RingOffset o)
{
	return (int)o.dx * (int)o.dx + (int)o.dz * (int)o.dz;
}

const RingOffset* ringOrder(void)
{
	if (s_built) return s_order;

	int n = 0;
	for (int dz = -RING_ORDER_MAX_RADIUS; dz <= RING_ORDER_MAX_RADIUS; dz++)
		for (int dx = -RING_ORDER_MAX_RADIUS; dx <= RING_ORDER_MAX_RADIUS; dx++)
			s_order[n++] = (RingOffset){(int8_t)dx, (int8_t)dz};

	// Insertion sort, and deliberately so. It is STABLE, which is the whole reason: a great
	// many of these offsets tie on dx*dx + dz*dz — (3,4) and (5,0) both key 25 — and a stable
	// sort leaves every tied group in the row-major order it was filled in above. That makes
	// the table a fixed sequence of bytes rather than one that depends on the sort's internal
	// pivot choices, so ringorder_test.c can assert the whole thing and a different libc or a
	// different optimisation level cannot quietly reorder the world's load pattern.
	//
	// O(n^2) on 169 elements, once per process, on a path that already blocks for a 117 ms
	// saveWorldDir() (see genStart in main.c). qsort would need a comparator, a tie-break
	// field to stay stable, and a <stdlib.h> this file otherwise does not want.
	for (int i = 1; i < n; i++) {
		const RingOffset v = s_order[i];
		const int vk = key(v);
		int j = i - 1;
		while (j >= 0 && key(s_order[j]) > vk) {
			s_order[j + 1] = s_order[j];
			j--;
		}
		s_order[j + 1] = v;
	}

	s_built = true;
	return s_order;
}
