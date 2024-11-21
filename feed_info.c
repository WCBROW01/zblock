#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <concord/discord.h>
#include <concord/log.h>

#include "config.h"
#include "feed_info.h"

void feed_info_free(feed_info *feed) {
	free(feed->title);
	free(feed->url);
	free(feed->last_pubDate);
	free(feed);
}

const char *feed_info_strerror(feed_info_err error) {
	static_assert(FEED_INFO_ERRORCOUNT == 3, "Not all feed info errors implemented");
	switch (error) {
		case FEED_INFO_OK: {
			return "OK";
		}
		case FEED_INFO_FILEERROR: {
			return strerror(errno);
		}
		case FEED_INFO_NULL: {
			return "No feed info was provided";
		}
		default: {
			return "Unspecified error";
		}
	}
}

/*
 * Reads feed info for given information from file and puts it in the provided struct
 */
feed_info_err feed_info_save_file(feed_info *feed) {
	if (!feed) return FEED_INFO_NULL;
	
	char file_path[PATH_MAX];
	// maybe check if we ran out of characters for path?
	snprintf(file_path, sizeof(file_path), "%s/%lu/%lu/%x", zblock_config.database_path, feed->guild_id, feed->channel_id, feed->feed_id);
	
	FILE *fp = fopen(file_path, "w");
	if (!fp) return FEED_INFO_FILEERROR;
	fprintf(fp, "title=%s\nurl=%s\nlast_pubDate=%s\n", feed->title, feed->url, feed->last_pubDate);
	fclose(fp);
}

feed_info_err feed_info_load_file(u64snowflake guild_id, u64snowflake channel_id, unsigned feed_id, feed_info *feed) {
	assert(0 && "not implemented yet");
}
