#define _GNU_SOURCE
#define _XOPEN_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>

#include <concord/discord.h>
#include <concord/log.h>

#include <postgresql/libpq-fe.h>

#include "config.h"
#include "feed_info.h"

static const char *ZBLOCK_FEED_INFO_ERRORS[] = {
	"OK",
	"Invalid arguments provided",
	"An error was encountered with the feed database",
	"The feed already exists",
	"The feed does not exist"
};
static_assert(sizeof(ZBLOCK_FEED_INFO_ERRORS) / sizeof(*ZBLOCK_FEED_INFO_ERRORS) == ZBLOCK_FEED_INFO_ERRORCOUNT, "Not all feed info errors implemented");

// returns a string about the result of a feed_info function
const char *zblock_feed_info_strerror(zblock_feed_info_err error) {
	return error < 0 || error >= ZBLOCK_FEED_INFO_ERRORCOUNT ? "Unspecified error" : ZBLOCK_FEED_INFO_ERRORS[error];
}

time_t pubDate_to_time_t(char *s) {
	struct tm tm;
	
	// time format with e.g. +0000
	if (!strptime(s, "%a, %d %b %Y %T %z", &tm)) { // try the other time format with timezone
		if(!strptime(s, "%a, %d %b %Y %T %Z", &tm)) return 0; // invalid time
	}
	
	return mktime(&tm);
}

// check if the feed currently exists. the result is in the exists pointer.
zblock_feed_info_err zblock_feed_info_exists(PGconn *conn, const char *url, u64snowflake channel_id, int *exists) {
	if (!conn || !exists) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	char channel_id_str[21]; // hold a 64-bit int in decimal form
	snprintf(channel_id_str, sizeof(channel_id_str), "%" PRId64, channel_id);
	
	const char *const params[] = {url, channel_id_str};
	PGresult *res = PQexecParams(conn, "SELECT COUNT(1) FROM feeds WHERE url = $1 AND channel_id = $2", 2, NULL, params, NULL, NULL, 1);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		log_error(PQresultErrorMessage(res));
		PQclear(res);
		return ZBLOCK_FEED_INFO_DBERROR;
	}
	
	*exists = be32toh(*(uint32_t *) PQgetvalue(res, 0, 0));
	PQclear(res);
	return ZBLOCK_FEED_INFO_OK;
}

// Insert new feed into the database
zblock_feed_info_err zblock_feed_info_insert(PGconn *conn, zblock_feed_info *feed) {
	if (!conn || !feed) return ZBLOCK_FEED_INFO_INVALID_ARGS;

	// check if the feed already exists
	{
		int feed_exists;
		zblock_feed_info_err exists_error = zblock_feed_info_exists(conn, feed->url, feed->channel_id, &feed_exists);
		if (exists_error) {
			return exists_error;
		} else if (feed_exists) {
			return ZBLOCK_FEED_INFO_EXISTS;
		}	
	}

	// I don't want to deal with the extra fuss that is sending these in binary format
	char channel_id_str[21];
	snprintf(channel_id_str, sizeof(channel_id_str), "%" PRId64, feed->channel_id);
	char guild_id_str[21];
	snprintf(guild_id_str, sizeof(guild_id_str), "%" PRId64, feed->guild_id);
	const char *const insert_params[] = {feed->url, feed->last_pubDate, channel_id_str, feed->title, guild_id_str};
	PGresult *insert_res = PQexecParams(conn,
		"INSERT INTO feeds (url, last_pubDate, channel_id, title, guild_id) VALUES ($1, $2, $3, $4, $5)",
		5, NULL, insert_params, NULL, NULL, 0
	);
	
	zblock_feed_info_err result = ZBLOCK_FEED_INFO_OK;
	if (PQresultStatus(insert_res) != PGRES_COMMAND_OK) {
		log_error(PQresultErrorMessage(insert_res));
		result = ZBLOCK_FEED_INFO_DBERROR;
	}
	
	PQclear(insert_res);
	return result;
}

// deletes feed from the database
zblock_feed_info_err zblock_feed_info_delete(PGconn *conn, const char *url, u64snowflake channel_id) {
	if (!conn) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	// check if the feed already exists
	{
		int feed_exists;
		zblock_feed_info_err exists_error = zblock_feed_info_exists(conn, url, channel_id, &feed_exists);
		if (exists_error) {
			return exists_error;
		} else if (!feed_exists) {
			return ZBLOCK_FEED_INFO_NOT_EXIST;
		}
	}
	
	// I don't want to deal with the extra fuss that is sending these in binary format
	char channel_id_str[21];
	snprintf(channel_id_str, sizeof(channel_id_str), "%" PRId64, channel_id);
	const char *const params[] = {url, channel_id_str};
	PGresult *res = PQexecParams(conn,
		"DELETE FROM feeds WHERE url = $1 AND channel_id = $2",
		2, NULL, params, NULL, NULL, 0
	);
	
	zblock_feed_info_err result = ZBLOCK_FEED_INFO_OK;
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		log_error(PQresultErrorMessage(res));
		result = ZBLOCK_FEED_INFO_DBERROR;
	}
	
	PQclear(res);
	return result;
}

// updates the last_pubDate field of a given feed in the database
zblock_feed_info_err zblock_feed_info_update(PGconn *conn, zblock_feed_info_minimal *feed) {
	if (!conn || !feed) return ZBLOCK_FEED_INFO_INVALID_ARGS;

	char channel_id_str[21]; // hold a 64-bit int in decimal form
	snprintf(channel_id_str, sizeof(channel_id_str), "%" PRId64, feed->channel_id);
	
	const char *const update_params[] = {feed->last_pubDate, feed->url, channel_id_str};
	// save the updated pubDate to disk once that's implemented
	PGresult *update_res = PQexecParams(conn,
		"UPDATE feeds SET last_pubDate = $1 WHERE url = $2 AND channel_id = $3",
		3, NULL, update_params, NULL, NULL, 0
	);
	
	zblock_feed_info_err result = ZBLOCK_FEED_INFO_OK;
	if (PQresultStatus(update_res) != PGRES_COMMAND_OK) {
		log_error("Failed to update feed: %s", PQresultErrorMessage(update_res));
		result = ZBLOCK_FEED_INFO_DBERROR;
	}
	
	PQclear(update_res);
	return result;
}
