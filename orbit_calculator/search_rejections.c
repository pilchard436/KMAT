#include "search_rejections.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_ullong rejections[SEARCH_REJECTION_COUNT];
static atomic_ullong complete_rejections[SEARCH_REJECTION_COUNT];

static const char *const limit_names[SEARCH_REJECTION_COUNT] = {
	"departure delta-v", "satellite delta-v", "total delta-v", "date or duration"
};

void reset_search_rejections(void) {
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) {
		atomic_store(&rejections[cause], 0);
		atomic_store(&complete_rejections[cause], 0);
	}
}

void count_search_rejection(enum SearchRejection cause) {
	if(cause >= 0 && cause < SEARCH_REJECTION_COUNT) atomic_fetch_add(&rejections[cause], 1);
}

void count_complete_route_rejection(enum SearchRejection cause) {
	if(cause < 0 || cause >= SEARCH_REJECTION_COUNT) return;
	atomic_fetch_add(&rejections[cause], 1);
	atomic_fetch_add(&complete_rejections[cause], 1);
}

Itin_Rejection_Counts get_search_rejections(void) {
	Itin_Rejection_Counts counts;
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) {
		counts.by_cause[cause] = atomic_load(&rejections[cause]);
		counts.complete_by_cause[cause] = atomic_load(&complete_rejections[cause]);
	}
	return counts;
}

void describe_search_rejections(const Itin_Rejection_Counts *counts, char *text, size_t size) {
	if(size == 0) return;
	text[0] = '\0';
	// Early legs are dropped by the thousands in every search, so they only explain an empty result when no route
	// reached the target at all.
	unsigned long long complete_total = 0;
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) complete_total += counts->complete_by_cause[cause];
	const unsigned long long *by_cause = complete_total > 0 ? counts->complete_by_cause : counts->by_cause;
	unsigned long long total = 0;
	int worst = 0;
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) {
		total += by_cause[cause];
		if(by_cause[cause] > by_cause[worst]) worst = cause;
	}
	if(total == 0) return;
	if(complete_total > 0)
		snprintf(text, size, "Most routes that reached the target (%llu of %llu) were rejected by the %s limit; try raising it.",
			by_cause[worst], total, limit_names[worst]);
	else
		snprintf(text, size, "No route reached the target; most legs (%llu of %llu) were rejected by the %s limit.",
			by_cause[worst], total, limit_names[worst]);
}
