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
	"The feed does not exist",
	"Finished getting feed info",
	"Out of memory"
};
static_assert(sizeof(ZBLOCK_FEED_INFO_ERRORS) / sizeof(*ZBLOCK_FEED_INFO_ERRORS) == ZBLOCK_FEED_INFO_ERRORCOUNT, "Not all feed info errors implemented");

// free all information associated with a minimal feed info struct (does not assume the struct was allocated using malloc)
void zblock_feed_info_minimal_free(zblock_feed_info_minimal *feed_info) {
	free(feed_info->last_pubDate);
	free(feed_info->url);
}

// free all information associated with a feed info struct (does not assume the struct was allocated using malloc)
void zblock_feed_info_free(zblock_feed_info *feed_info) {
	free(feed_info->title);
	zblock_feed_info_minimal_free((zblock_feed_info_minimal *) feed_info);
}

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

// Begin retrieval of feed info objects.
zblock_feed_info_err zblock_feed_info_retrieve_list_begin(PGconn *conn) {
	if (!conn) return ZBLOCK_FEED_INFO_INVALID_ARGS;

	if (!PQsendQueryParams(
		conn, "SELECT url, last_pubDate, channel_id from feeds",
		0, NULL, NULL, NULL, NULL, 1
	)) {
		return ZBLOCK_FEED_INFO_DBERROR;
	}
	PQsetSingleRowMode(conn);
	return ZBLOCK_FEED_INFO_OK;
}

// Retrieve the next feed list object.
// On error, no more objects can be retrieved and the returned object is invalid.
zblock_feed_info_err zblock_feed_info_retrieve_list_item(PGconn *conn, zblock_feed_info_minimal *feed_info) {
	if (!conn || !feed_info) return ZBLOCK_FEED_INFO_INVALID_ARGS;

	PGresult *res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_SINGLE_TUPLE) {
		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			log_error("Unable to retrieve feeds: %s", PQresultErrorMessage(res));	
			PQclear(res);
			PQgetResult(conn); // thrown out because it should be null
			return ZBLOCK_FEED_INFO_DBERROR;		
		} else {
			PQclear(res);
			// we need to do this one last time or the next query won't work
			PQgetResult(conn); // thrown out because it should be null
			return ZBLOCK_FEED_INFO_FINISHED;		
		}
	}

	feed_info->url = strdup(PQgetvalue(res, 0, 0));
	feed_info->last_pubDate = strdup(PQgetvalue(res, 0, 1));
	if (!feed_info->url || !feed_info->last_pubDate) {
		PQclear(res);
		return ZBLOCK_FEED_INFO_NOMEM;
	}
	feed_info->channel_id = be64toh(*(uint64_t *) PQgetvalue(res, 0, 2));
	
	PQclear(res);
	return ZBLOCK_FEED_INFO_OK;
}

// check if the feed currently exists. the result is in the exists pointer.
zblock_feed_info_err zblock_feed_info_exists(PGconn *conn, const char *url, u64snowflake channel_id, int *exists) {
	if (!conn || !exists) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	uint64_t channel_id_be = htobe64(channel_id);
	const char *const params[] = {url, (char *) &channel_id_be};
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
	const char *const insert_params[] = {feed->url, feed->last_pubDate, (char *) &channel_id_be, feed->title, (char *) &guild_id_be};
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
	const char *const params[] = {url, (char *) &channel_id_be};
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

// deletes all feeds associated with a guild from the database
zblock_feed_info_err zblock_feed_info_delete_all_guild(PGconn *conn, u64snowflake guild_id) {
	if (!conn) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	uint64_t guild_id_be = htobe64(guild_id);
	const char *const params[] = {(char *) &guild_id_be};
	const int param_lengths[] = {sizeof(guild_id_be)};
	const int param_formats[] = {1};
	PGresult *res = PQexecParams(conn,
		"DELETE FROM feeds WHERE guild_id = $1::bigint",
		1, NULL, params, param_lengths, param_formats, 1
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
	const char *const update_params[] = {feed->last_pubDate, feed->url, (char *) &channel_id_be};
	const int param_lengths[] = {0, 0, sizeof(channel_id_be)};
	const int param_formats[] = {0, 0, 1};
	PGresult *update_res = PQexecParams(conn,
		"UPDATE feeds SET last_pubDate = $1 WHERE url = $2 AND channel_id = $3::bigint",
		3, NULL, update_params, param_lengths, param_formats, 1
	);
	
	zblock_feed_info_err result = ZBLOCK_FEED_INFO_OK;
	if (PQresultStatus(update_res) != PGRES_COMMAND_OK) result = ZBLOCK_FEED_INFO_DBERROR;
	PQclear(update_res);
	return result;
}

// returns the number of feeds in a channel in count
zblock_feed_info_err zblock_feed_info_count_channel(PGconn *conn, u64snowflake channel_id, int64_t *count) {
	if (!conn || !count) return ZBLOCK_FEED_INFO_INVALID_ARGS;

	uint64_t channel_id_be = htobe64(channel_id);
	const char *const params[] = {(char *) &channel_id_be};
	const int param_lengths[] = {sizeof(channel_id_be)};
	const int param_formats[] = {1};
	PGresult *res = PQexecParams(conn,
		"SELECT COUNT(*) FROM feeds WHERE channel_id = $1::bigint",
		1, NULL, params, param_lengths, param_formats, 1
	);
	
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		log_error(PQresultErrorMessage(res));
		PQclear(res);
		return ZBLOCK_FEED_INFO_DBERROR;
	}
	
	*count = be64toh(*(uint64_t *) PQgetvalue(res, 0, 0));
	PQclear(res);
	return ZBLOCK_FEED_INFO_OK;
}

// returns a chunk of feeds for the given channel in the array provided by chunk.
// assumes chunk was preallocated with the number of elements in size
// num_retrieved is an optional parameter that will contain the number of feeds actually retrieved (in case it is less)
zblock_feed_info_err zblock_feed_info_retrieve_chunk_channel(PGconn *conn, u64snowflake channel_id, uint64_t offset, size_t size, zblock_feed_info *chunk, int *num_retrieved) {
	if (!conn || !chunk) return ZBLOCK_FEED_INFO_INVALID_ARGS;
	
	uint64_t channel_id_be = htobe64(channel_id);
	uint64_t offset_be = htobe64(offset);
	uint64_t size_be = htobe64(size);
	
	const char *const params[] = {(char *) &channel_id_be, (char *) &offset_be, (char *) &size_be};
	const int param_lengths[] = {sizeof(channel_id_be), sizeof(offset_be), sizeof(size_be)};
	const int param_formats[] = {1, 1, 1};
	PGresult *res = PQexecParams(conn,
		"SELECT url, last_pubDate, channel_id, title, guild_id FROM feeds WHERE channel_id = $1::bigint OFFSET $2::bigint LIMIT $3::bigint",
		3, NULL, params, param_lengths, param_formats, 1
	);
	
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		log_error(PQresultErrorMessage(res));
		PQclear(res);
		return ZBLOCK_FEED_INFO_DBERROR;
	}
	
	int nfeeds = PQntuples(res);
	if (num_retrieved) *num_retrieved = nfeeds;
	for (int i = 0; i < nfeeds; ++i) {
		chunk[i].url = strdup(PQgetvalue(res, i, 0));
		chunk[i].last_pubDate = strdup(PQgetvalue(res, i, 1));
		chunk[i].channel_id = be64toh(*(uint64_t *) PQgetvalue(res, i, 2));
		chunk[i].title = strdup(PQgetvalue(res, i, 3));
		chunk[i].guild_id = be64toh(*(uint64_t *) PQgetvalue(res, i, 4));
		
		if (!chunk[i].url || !chunk[i].last_pubDate || !chunk[i].title) {
			PQclear(res);
			return ZBLOCK_FEED_INFO_NOMEM;
		}
	}
	
	PQclear(res);
	return ZBLOCK_FEED_INFO_OK;
}
