/* Caller owns synchronization. Reservations cover queued AND in-flight records. */
#pragma once
#include <stddef.h>
typedef struct
{
	size_t count, bytes;
	int closed;
} PH_BUDGET;
static int ph_reserve(PH_BUDGET *b, size_t bytes)
{
	if (b->closed || b->count >= 8192 || bytes > 32u * 1024u * 1024u - b->bytes)
		return 0;
	b->count++;
	b->bytes += bytes;
	return 1;
}
static void ph_release(PH_BUDGET *b, size_t bytes)
{
	b->count--;
	b->bytes -= bytes;
}
