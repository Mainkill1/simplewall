#include "packet_queue.h"
#include <assert.h>
int main(void)
{
	PH_BUDGET b = {0};
	unsigned i;
	for (i = 0; i < 8192; i++)
		assert(ph_reserve(&b, 1));
	assert(!ph_reserve(&b, 1));
	assert(b.count == 8192 && b.bytes == 8192);
	for (i = 0; i < 8192; i++)
		ph_release(&b, 1);
	assert(b.bytes == 0 && b.count == 0);
	assert(ph_reserve(&b, 32 * 1024 * 1024));
	assert(!ph_reserve(&b, 1));
	ph_release(&b, 32 * 1024 * 1024);
	b.closed = 1;
	assert(!ph_reserve(&b, 1));
	return 0;
}
