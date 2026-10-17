#include "search_rejections.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_ullong rejections[SEARCH_REJECTION_COUNT];

static const char *const limit_names[SEARCH_REJECTION_COUNT] = {
	"departure delta-v", "satellite delta-v", "total delta-v", "date or duration"
};

void reset_search_rejections(void) {
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) atomic_store(&rejections[cause], 0);
}

void count_search_rejection(enum SearchRejection cause) {
	if(cause >= 0 && cause < SEARCH_REJECTION_COUNT) atomic_fetch_add(&rejections[cause], 1);
}

Itin_Rejection_Counts get_search_rejections(void) {
	Itin_Rejection_Counts counts;
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) counts.by_cause[cause] = atomic_load(&rejections[cause]);
	return counts;
}

void describe_search_rejections(const Itin_Rejection_Counts *counts, char *text, size_t size) {
	if(size == 0) return;
	text[0] = '\0';
	unsigned long long total = 0;
	int worst = 0;
	for(int cause = 0; cause < SEARCH_REJECTION_COUNT; cause++) {
		total += counts->by_cause[cause];
		if(counts->by_cause[cause] > counts->by_cause[worst]) worst = cause;
	}
	if(total == 0) return;
	snprintf(text, size, "Most routes (%llu of %llu) were rejected by the %s limit; try raising it.",
		counts->by_cause[worst], total, limit_names[worst]);
}
