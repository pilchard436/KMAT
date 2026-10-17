#ifndef KMAT_SEARCH_CANCEL_H
#define KMAT_SEARCH_CANCEL_H

#include "tools/thread_pool.h"

// Counter 3 is raised by the GUI to abort a running itinerary search.
static inline int itinerary_search_cancelled(void *context) {
	(void)context;
	return get_thread_counter(3) > 0;
}

#endif
