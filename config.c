#include <concord/discord.h>

#include "config.h"

struct zblock_config zblock_config;

int zblock_config_load(struct discord *client) {
	// TODO: actually load config
	zblock_config.conninfo = "feeds";
	return 0;
}