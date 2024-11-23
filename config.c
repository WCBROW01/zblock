#include <stdio.h>
#include <concord/discord.h>

#include "config.h"

struct zblock_config zblock_config;

int zblock_config_load(struct discord *client) {
	struct ccord_szbuf_readonly conninfo = discord_config_get_field(client, (char *[2]){"zblock", "conninfo"}, 2);
	if (asprintf(&zblock_config.conninfo, "%.*s", (int)conninfo.size, conninfo.start) < 0) {
		return 1;
	}
	
	return 0;
}
