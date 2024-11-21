#ifndef ZBLOCK_CONFIG_H
#define ZBLOCK_CONFIG_H

#include <concord/discord.h>

// the current zblock config
extern struct zblock_config {
	char *database_path;
} zblock_config;

int zblock_config_load(struct discord *client);

#endif
