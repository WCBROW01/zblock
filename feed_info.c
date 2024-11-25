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
	
	return timegm(&tm);
}

// check if the feed currently exists. the result is in the exists pointer.
zblock_feed_info_err zblock_feed_info_exists(PGconn *conn, const char *url, u64snowflake channel_id, int *exists) {
	if (!conn || !exists) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	uint64_t channel_id_be = htobe64(channel_id);
	const char *const params[] = {url, (char *) channel_id_be};
	const int param_lengths[] = {0, sizeof(channel_id_be)};
	const int param_formats[] = {0, 1};
	PGresult *res = PQexecParams(conn,
		"SELECT COUNT(1) FROM feeds WHERE url = $1 AND channel_id = $2::bigint",
		2, NULL, params, param_lengths, param_formats, 1
	);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		log_error(PQresultErrorMessage(res));
		PQclear(res);
		return ZBLOCK_FEED_INFO_DBERROR;
	}
	
	*exists = be64toh(*(uint64_t *) PQgetvalue(res, 0, 0));
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

	uint64_t channel_id_be = htobe64(feed->channel_id);
	uint64_t guild_id_be = htobe64(feed->guild_id);
	const char *const insert_params[] = {feed->url, feed->last_pubDate, (char *) channel_id_be, feed->title, (char *) guild_id_be};
	const int param_lengths[] = {0, 0, sizeof(channel_id_be), 0, sizeof(guild_id_be)};
	const int param_formats[] = {0, 0, 1, 0, 1};
	PGresult *insert_res = PQexecParams(conn,
		"INSERT INTO feeds (url, last_pubDate, channel_id, title, guild_id) VALUES ($1, $2, $3::bigint, $4, $5::bigint)",
		5, NULL, insert_params, param_lengths, param_formats, 1
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
	
	uint64_t channel_id_be = htobe64(channel_id);
	const char *const params[] = {url, (char *) channel_id_be};
	const int param_lengths[] = {0, sizeof(channel_id_be)};
	const int param_formats[] = {0, 1};
	PGresult *res = PQexecParams(conn,
		"DELETE FROM feeds WHERE url = $1 AND channel_id = $2::bigint",
		2, NULL, params, param_lengths, param_formats, 1
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
	
	uint64_t channel_id_be = htobe64(feed->channel_id);
	const char *const update_params[] = {feed->last_pubDate, feed->url, (char *) channel_id_be};
	const int param_lengths[] = {0, 0, sizeof(channel_id_be)};
	const int param_formats[] = {0, 0, 1};
	PGresult *update_res = PQexecParams(conn,
		"UPDATE feeds SET last_pubDate = $1 WHERE url = $2 AND channel_id = $3::bigint",
		3, NULL, update_params, param_lengths, param_formats, 1
	);
	
	zblock_feed_info_err result = ZBLOCK_FEED_INFO_OK;
	if (PQresultStatus(update_res) != PGRES_COMMAND_OK) {
		log_error("Failed to update feed: %s", PQresultErrorMessage(update_res));
		result = ZBLOCK_FEED_INFO_DBERROR;
	}
	
	PQclear(update_res);
	return result;
}
