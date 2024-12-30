#ifndef ZBLOCK_CONFIG_H
#define ZBLOCK_CONFIG_H

#include <concord/discord.h>

// the current zblock config
extern struct zblock_config {
	char *conninfo;
	u64snowflake tuesday_channel;
	bool tuesday_enable;
} zblock_config;

typedef enum {
	ZBLOCK_CONFIG_OK,
	ZBLOCK_CONFIG_CONNINFO_ERROR,
	ZBLOCK_CONFIG_NO_TUESDAY_CHANNEL,
	ZBLOCK_CONFIG_ERRORCOUNT
} zblock_config_err;

// load config entries for zblock
zblock_config_err zblock_config_load(struct discord *client);

// returns a string about the result of a config function
const char *zblock_config_strerror(zblock_config_err error);

#endif
