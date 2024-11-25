#ifndef ZBLOCK_FEED_INFO_H
#define ZBLOCK_FEED_INFO_H

#include <concord/discord.h>

#include <postgresql/libpq-fe.h>

typedef struct {
	char *url;
	char *last_pubDate;
	u64snowflake channel_id;
} zblock_feed_info_minimal;

typedef struct {
	// same definition as feed_info_minimal
	char *url;
	char *last_pubDate;
	u64snowflake channel_id;
	// extra things
	char *title;
	u64snowflake guild_id;
} zblock_feed_info;

typedef enum {
	ZBLOCK_FEED_INFO_OK,
	ZBLOCK_FEED_INFO_INVALID_ARGS,
	ZBLOCK_FEED_INFO_DBERROR,
	ZBLOCK_FEED_INFO_EXISTS,
	ZBLOCK_FEED_INFO_NOT_EXIST,
	ZBLOCK_FEED_INFO_ERRORCOUNT
} zblock_feed_info_err;

// maybe change the function signature so you can actually do error handling with the result?
time_t pubDate_to_time_t(char *s);

// returns a string about the result of a feed_info function
const char *zblock_feed_info_strerror(zblock_feed_info_err error);

// check if the feed currently exists. the result is in the exists pointer.
zblock_feed_info_err zblock_feed_info_exists(PGconn *conn, const char *url, u64snowflake channel_id, int *exists);

// Insert new feed into the database
zblock_feed_info_err zblock_feed_info_insert(PGconn *conn, zblock_feed_info *feed);

// deletes feed from the database
zblock_feed_info_err zblock_feed_info_delete(PGconn *conn, const char *url, u64snowflake channel_id);

// updates the last_pubDate field of a given feed in the database
zblock_feed_info_err zblock_feed_info_update(PGconn *conn, zblock_feed_info_minimal *feed);

// returns the number of feeds in a channel in count
zblock_feed_info_err zblock_feed_info_count_channel(PGconn *conn, u64snowflake channel_id, int64_t *count)

#endif
