#ifndef ZBLOCK_FEED_INFO_H
#define ZBLOCK_FEED_INFO_H

#include <concord/discord.h>

// TODO: last_pubDate doesn't actually work properly yet
typedef struct feed_info {
	char *title;
	char *url;
	char *last_pubDate;
	u64snowflake guild_id;
	u64snowflake channel_id;
	unsigned timer_id;
	unsigned feed_id;
} feed_info;

typedef enum {
	FEED_INFO_OK,
	FEED_INFO_NULL,
	FEED_INFO_FILEERROR,
	FEED_INFO_ERRORCOUNT
} feed_info_err;

/*
 * Free the feed info struct
 */
void feed_info_free(struct feed_info *feed);

/*
 * Get a string explaining a feed info error
 */
const char *feed_info_strerror(feed_info_err error);

/*
 * Saves feed info to given file
 */
feed_info_err feed_info_save_file(feed_info *feed);

/*
 * Reads feed info for given information from file and puts it in the provided struct
 */
feed_info_err feed_info_load_file(u64snowflake guild_id, u64snowflake channel_id, unsigned feed_id, feed_info *feed);

#endif
