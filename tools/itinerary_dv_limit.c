#include "tools/itinerary_dv_limit.h"

#include <glib.h>
#include <stdlib.h>

double parse_itinerary_dv_limit(const char *text) {
	if(text == NULL) return ITINERARY_UNBOUNDED_DV_LIMIT;
	while(g_ascii_isspace(*text)) text++;
	return *text == '\0' ? ITINERARY_UNBOUNDED_DV_LIMIT : strtod(text, NULL);
}
