#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <locale.h>
#include <time.h>

#include <curl/curl.h>

#include <concord/discord.h>
#include <concord/log.h>

#include <mrss.h>

#include <postgresql/libpq-fe.h>

#include "config.h"
#include "feed_info.h"

// Function pointer type for commands
typedef void (*command_func)(struct discord *, const struct discord_interaction *);

struct bot_command {
	struct discord_create_global_application_command cmd;
	const command_func func;
};

#define P99_PROTECT(...) __VA_ARGS__

// absolutely ridiculous preprocessor hack.
#define _CREATE_OPTIONS(options) &(struct discord_application_command_options) { .size = sizeof((struct discord_application_command_option[]) options) / sizeof(struct discord_application_command_option), .array = (struct discord_application_command_option[]) options }
#define CREATE_OPTIONS(...) _CREATE_OPTIONS(P99_PROTECT(__VA_ARGS__))

#define BOT_COMMAND_NOT_IMPLEMENTED() do { \
	struct discord_interaction_response res = { \
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE, \
		.data = &(struct discord_interaction_callback_data) { \
			.content = "This command has not been implemented yet." \
		} \
	}; \
	discord_create_interaction_response(client, event->id, event->token, &res, NULL); \
} while (0)

typedef struct {
	zblock_feed_info_minimal info;
	FILE *fp;
	char *buf;
	size_t bufsize;
} zblock_feed_buffer;

// the database connection
static PGconn *database_conn;

// this does not account for large-scale usage yet.
static void timer_retrieve_feeds(struct discord *client, struct discord_timer *timer) {
	// not doing anything with the timer yet
	(void) timer;

	// all of this is as asynchronous as I can reasonably make it
	CURLM *multi = curl_multi_init();
	if (!multi) {
		// oh no
		log_error("Unable to retrieve feed list: NULL pointer from curl_multi_init()");
		return;
	}

	if (!PQsendQueryParams(
		database_conn, "SELECT url, last_pubDate, channel_id from feeds",
		0, NULL, NULL, NULL, NULL, 1
	)) {
		log_error("Unable to retrieve feed list: %s", PQerrorMessage(database_conn));
		return;
	}
	PQsetSingleRowMode(database_conn);
	
	// put running handles up here so we can start transfers now instead of later
	int running_handles, total_feeds = 0;
	// get all the required feed info to send messages
	PGresult *database_res;
	while ((database_res = PQgetResult(database_conn))) {
		if (PQresultStatus(database_res) != PGRES_SINGLE_TUPLE) {
			if (PQresultStatus(database_res) != PGRES_TUPLES_OK) {
				log_error("Unable to retrieve feeds: %s", PQresultErrorMessage(database_res));			
			}
			goto db_loop_end;
		}
		
		++total_feeds;
		zblock_feed_buffer *feed_buffer = malloc(sizeof(*feed_buffer));
		if (!feed_buffer) {
			log_error("Failure allocating feed buffer: %s", strerror(errno));
			goto db_loop_end;
		}
		feed_buffer->info.url = strdup(PQgetvalue(database_res, 0, 0));
		if (!feed_buffer->info.url) {
			log_error("Failure allocating feed buffer: %s", strerror(errno));
			free(feed_buffer);
			goto db_loop_end;
		}
		feed_buffer->info.last_pubDate = strdup(PQgetvalue(database_res, 0, 1));
		if (!feed_buffer->info.url) {
			log_error("Failure allocating feed buffer: %s", strerror(errno));
			free(feed_buffer->info.url);
			free(feed_buffer);
			goto db_loop_end;
		}
		feed_buffer->info.channel_id = be64toh(*(uint64_t *) PQgetvalue(database_res, 0, 2));
		feed_buffer->fp = open_memstream(&feed_buffer->buf, &feed_buffer->bufsize);
		if (!feed_buffer->fp) {
			log_error("Unable to retrieve feed: %s", strerror(errno));
			free(feed_buffer->info.last_pubDate);
			free(feed_buffer->info.url);
			free(feed_buffer);
			goto db_loop_end;
		}
	
		CURL *feed_handle = curl_easy_init();
		if (!feed_handle) {
			fclose(feed_buffer->fp);
			free(feed_buffer->buf);
			free(feed_buffer->info.last_pubDate);
			free(feed_buffer->info.url);
			free(feed_buffer);
			goto db_loop_end;
		}
		
		curl_easy_setopt(feed_handle, CURLOPT_URL, feed_buffer->info.url);
		curl_easy_setopt(feed_handle, CURLOPT_WRITEDATA, feed_buffer->fp);
		curl_easy_setopt(feed_handle, CURLOPT_PRIVATE, feed_buffer);
		CURLMcode mc = curl_multi_add_handle(multi, feed_handle);
		if (mc) {
			log_error("Unable to retrieve feed: %s", curl_multi_strerror(mc));
			curl_easy_cleanup(feed_handle);
			fclose(feed_buffer->fp);
			free(feed_buffer->buf);
			free(feed_buffer->info.last_pubDate);
			free(feed_buffer->info.url);
			free(feed_buffer);
			goto db_loop_end;
		}
		curl_multi_perform(multi, &running_handles);
		
		db_loop_end:
		PQclear(database_res);
	}
	
	int successful_feeds = 0;
	// it's time
	do {
		CURLMcode mc = curl_multi_perform(multi, &running_handles);
		CURLMsg *msg;
		int msgs_in_queue;
		do {
			msg = curl_multi_info_read(multi, &msgs_in_queue);
			if (msg && msg->msg == CURLMSG_DONE) {
				CURL *handle = msg->easy_handle;
				// get our buffer out
				zblock_feed_buffer *feed_buffer;
				curl_easy_getinfo(handle, CURLINFO_PRIVATE, &feed_buffer);
				fclose(feed_buffer->fp); // close the file descriptor for the buffer (also flushes buffer)
				if (!msg->data.result) {
					// hell yeah parse that RSS feed
					mrss_t *mrss_feed;
					mrss_error_t mrss_err = mrss_parse_buffer(feed_buffer->buf, feed_buffer->bufsize, &mrss_feed);
					if (!mrss_err) {
						++successful_feeds;
						// get publication date of entries and send any new ones
						time_t last_pubDate_time = pubDate_to_time_t(feed_buffer->info.last_pubDate);
						mrss_item_t *item = mrss_feed->item;
						bool update_pubDate = false;
						while (item && pubDate_to_time_t(item->pubDate) > last_pubDate_time) {
							update_pubDate = true;
							
							// Send new entry in the feed
							char msg[DISCORD_MAX_MESSAGE_LEN];
							snprintf(msg, sizeof(msg), "### %s\n[%s](%s)", mrss_feed->title, mrss_feed->item->title, mrss_feed->item->link);
							struct discord_create_message res = { .content = msg };
							discord_create_message(client, feed_buffer->info.channel_id, &res, NULL);
							item = item->next;
						}
						
						if (update_pubDate) {
							zblock_feed_info_minimal updated_feed = feed_buffer->info;
							updated_feed.last_pubDate = mrss_feed->item->pubDate;
							zblock_feed_info_update(database_conn, &updated_feed);
						}
						
						// done with our feed!
						mrss_free(mrss_feed);
					} else {
						log_error("Error parsing feed at %s: %s\n", feed_buffer->info.url, mrss_strerror(mrss_err));
					}
				} else {
					log_error("Error downloading RSS feed at %s: %s\n", feed_buffer->info.url, curl_easy_strerror(msg->data.result));
				}
				
				// free our buffers
				curl_multi_remove_handle(multi, handle);
				curl_easy_cleanup(handle);
				free(feed_buffer->buf);
				free(feed_buffer->info.last_pubDate);
				free(feed_buffer->info.url);
				free(feed_buffer);
			}
		} while (msg);
		
		if (!mc && running_handles) {
			mc = curl_multi_poll(multi, NULL, 0, 300, NULL);
		}
		if (mc) {
			// figure out how to free all resources instead of crashing
			log_fatal("curl_multi_poll(): %s", curl_multi_strerror(mc));
			exit(1);
		}
	} while (running_handles);
	
	// processing is done
	curl_multi_cleanup(multi);
	log_info("Retrieved %d of %d feeds!", successful_feeds, total_feeds);
}

static void bot_command_add(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];
	zblock_feed_info feed;
	
	feed.url = event->data->options->array[0].value;
	feed.channel_id = event->channel_id;
	feed.guild_id = event->guild_id;
	
	mrss_t *mrss_feed;
	mrss_error_t mrss_error = mrss_parse_url(feed.url, &mrss_feed);
	if (mrss_error) {
		snprintf(msg, sizeof(msg), "Error adding feed: %s", mrss_strerror(mrss_error));
		goto send_msg;
	}
	
	feed.title = mrss_feed->title;
	feed.last_pubDate = mrss_feed->item->pubDate;
	
	zblock_feed_info_err insert_res = zblock_feed_info_insert(database_conn, &feed);
	if (insert_res) {
		// write error message
		snprintf(msg, sizeof(msg), "Error adding feed: %s", zblock_feed_info_strerror(insert_res));
	} else {
		// write the confirmation message
		snprintf(msg, sizeof(msg), "The following feed has been successfully added to this channel:\n`%s`", feed.url);
	}
	
	mrss_free(mrss_feed);
	
	send_msg:
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = msg
		}
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

static void bot_command_remove(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];
		
	char *url = event->data->options->array[0].value;
	zblock_feed_info_err error = zblock_feed_info_delete(database_conn, url, event->channel_id);
	if (error) {
		// write error message
		snprintf(msg, sizeof(msg), "Error removing feed: %s", zblock_feed_info_strerror(error));
	} else {
		// write the confirmation message
		snprintf(msg, sizeof(msg), "The following feed has been successfully removed from this channel:\n`%s`", url);
	}
	
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = msg
		}
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

static void bot_command_list(struct discord *client, const struct discord_interaction *event) {
	BOT_COMMAND_NOT_IMPLEMENTED();
}

static void bot_command_help(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];

	// intro message
	snprintf(
		msg, sizeof(msg),
		"Hello %s, welcome to zblock, a lightweight RSS bot for Discord!\n"
		"You can find the source code for this bot at https://github.com/WCBROW01/zblock\n"
		"Please submit any bugs or issues there, or feel free to make a pull request!",
		event->user ? event->user->username : event->member->user->username
	);

	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = msg
		}
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

static struct bot_command commands[] = {
	{
		.cmd = {
			.name = "add",
			.description = "Add an RSS feed",
			.default_permission = true,
			.options = CREATE_OPTIONS({
				{
					.type = DISCORD_APPLICATION_OPTION_STRING,
					.name  = "url",
					.description = "The URL of your feed",
					.required = true
				}
			})
		},
		.func  = &bot_command_add
	},
	{
		.cmd = {
			.name = "remove",
			.description = "Remove an RSS feed",
			.default_permission = true,
			.options = CREATE_OPTIONS({
				{
					.type = DISCORD_APPLICATION_OPTION_STRING,
					.name  = "url",
					.description = "The URL of your feed",
					.required = true
				}
			})
		},
		.func  = &bot_command_remove
	},
	{
		.cmd = {
			.name = "list",
			.description = "List the RSS feeds in the current channel",
			.default_permission = true
		},
		.func = &bot_command_list
	},
	{
		.cmd = {
			.name = "help",
			.description = "Get help on how to use the bot",
			.dm_permission = true
		},
		.func = &bot_command_help
	}
};

static void on_ready(struct discord *client, const struct discord_ready *event) {
	log_info("Logged in as %s!", event->user->username);

	// create commands
	for (struct bot_command *i = commands; i < commands + sizeof(commands) / sizeof(*commands); ++i) {
		discord_create_global_application_command(client, event->application->id, &i->cmd, NULL);
	}
}

static void on_interaction(struct discord *client, const struct discord_interaction *event) {
	if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND)
		return; // not a slash command

	// invoke the command
	for (struct bot_command *i = commands; i < commands + sizeof(commands) / sizeof(*commands); ++i) {
		if (!strcmp(event->data->name, i->cmd.name)) {
			i->func(client, event);
			return;
		}
	}

	// not a real command
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = &(struct discord_interaction_callback_data) {
			.content = "Invalid command, contact the maintainer of this bot."
		}
	};
	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
}

// delay before the first feed retrieval (in ms)
#define FEED_TIMER_DELAY 15000

// interval for the feed retrieval timer (in ms)
#define FEED_TIMER_INTERVAL 600000

int main(void) {
	int exit_code = 0;	
	
	// set locale for time
	setlocale(LC_ALL, "C");
	srand(time(NULL));
	struct discord *client = discord_config_init("config.json");
	zblock_config_load(client);
	
	// connect to database
	database_conn = PQconnectdb(zblock_config.conninfo);
	if (!database_conn) {
		log_fatal("Failed to connect to database.");
		exit_code = 1;
		goto cleanup;
	}
	
	discord_set_on_ready(client, &on_ready);
	discord_set_on_interaction_create(client, &on_interaction);
	discord_timer_interval(client, timer_retrieve_feeds, NULL, NULL, FEED_TIMER_DELAY, FEED_TIMER_INTERVAL, -1);
	discord_run(client);
	
	PQfinish(database_conn);
	cleanup:
	discord_cleanup(client);
	ccord_global_cleanup();
	
	return exit_code;
}
