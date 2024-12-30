#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <concord/discord.h>

#include "config.h"

struct zblock_config zblock_config;

static const char *ZBLOCK_CONFIG_ERRORS[] = {
	"OK",
	"Error loading conninfo from config",
	"No channel id was provided for the tuesday event"
};
static_assert(sizeof(ZBLOCK_CONFIG_ERRORS) / sizeof(*ZBLOCK_CONFIG_ERRORS) == ZBLOCK_CONFIG_ERRORCOUNT, "Not all config errors implemented");

zblock_config_err zblock_config_load(struct discord *client) {
	struct ccord_szbuf_readonly conninfo = discord_config_get_field(client, (char *[2]){"zblock", "conninfo"}, 2);
	if (asprintf(&zblock_config.conninfo, "%.*s", (int)conninfo.size, conninfo.start) < 0) {
		return ZBLOCK_CONFIG_CONNINFO_ERROR;
	}

	struct ccord_szbuf_readonly tuesday_enable = discord_config_get_field(client, (char *[3]){"zblock", "tuesday", "enable"}, 3);
	if (!strncmp(tuesday_enable.start, "true", tuesday_enable.size)) {
		zblock_config.tuesday_enable = true;
		struct ccord_szbuf_readonly tuesday_channel = discord_config_get_field(client, (char *[3]){"zblock", "tuesday", "channel"}, 3);
		zblock_config.tuesday_channel = 0; // initialize it with something
		if (tuesday_channel.size > 0) zblock_config.tuesday_channel = strtoull(tuesday_channel.start, NULL, 10);
		if (!zblock_config.tuesday_channel) return ZBLOCK_CONFIG_NO_TUESDAY_CHANNEL;
	}
	
	return ZBLOCK_CONFIG_OK;
}

// returns a string about the result of a config function
const char *zblock_config_strerror(zblock_config_err error) {
	return error < 0 || error >= ZBLOCK_CONFIG_ERRORCOUNT ? "Unspecified error" : ZBLOCK_CONFIG_ERRORS[error];
}
