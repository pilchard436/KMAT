#ifndef KMAT_SEARCH_REJECTIONS_H
#define KMAT_SEARCH_REJECTIONS_H

#include <stddef.h>

/* Counts the routes of the running search that a user limit removed, so an empty result can name the limit. */

enum SearchRejection {REJECT_DEPARTURE_DV, REJECT_SATELLITE_DV, REJECT_TOTAL_DV, REJECT_TIME_LIMIT, SEARCH_REJECTION_COUNT};

typedef struct Itin_Rejection_Counts {
	unsigned long long by_cause[SEARCH_REJECTION_COUNT];
	// The part of by_cause that removed routes which had already reached the target.
	unsigned long long complete_by_cause[SEARCH_REJECTION_COUNT];
} Itin_Rejection_Counts;

void reset_search_rejections(void);
// Safe to call from the search worker threads.
void count_search_rejection(enum SearchRejection cause);
// For a route that reached the target; also counted by count_search_rejection's totals.
void count_complete_route_rejection(enum SearchRejection cause);
Itin_Rejection_Counts get_search_rejections(void);
// Writes a sentence naming the limit behind most rejections of routes that reached the target, or of all routes
// when none did; empty if no limit rejected anything.
void describe_search_rejections(const Itin_Rejection_Counts *counts, char *text, size_t size);

#endif
