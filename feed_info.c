#define _GNU_SOURCE
#define _XOPEN_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <concord/discord.h>
#include <concord/log.h>

#include <postgresql/libpq-fe.h>

#include "config.h"
#include "feed_info.h"

// returns a string about the result of a feed_info function
const char *zblock_feed_info_strerror(zblock_feed_info_err error) {
	static_assert(ZBLOCK_FEED_INFO_ERRORCOUNT == 3, "Not all feed info errors implemented");
	switch (error) {
		case ZBLOCK_FEED_INFO_OK: {
			return "OK";
		}
		case ZBLOCK_FEED_INFO_NULL: {
			return "No feed info was provided";
		}
		case ZBLOCK_FEED_INFO_POSTGRES: {
			return "An error was encountered with the feed database";
		}
		default: {
			return "Unspecified error";
		}
	}
}

// format string for for the time format of pubDate
#define PUBDATE_FMT "%a, %d %b %Y %T %z"

time_t pubDate_to_time_t(char *s) {
	struct tm tm;
	char *res = strptime(s, PUBDATE_FMT, &tm);
	if (!res || !*res) return 0; // invalid time
	
	return mktime(&tm);
}

// Insert new feed into the database
zblock_feed_info_err zblock_feed_info_insert(PGconn *conn, zblock_feed_info *feed) {
	assert(0 && "not implemented yet");
}
