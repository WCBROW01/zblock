#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <locale.h>
#include <time.h>
#include <pthread.h>

#include <curl/curl.h>

#include <concord/discord.h>
#include <concord/log.h>

#include <mrss.h>

#include <postgresql/libpq-fe.h>

#include "config.h"
#include "feed_info.h"
#include "arena.h"

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

#define _CREATE_EMBEDS(embeds) &(struct discord_embeds) { .size = sizeof((struct discord_embed[]) embeds) / sizeof(struct discord_embed), .array = (struct discord_embed[]) embeds }
#define CREATE_EMBEDS(...) _CREATE_EMBEDS(P99_PROTECT(__VA_ARGS__))

#define _CREATE_COMPONENTS(components) &(struct discord_components) { .size = sizeof((struct discord_component[]) components) / sizeof(struct discord_component), .array = (struct discord_component[]) components }
#define CREATE_COMPONENTS(...) _CREATE_COMPONENTS(P99_PROTECT(__VA_ARGS__))

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
static void *thread_retrieve_feeds(void *arg) {
	struct discord *client = arg;

	// all of this is as asynchronous as I can reasonably make it
	CURLM *multi = curl_multi_init();
	if (!multi) {
		// oh no
		log_error("Unable to retrieve feed list: NULL pointer from curl_multi_init()");
		return NULL;
	}
	
	PGconn *database_conn = PQconnectdb(zblock_config.conninfo); // yes i know this name is reused
	if (!database_conn) {
		log_error("Failed to connect to database.");
		curl_multi_cleanup(multi);
		return NULL;
	}
	
	// Begin retrieval of feed list objects.
	if (zblock_feed_info_retrieve_list_begin(database_conn)) {
		log_error("Unable to retrieve feed list: %s", PQerrorMessage(database_conn));
		curl_multi_cleanup(multi);
		return NULL;
	}
	
	// put running handles up here so we can start transfers now instead of later
	int running_handles, total_feeds = 0;
	// get all the required feed info to send messages
	zblock_feed_info_minimal feed_info;
	while (!zblock_feed_info_retrieve_list_item(database_conn, &feed_info)) {
		++total_feeds;
		zblock_feed_buffer *feed_buffer = malloc(sizeof(*feed_buffer));
		if (!feed_buffer) {
			log_error("Failure allocating feed buffer: %s", strerror(errno));
			continue;
		}
		feed_buffer->info = feed_info;
		feed_buffer->fp = open_memstream(&feed_buffer->buf, &feed_buffer->bufsize);
		if (!feed_buffer->fp) {
			log_error("Unable to retrieve feed: %s", strerror(errno));
			zblock_feed_info_minimal_free(&feed_buffer->info);
			free(feed_buffer);
			continue;
		}
	
		CURL *feed_handle = curl_easy_init();
		if (!feed_handle) {
			fclose(feed_buffer->fp);
			free(feed_buffer->buf);
			zblock_feed_info_minimal_free(&feed_buffer->info);
			free(feed_buffer);
			continue;
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
			zblock_feed_info_minimal_free(&feed_buffer->info);
			free(feed_buffer);
			continue;
		}
		curl_multi_perform(multi, &running_handles);
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
				zblock_feed_info_minimal_free(&feed_buffer->info);
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
	PQfinish(database_conn);
	log_info("Retrieved %d of %d feeds!", successful_feeds, total_feeds);
	return NULL;
}

static void timer_retrieve_feeds(struct discord *client, struct discord_timer *timer) {
	// not doing anything with the timer
	(void) timer;
	
	pthread_t retrieve_thread;
	pthread_create(&retrieve_thread, NULL, &thread_retrieve_feeds, client);
}

static void timer_tuesday_event(struct discord *client, struct discord_timer *timer) {
	// not doing anything with the timer
	(void) timer;

	struct discord_create_message msg = {
		.content = "https://tenor.com/view/happy-tuesday-its-tuesday-tuesday-dance-default-mario-gif-15064439"
	};

	discord_create_message(client, zblock_config.tuesday_channel, &msg, NULL);
}

static void bot_command_add(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];
	zblock_feed_info feed;
	
	feed.url = event->data->options->array[0].value;
	feed.channel_id = event->channel_id;
	feed.guild_id = event->guild_id;

	// check if the feed already exists
	{
		int feed_exists;
		zblock_feed_info_err exists_error = zblock_feed_info_exists(database_conn, feed.url, feed.channel_id, &feed_exists);
		if (exists_error) {
			snprintf(msg, sizeof(msg), "Error adding feed: %s", zblock_feed_info_strerror(exists_error));
			goto send_msg;
		} else if (feed_exists) {
			snprintf(msg, sizeof(msg), "Error adding feed: It has already been added to this channel");
			goto send_msg;
		}
	}
	
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

#define LIST_PAGE_SIZE 5

// The arena everything gets allocated to will be returned in the arena pointer
static struct discord_interaction_callback_data *list_data_create(u64snowflake channel_id, int page_number, Arena **arena) {
	assert(arena && "No arena provided"); // this is programmer error
	// clamp page number
	page_number = page_number < 1 ? 1 : page_number;
	
	*arena = Arena_new(8192); // this should be more than enough
	struct discord_interaction_callback_data *data = Arena_allocz(*arena, sizeof(*data));
	
	int64_t count;
	zblock_feed_info_err error = zblock_feed_info_count_channel(database_conn, channel_id, &count);
	if (error) {
		char *msg = Arena_alloc(*arena, sizeof(DISCORD_MAX_MESSAGE_LEN));
		snprintf(msg, DISCORD_MAX_MESSAGE_LEN, "Error creating list: %s", zblock_feed_info_strerror(error));
		data->content = msg;
		return data;
	}
	
	int last_page_number = count ? count % LIST_PAGE_SIZE ? count / LIST_PAGE_SIZE + 1 : count / LIST_PAGE_SIZE : 1;
	
	zblock_feed_info feeds[LIST_PAGE_SIZE];
	int num_retrieved;
	error = zblock_feed_info_retrieve_chunk_channel(database_conn, channel_id, (page_number - 1) * LIST_PAGE_SIZE, LIST_PAGE_SIZE, feeds, &num_retrieved);
	if (error) {
		char *msg = Arena_alloc(*arena, sizeof(DISCORD_MAX_MESSAGE_LEN));
		snprintf(msg, DISCORD_MAX_MESSAGE_LEN, "Error creating list: %s", zblock_feed_info_strerror(error));
		data->content = msg;
		return data;
	}
	
	// create our components starting with the action row
	data->components = Arena_alloc(*arena, sizeof(*data->components));
	data->components->size = 1;
	data->components->array = Arena_allocz(*arena, data->components->size * sizeof(*data->components->array));
	struct discord_component *action_row = data->components->array;
	action_row->type = DISCORD_COMPONENT_ACTION_ROW;
	// create buttons
	action_row->components = Arena_alloc(*arena, sizeof(*action_row->components));
	action_row->components->size = 2;
	action_row->components->array = Arena_allocz(*arena, action_row->components->size * sizeof(*action_row->components->array));
	struct discord_component *buttons = action_row->components->array;
	// create emojis
	struct discord_emoji *back_arrow = Arena_allocz(*arena, sizeof(*back_arrow));
	back_arrow->name = "◀️";
	struct discord_emoji *next_arrow = Arena_allocz(*arena, sizeof(*next_arrow));
	next_arrow->name = "▶️";
	// create button ids
	int back_id_size = snprintf(NULL, 0, "list_page%d", page_number - 1) + 1;
	char *back_id = Arena_alloc(*arena, back_id_size);
	snprintf(back_id, back_id_size, "list_page%d", page_number - 1);
	int next_id_size = snprintf(NULL, 0, "list_page%d", page_number + 1) + 1;
	char *next_id = Arena_alloc(*arena, next_id_size);
	snprintf(next_id, next_id_size, "list_page%d", page_number + 1);
	// populate buttons
	buttons[0] = (struct discord_component) {
		.type = DISCORD_COMPONENT_BUTTON,
		.disabled = page_number == 1,
		.style = DISCORD_BUTTON_SECONDARY,
		.custom_id = back_id,
		.label = "Back",
		.emoji = back_arrow
	};
	buttons[1] = (struct discord_component) {
		.type = DISCORD_COMPONENT_BUTTON,
		.disabled = page_number == last_page_number,
		.style = DISCORD_BUTTON_SECONDARY,
		.custom_id = next_id,
		.label = "Next",
		.emoji = next_arrow
	};
	
	// create embed
	data->embeds = Arena_alloc(*arena, sizeof(*data->embeds));
	data->embeds->size = 1;
	data->embeds->array = Arena_allocz(*arena, data->embeds->size * sizeof(*data->embeds->array));
	struct discord_embed *embed = data->embeds->array;
	int embed_title_size = snprintf(NULL, 0, "Feed List (Page %d of %d)", page_number, last_page_number) + 1;
	char *embed_title = Arena_alloc(*arena, embed_title_size);
	snprintf(embed_title, embed_title_size, "Feed List (Page %d of %d)", page_number, last_page_number);
	
	// write the description
	char *embed_description;
	if (count) {
		embed_description = Arena_alloc(*arena, 4096); // the current max size of embed descriptions
		int embed_description_size = 0;
		for (int i = 0; i < num_retrieved; ++i) {
			// in case somebody has maliciously long text in their feed
			if (embed_description_size < 4096) {
				embed_description_size += snprintf(embed_description + embed_description_size, 4096 - embed_description_size,
					"### %d. %s\n" // feed title
					"Link: %s\n" // feed url
					"Last updated: %s\n", // last_pubDate
					(page_number - 1) * LIST_PAGE_SIZE + i + 1, feeds[i].title,
					feeds[i].url,
					feeds[i].last_pubDate
				);
			}
		}
	} else {
		embed_description = "There are no feeds in this channel.";
	}
	
	*embed = (struct discord_embed) {
		.title = embed_title,
		.type = "rich",
		.description = embed_description
	};
	
	return data;
}

static void bot_command_list(struct discord *client, const struct discord_interaction *event) {
	Arena *arena;
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_CHANNEL_MESSAGE_WITH_SOURCE,
		.data = list_data_create(event->channel_id, 1, &arena)
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
	Arena_delete(arena);
}

static void list_update(struct discord *client, const struct discord_interaction *event) {
	int page_number;
	sscanf(event->data->custom_id, "list_page%d", &page_number);

	Arena *arena;
	struct discord_interaction_response res = {
		.type = DISCORD_INTERACTION_UPDATE_MESSAGE,
		.data = list_data_create(event->channel_id, page_number, &arena)
	};

	discord_create_interaction_response(client, event->id, event->token, &res, NULL);
	Arena_delete(arena);
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
	switch (event->type) {
		case DISCORD_INTERACTION_APPLICATION_COMMAND: {
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
		} break;
		case DISCORD_INTERACTION_MESSAGE_COMPONENT: { // only the list command is used here so far
			list_update(client, event);
		} break;
		default: // nothing
	}

}

static void on_guild_delete(struct discord *client, const struct discord_guild *event) {
	(void) client;
	if (zblock_feed_info_delete_all_guild(database_conn, event->id)) {
		log_error("Unable to delete all feeds from guild %" PRIu64 ". You probably want to clean this up.", event->id);
	}
}

static void on_channel_delete(struct discord *client, const struct discord_channel *event) {
	(void) client;
	if (zblock_feed_info_delete_all_channel(database_conn, event->id)) {
		log_error("Unable to delete all feeds from channel %" PRIu64 ". You probably want to clean this up.", event->id);
	}
}

// delay before the first feed retrieval (in ms)
#define FEED_TIMER_DELAY 15000

// interval for the feed retrieval timer (in ms)
#define FEED_TIMER_INTERVAL 600000

// seconds in a day
#define ONE_DAY_SEC 86400

// milliseconds in a week
#define ONE_WEEK_MS 604800000

int main(void) {
	int exit_code = 0;	
	
	// set locale for time
	setlocale(LC_ALL, "C");
	srand(time(NULL));
	struct discord *client = discord_config_init("config.json");

	zblock_config_err config_err = zblock_config_load(client);
	if (config_err) {
		log_fatal("Error loading zblock config: %s\n", zblock_config_strerror(config_err));
		exit_code = 1;
		goto cleanup;
	}
	
	// connect to database
	database_conn = PQconnectdb(zblock_config.conninfo);
	if (!database_conn) {
		log_fatal("Failed to connect to database.");
		exit_code = 1;
		goto cleanup;
	}
	
	discord_set_on_ready(client, &on_ready);
	discord_set_on_interaction_create(client, &on_interaction);
	discord_set_on_guild_delete(client, &on_guild_delete);
	discord_set_on_channel_delete(client, &on_channel_delete);

	// register timers
	discord_timer_interval(client, timer_retrieve_feeds, NULL, NULL, FEED_TIMER_DELAY, FEED_TIMER_INTERVAL, -1);

	// find the next tueday and start the timer for the tuesday event
	if (zblock_config.tuesday_enable) {
		time_t current_time = time(NULL);
		struct tm midnight_tm;
		gmtime_r(&current_time, &midnight_tm);
		// set it to midnight
		midnight_tm.tm_sec = 0;
		midnight_tm.tm_min = 0;
		midnight_tm.tm_hour = 0;
		time_t midnight_time = timegm(&midnight_tm);
		// find next tuesday and add it to the time
		time_t next_tuesday = midnight_time + ONE_DAY_SEC * (((1 - midnight_tm.tm_wday + 7) % 7) + 1);
		// set a timer that starts midnight next tuesday and triggers every week
		discord_timer_interval(client, timer_tuesday_event, NULL, NULL, (next_tuesday - current_time) * 1000, ONE_WEEK_MS, -1);
	}

	discord_run(client);
	
	PQfinish(database_conn);
	cleanup:
	discord_cleanup(client);
	ccord_global_cleanup();
	
	return exit_code;
}
