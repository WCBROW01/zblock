#define _GNU_SOURCE
#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <concord/discord.h>
#include <concord/log.h>

#include <mrss.h>

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

struct feed_info {
	char *title;
	char *url;
	char *last_pubDate;
	u64snowflake guild_id;
	u64snowflake channel_id;
	unsigned timer_id;
	unsigned feed_id;
};

void feed_info_free(struct feed_info *feed) {
	free(feed->title);
	free(feed->url);
	free(feed->last_pubDate);
	free(feed);
}

static const char *database_path = "feeds";

// format string for for the time format of pubDate
#define PUBDATE_FMT "%a, %d %b %Y %T %z"

// maybe change the function signature so you can actually do error handling with the result?
static time_t pubDate_to_time_t(char *s) {
	struct tm tm;
	char *res = strptime(s, PUBDATE_FMT, &tm);
	if (!res || !*res) return 0; // invalid time
	
	return mktime(&tm);
}

// default interval for the feed retrieval timer
#define TIMER_INTERVAL 600

// this just barely works at the moment
static void timer_retrieve_feeds(struct discord *client, struct discord_timer *timer) {
	struct feed_info *feed = timer->data;
	
	mrss_t *mrss_feed;
	if (mrss_parse_url(feed->url, &mrss_feed)) return; // do nothing on error
	
	// get publication date of entries send any new ones
	time_t last_pubDate_time = pubDate_to_time_t(feed->last_pubDate);
	mrss_item_t *item = mrss_feed->item;
	bool update_pubDate = false;
	while (item && pubDate_to_time_t(item->pubDate) > last_pubDate_time) {
		update_pubDate = true;
		
		// Send new entry in the feed
		char msg[DISCORD_MAX_MESSAGE_LEN];
		snprintf(msg, sizeof(msg), "## %s\n### %s\n%s", mrss_feed->title, mrss_feed->item->title, mrss_feed->item->link);
		struct discord_create_message res = { .content = msg };
		discord_create_message(client, feed->channel_id, &res, NULL);
		item = item->next;
	}
	
	if (update_pubDate) {
		free(feed->last_pubDate);
		feed->last_pubDate = strdup(mrss_feed->item->pubDate);
	}
	
	mrss_free(mrss_feed);
}

static void bot_command_add(struct discord *client, const struct discord_interaction *event) {
	char msg[DISCORD_MAX_MESSAGE_LEN];
	struct feed_info *feed = calloc(1, sizeof(struct feed_info));
	if (!feed) {
		snprintf(msg, sizeof(msg), "Error adding feed: %s", strerror(errno));
		goto send_msg;
	}
	
	feed->url = strdup(event->data->options->array[0].value);
	if (!feed->url) {
		snprintf(msg, sizeof(msg), "Error adding feed: %s", strerror(errno));
		feed_info_free(feed);
		goto send_msg;
	}
	
	mrss_t *mrss_feed = NULL;
	if(mrss_parse_url(feed->url, &mrss_feed)) {
		// error here figure this out
		feed_info_free(feed);
		goto send_msg;
	}
	
	feed->title = mrss_feed->title;
	feed->last_pubDate = mrss_feed->item->pubDate;
	feed->feed_id = rand();
	
	
	char file_path[PATH_MAX];
	// maybe check if we ran out of characters for path?
	snprintf(file_path, sizeof(file_path), "%s/%lu/%lu/%x", database_path, event->guild_id, event->channel_id, feed->feed_id);
	
	FILE *fp = fopen(file_path, "w");
	if (!fp) {
		snprintf(msg, sizeof(msg), "Error adding feed: %s", strerror(errno));
		mrss_free(mrss_feed);
		feed_info_free(feed);
		goto send_msg;
	}
	fprintf(fp, "title=%s\nurl=%s\nlast_pubDate=%s\n", feed->title, feed->url, feed->last_pubDate);
	fclose(fp);
	
	// spawn the timer for this feed
	feed->timer_id = discord_timer_interval(client, timer_retrieve_feeds, NULL, feed, 0, TIMER_INTERVAL, -1);
	
	mrss_free(mrss_feed);
	
	// send the confirmation message
	snprintf(msg, sizeof(msg), "The following feed has been successfully added to this channel:\n`%s`", feed->url);
	
	send_msg:
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
	
	// create feed retrieval timers
	

	log_info("Ready!");
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

int main(void) {
	srand(time(NULL));
	struct discord *client = discord_config_init("config.json");
	discord_set_on_ready(client, &on_ready);
	discord_set_on_interaction_create(client, &on_interaction);
	discord_run(client);
	discord_cleanup(client);
	ccord_global_cleanup();
}
