/**
 * @file debug_cmd.c  Debug commands
 *
 * Copyright (C) 2010 - 2016 Alfred E. Heggestad
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef USE_OPENSSL
#include <openssl/crypto.h>
#endif
#include <re.h>
#include <rem.h>
#include <baresip.h>


/**
 * @defgroup debug_cmd debug_cmd
 *
 * Advanced debug commands
 */


static uint64_t start_ticks;          /**< Ticks when app started         */
static time_t start_time;             /**< Start time of application      */
static struct play *g_play;


static int cmd_net_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return net_debug(pf, baresip_network());
}


static int print_system_info(struct re_printf *pf, void *arg)
{
	uint32_t uptime;
	int err = 0;

	(void)arg;

	uptime = (uint32_t)((long long)(tmr_jiffies() - start_ticks)/1000);

	err |= re_hprintf(pf, "\n--- System info: ---\n");

	err |= re_hprintf(pf, " Machine:  %s/%s\n", sys_arch_get(),
			  sys_os_get());
	err |= re_hprintf(pf, " Version:  %s (libre v%s)\n",
			  baresip_version(), sys_libre_version_get());
	err |= re_hprintf(pf, " Build:    %H\n", sys_build_get, NULL);
	err |= re_hprintf(pf, " Kernel:   %H\n", sys_kernel_get, NULL);
	err |= re_hprintf(pf, " Uptime:   %H\n", fmt_human_time, &uptime);
	err |= re_hprintf(pf, " Started:  %s", ctime(&start_time));

#ifdef __VERSION__
	err |= re_hprintf(pf, " Compiler: %s\n", __VERSION__);
#endif

#ifdef USE_OPENSSL
	err |= re_hprintf(pf, " OpenSSL:  %s\n",
			  OpenSSL_version(OPENSSL_VERSION));
#endif

	return err;
}


static int cmd_config_print(struct re_printf *pf, void *unused)
{
	(void)unused;
	return config_print(pf, conf_config());
}


static int cmd_ua_debug(struct re_printf *pf, void *unused)
{
	struct le *le;
	int err;
	(void)unused;

	if (list_isempty(uag_list()))
		return re_hprintf(pf, "(no user-agent)\n");

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;

		err = ua_debug(pf, ua);
		if (err)
			return err;
	}

	return 0;
}


/**
 * Returns all the User-Agents and their general codec state.
 * Formatted as JSON, for use with TCP / MQTT API interface.
 * JSON object with 'cuser' as the key.
 *
 * @return All User-Agents available, NULL if none
 */
static int cmd_api_uastate(struct re_printf *pf, void *unused)
{
	struct odict *od = NULL;
	struct le *le;
	int err;
	(void)unused;

	err = odict_alloc(&od, 8);
	if (err)
		return err;

	for (le = list_head(uag_list()); le && !err; le = le->next) {
		const struct ua *ua = le->data;
		struct odict *odua;

		err = odict_alloc(&odua, 8);

		err |= ua_state_json_api(odua, ua);
		err |= odict_entry_add(od, account_aor(ua_account(ua)),
				       ODICT_OBJECT, odua);
		mem_deref(odua);
	}

	err |= json_encode_odict(pf, od);
	if (err)
		warning("debug: failed to encode json (%m)\n", err);

	mem_deref(od);

	return re_hprintf(pf, "\n");
}


static int cmd_play_file(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct config *cfg;
	const char *filename = carg->prm;
	int err = 0;

	cfg = conf_config();

	/* Stop the current tone, if any */
	g_play = mem_deref(g_play);

	if (str_isset(filename))
	{
		err = re_hprintf(pf, "playing audio file \"%s\" ..\n",
				 filename);
		if (err)
			return err;

		err = play_file(&g_play, baresip_player(), filename, 0,
                        cfg->audio.alert_mod, cfg->audio.alert_dev);
		if (err)
		{
			warning("debug_cmd: play_file(%s) failed (%m)\n",
					filename, err);
			return err;
		}
	}

	return err;
}


static void print_fileinfo(struct ausrc_prm *prm)
{
	double s  = ((float) prm->duration) / 1000;

	if (prm->duration) {
		info("debug_cmd: length = %1.3lf seconds\n", s);
		module_event("debug_cmd", "aufileinfo", NULL, NULL,
			 "length = %lf seconds", s);
	}
	else {
		info("debug_cmd: timeout\n");
		module_event("debug_cmd", "aufileinfo", NULL, NULL,
			 "length unknown");
	}
}


/**
 * Command aufileinfo reads given audio file with ausrc that is specified in
 * config file_ausrc, computes the length in milli seconds and sends a ua_event
 * to inform about the result. The file has to be located in the path specified
 * by audio_path.
 *
 * Usage:
 * /aufileinfo audiofile
 *
 * @param pf Print handler is used to return length.
 * @param arg Command argument contains the file name.
 *
 * @return 0 if success, otherwise errorcode
 */
/* ------------------------------------------------------------------------- */
static int cmd_aufileinfo(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	int err = 0;
	char *path;
	char aumod[16];
	const char *file = carg->prm;
	struct ausrc_prm prm;

	if (!str_isset(file)) {
		re_hprintf(pf, "fileplay: filename not specified\n");
		return EINVAL;
	}

	err = conf_get_str(conf_cur(), "file_ausrc", aumod, sizeof(aumod));
	if (err) {
		warning("debug_cmd: file_ausrc is not set\n");
		return EINVAL;
	}

	/* absolute path? */
	if (file[0] == '/' ||
	    !re_regex(file, strlen(file), "https://") ||
	    !re_regex(file, strlen(file), "http://") ||
	    !re_regex(file, strlen(file), "file://")) {
		if (re_sdprintf(&path, "%s", file) < 0)
			return ENOMEM;
	}
	else if (re_sdprintf(&path, "%s/%s",
			conf_config()->audio.audio_path, file) < 0)
		return ENOMEM;

	err = ausrc_info(baresip_ausrcl(), aumod, &prm, path);
	if (err) {
		warning("debug_cmd: %s - ausrc %s does not support info query "
			"or reading source %s failed. (%m)\n",
			__func__, aumod, carg->prm, err);
		goto out;
	}

	print_fileinfo(&prm);
out:

	mem_deref(path);
	return err;
}


static int cmd_sip_debug(struct re_printf *pf, void *unused)
{
	(void)unused;
	return sip_debug(pf, uag_sip());
}


static bool lab_str_eq(const char *a, const char *b)
{
	return 0 == str_casecmp(a, b);
}



static int lab_ncasecmp(const char *a, const char *b, size_t n)
{
	size_t i;

	if (!a || !b)
		return a == b ? 0 : a ? 1 : -1;

	for (i = 0; i < n; ++i) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];

		if (!ca || !cb || tolower((unsigned char)ca) != tolower((unsigned char)cb))
			return (int)tolower((unsigned char)ca) - (int)tolower((unsigned char)cb);
	}

	return 0;
}

static const char *lab_skipws(const char *p)
{
	while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
		++p;
	return p;
}


static int lab_split_word(const char *s, char *word, size_t wsz,
			  const char **restp)
{
	size_t n = 0;

	s = lab_skipws(s);
	if (!str_isset(s))
		return EINVAL;

	while (s[n] && s[n] != ' ' && s[n] != '\t' &&
	       s[n] != '\r' && s[n] != '\n')
		++n;

	if (n + 1 > wsz)
		return ENOMEM;

	(void)re_snprintf(word, wsz, "%.*s", (int)n, s);
	*restp = lab_skipws(s + n);
	return 0;
}

static int cmd_sip_trace(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	const char *prm = carg ? carg->prm : NULL;

	if (!str_isset(prm)) {
		uag_enable_sip_trace(!uag_sip_trace_enabled());
		return uag_sip_trace_debug(pf);
	}

	prm = lab_skipws(prm);

	if (0 == str_casecmp(prm, "on") ||
	    0 == str_casecmp(prm, "yes") ||
	    0 == str_casecmp(prm, "true")) {
		uag_enable_sip_trace(true);
		return uag_sip_trace_debug(pf);
	}

	if (0 == str_casecmp(prm, "off") ||
	    0 == str_casecmp(prm, "no") ||
	    0 == str_casecmp(prm, "false")) {
		uag_enable_sip_trace(false);
		return uag_sip_trace_debug(pf);
	}

	if (0 == str_casecmp(prm, "stdout")) {
		uag_sip_trace_stdout();
		return uag_sip_trace_debug(pf);
	}

	if (0 == str_casecmp(prm, "status"))
		return uag_sip_trace_debug(pf);

	if (0 == lab_ncasecmp(prm, "file ", 5)) {
		const char *file = lab_skipws(prm + 5);
		int err;

		if (!str_isset(file))
			return re_hprintf(pf, "usage: /siptrace file PATH\n");

		err = uag_sip_trace_file_set(file);
		if (err)
			return re_hprintf(pf, "siptrace file failed: %m\n", err);

		return uag_sip_trace_debug(pf);
	}

	if (prm[0] == '/' || prm[0] == '.') {
		int err = uag_sip_trace_file_set(prm);
		if (err)
			return re_hprintf(pf, "siptrace file failed: %m\n", err);

		return uag_sip_trace_debug(pf);
	}

	return re_hprintf(pf,
		"usage: /siptrace [on|off|status|stdout|file PATH]\n");
}

static int cmd_siphdr_common(struct re_printf *pf, const char *prm, bool reg)
{
	const char *name;
	int err;

	if (!str_isset(prm))
		return uag_custom_hdr_debug(pf, reg);

	prm = lab_skipws(prm);

	if (0 == str_casecmp(prm, "list"))
		return uag_custom_hdr_debug(pf, reg);

	if (0 == str_casecmp(prm, "clear")) {
		uag_custom_hdr_clear(reg);
		return re_hprintf(pf, "%s headers cleared\n",
				  reg ? "REGISTER" : "global SIP");
	}

	if (0 == lab_ncasecmp(prm, "add ", 4)) {
		const char *line = lab_skipws(prm + 4);

		if (!str_isset(line))
			return re_hprintf(pf,
				"usage: /%s add Header-Name: value\n",
				reg ? "reghdr" : "siphdr");

		err = uag_custom_hdr_add_line(reg, line);
		if (err)
			return re_hprintf(pf, "could not add header: %m\n", err);

		return re_hprintf(pf, "added %s header\n",
				  reg ? "REGISTER" : "global SIP");
	}

	if (0 == lab_ncasecmp(prm, "del ", 4) ||
	    0 == lab_ncasecmp(prm, "rm ", 3) ||
	    0 == lab_ncasecmp(prm, "remove ", 7)) {
		if (0 == lab_ncasecmp(prm, "del ", 4))
			name = lab_skipws(prm + 4);
		else if (0 == lab_ncasecmp(prm, "rm ", 3))
			name = lab_skipws(prm + 3);
		else
			name = lab_skipws(prm + 7);

		if (!str_isset(name))
			return re_hprintf(pf, "usage: /%s del Header-Name\n",
					  reg ? "reghdr" : "siphdr");

		err = uag_custom_hdr_remove(reg, name);
		if (err)
			return re_hprintf(pf, "header not found: %s\n", name);

		return re_hprintf(pf, "removed %s\n", name);
	}

	if (strchr(prm, ':')) {
		err = uag_custom_hdr_add_line(reg, prm);
		if (err)
			return re_hprintf(pf, "could not add header: %m\n", err);

		return re_hprintf(pf, "added %s header\n",
				  reg ? "REGISTER" : "global SIP");
	}

	return re_hprintf(pf,
		"usage: /%s [list|clear|add Header: value|del Header]\n",
		reg ? "reghdr" : "siphdr");
}

static int cmd_siphdr(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	return cmd_siphdr_common(pf, carg->prm, false);
}


static int cmd_reghdr(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	return cmd_siphdr_common(pf, carg->prm, true);
}


static int cmd_siplog(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	unsigned n = 0;

	if (str_isset(carg->prm))
		n = (unsigned int)strtoul(carg->prm, NULL, 10);

	return uag_sip_log_debug(pf, n);
}


static int cmd_siptransport(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	int err;

	if (!str_isset(carg->prm))
		return re_hprintf(pf, "forced SIP transport: %s\n",
				  uag_force_transport_name());

	err = uag_force_transport_set(carg->prm);
	if (err)
		return re_hprintf(pf, "usage: /siptransport auto|udp|tcp|tls\n");

	return re_hprintf(pf, "forced SIP transport: %s\n",
			  uag_force_transport_name());
}

static int cmd_calldump(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct le *le;
	int err = 0;
	bool all = false;

	if (str_isset(carg->prm) && 0 == str_casecmp(carg->prm, "all"))
		all = true;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;
		struct le *lec;

		for (lec = list_head(ua_calls(ua)); lec; lec = lec->next) {
			struct call *call = lec->data;
			err |= all ? call_debug_full(pf, call)
				   : call_debug_sip(pf, call);
		}
	}

	if (!uag_call_count())
		err |= re_hprintf(pf, "(no active calls)\n");

	return err;
}

static int reload_config(struct re_printf *pf, void *arg)
{
	int err;
	(void)arg;

	err = re_hprintf(pf, "reloading config file ..\n");
	if (err)
		return err;

	err = conf_configure();
	if (err) {
		(void)re_hprintf(pf, "reload_config failed: %m\n", err);
		return err;
	}

	(void)re_hprintf(pf, "done\n");

	return 0;
}


static int cmd_log_level(struct re_printf *pf, void *unused)
{
	int level;
	(void)unused;

	level = log_level_get();

	--level;

	if (level < LEVEL_DEBUG)
		level = LEVEL_ERROR;

	log_level_set(level);

	return re_hprintf(pf, "Log level '%s'\n", log_level_name(level));
}


static int print_uuid(struct re_printf *pf, void *arg)
{
	struct config *cfg = conf_config();
	(void)arg;

	if (cfg)
		re_hprintf(pf, "UUID: %s\n", cfg->sip.uuid);
	return 0;
}


static const struct cmd debugcmdv[] = {
{"apistate",    0,       0, "User Agent state",       cmd_api_uastate     },
{"aufileinfo",  0, CMD_PRM, "Audio file info",        cmd_aufileinfo      },
{"conf_reload", 0,       0, "Reload config file",     reload_config       },
{"config",      0,       0, "Print configuration",    cmd_config_print    },
{"loglevel",   'v',      0, "Log level toggle",       cmd_log_level       },
{"main",        0,       0, "Main loop debug",        re_debug            },
{"memstat",    'y',      0, "Memory status",          mem_status          },
{"modules",     0,       0, "Module debug",           mod_debug           },
{"netstat",    'n',      0, "Network debug",          cmd_net_debug       },
{"play",        0, CMD_PRM, "Play audio file",        cmd_play_file       },
{"sipstat",    'i',      0, "SIP debug",              cmd_sip_debug       },
{"reghdr",      0, CMD_PRM, "REGISTER SIP headers",   cmd_reghdr          },
{"siphdr",      0, CMD_PRM, "Global SIP headers",     cmd_siphdr          },
{"siplog",      0, CMD_PRM, "Recent SIP packets",     cmd_siplog          },
{"siptransport",0, CMD_PRM, "Force SIP transport",    cmd_siptransport    },
{"siptrace",    0, CMD_PRM, "SIP trace",              cmd_sip_trace       },
{"calldump",    0, CMD_PRM, "Call SIP/media dump",    cmd_calldump        },
{"sysinfo",    's',      0, "System info",            print_system_info   },
{"timers",      0,       0, "Timer debug",            tmr_status          },
{"uastat",     'u',      0, "UA debug",               cmd_ua_debug        },
{"uuid",        0,       0, "Print UUID",             print_uuid          },
};


static int module_init(void)
{
	int err;

	start_ticks = tmr_jiffies();
	(void)time(&start_time);

	err = cmd_register(baresip_commands(),
			   debugcmdv, RE_ARRAY_SIZE(debugcmdv));

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), debugcmdv);

	g_play = mem_deref(g_play);
	return 0;
}


const struct mod_export DECL_EXPORTS(debug_cmd) = {
	"debug_cmd",
	"application",
	module_init,
	module_close
};
