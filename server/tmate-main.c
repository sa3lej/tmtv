#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libssh/libssh.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#ifdef HAVE_CURSES_H
#include <curses.h>
#else
#include <ncurses.h>
#endif
#include <term.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include "tmate.h"

static char *cmdline;
static char *cmdline_end;

struct tmate_settings _tmate_settings = {
	.keys_dir        	= TMATE_SSH_DEFAULT_KEYS_DIR,
	.ssh_port        	= TMATE_SSH_DEFAULT_PORT,
	.ssh_port_advertized    = -1,
	.websocket_hostname  	= NULL,
	.bind_addr	 	= NULL,
	.websocket_port      	= 0,
	.web_port            	= 0,
	.tmate_host      	= NULL,
	.log_level      	= 0,
	.use_proxy_protocol	= false,
	.authorized_keys_only	= false,
	.idle_timeout		= 0,
	.max_lifetime		= 0,
};

struct tmate_settings *tmate_settings = &_tmate_settings;

void request_server_termination(void)
{
	/*
	 * tmux 3.6a: server_fd and server_send_exit() are static.
	 * Use SIGTERM to trigger clean shutdown.
	 */
	kill(getpid(), SIGTERM);
}

static void usage(void)
{
	fprintf(stderr, "usage: tmtv-server [-A] [-b ip] [-h hostname] [-k keys_dir] [-L max_lifetime] [-p listen_port] [-q ssh_port_advertized] [-T idle_timeout] [-V] [-w web_port] [-z sse_port] [-x] [-v]\n");
}

static char* get_full_hostname(void)
{
	struct addrinfo hints, *info;
	char hostname[1024];
	int gai_result;
	char *ret;

	if (gethostname(hostname, sizeof(hostname)) < 0)
		tmate_fatal("cannot get hostname");
	hostname[1023] = '\0';

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; /*either IPV4 or IPV6*/
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	if ((gai_result = getaddrinfo(hostname, NULL, &hints, &info)) != 0) {
		tmate_info("cannot lookup hostname: %s", gai_strerror(gai_result));
		return xstrdup(hostname);
	}

	ret = xstrdup(info->ai_canonname);

	freeaddrinfo(info);
	return ret;
}
#include <langinfo.h>
#include <locale.h>

static void setup_locale(void)
{
	const char *s;

	if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL) {
		if (setlocale(LC_CTYPE, "") == NULL)
			tmate_fatal("invalid LC_ALL, LC_CTYPE or LANG");
		s = nl_langinfo(CODESET);
		if (strcasecmp(s, "UTF-8") != 0 &&
		    strcasecmp(s, "UTF8") != 0)
			tmate_fatal("need UTF-8 locale (LC_CTYPE) but have %s", s);
	}

	setlocale(LC_TIME, "");
	tzset();
}

static int check_owned_directory_mode(const char *path, mode_t expected_mode)
{
	struct stat stat;
	if (lstat(path, &stat))
		return -1;

	if (!S_ISDIR(stat.st_mode))
		return -1;

	if (stat.st_uid != getuid())
		return -1;

	if ((stat.st_mode & 07777) != expected_mode)
		return -1;

	return 0;
}

int main(int argc, char **argv, char **envp)
{
	int opt;

	while ((opt = getopt(argc, argv, "Ab:h:k:L:p:q:T:Vw:z:xv")) != -1) {
		switch (opt) {
		case 'V':
			printf("tmtv-server %s (based on tmux %s)\n",
			       TMTV_VERSION, TMUX_VERSION);
			return 0;
		case 'A':
			tmate_settings->authorized_keys_only = true;
			break;
		case 'b':
			tmate_settings->bind_addr = xstrdup(optarg);
			break;
		case 'h':
			tmate_settings->tmate_host = xstrdup(optarg);
			break;
		case 'k':
			tmate_settings->keys_dir = xstrdup(optarg);
			break;
		case 'L':
			tmate_settings->max_lifetime = atoi(optarg);
			break;
		case 'p':
			tmate_settings->ssh_port = atoi(optarg);
			break;
		case 'q':
			tmate_settings->ssh_port_advertized = atoi(optarg);
			break;
		case 'T':
			tmate_settings->idle_timeout = atoi(optarg);
			break;
		case 'w':
			tmate_settings->web_port = atoi(optarg);
			break;
		case 'z':
			tmate_settings->websocket_port = atoi(optarg);
			break;
		case 'x':
			tmate_settings->use_proxy_protocol = true;
			break;
		case 'v':
			tmate_settings->log_level++;
			break;
		default:
			usage();
			return 1;
		}
	}

	/*
	 * tmux 3.6a: no init_logging(). Use log_open() for file-based
	 * logging, or just rely on log_debug() which goes to the tmux log.
	 */
	if (tmate_settings->log_level > 0)
		log_open("tmtv-server");

	setup_locale();

	if (!tmate_settings->tmate_host)
		tmate_settings->tmate_host = get_full_hostname();

	cmdline = *argv;
	cmdline_end = *envp;

	tmate_preload_trace_lib();
	tmate_catch_sigsegv();
	tmate_init_rand();

	/*
	 * Initialize tmux global options (normally done in tmux.c main(),
	 * which is excluded from the server build).
	 */
	{
		const struct options_table_entry *oe;
		global_environ = environ_create();
		for (char **var = envp; *var != NULL; var++)
			environ_put(global_environ, *var, 0);
		global_options = options_create(NULL);
		global_s_options = options_create(NULL);
		global_w_options = options_create(NULL);
		for (oe = options_table; oe->name != NULL; oe++) {
			if (oe->scope & OPTIONS_TABLE_SERVER)
				options_default(global_options, oe);
			if (oe->scope & OPTIONS_TABLE_SESSION)
				options_default(global_s_options, oe);
			if (oe->scope & OPTIONS_TABLE_WINDOW)
				options_default(global_w_options, oe);
		}
	}

	if ((mkdir(TMATE_WORKDIR, 0700)             < 0 && errno != EEXIST) ||
	    (mkdir(TMATE_WORKDIR "/sessions", 0700) < 0 && errno != EEXIST) ||
	    (mkdir(TMATE_WORKDIR "/jail", 0700)     < 0 && errno != EEXIST))
		tmate_fatal("Cannot prepare session in " TMATE_WORKDIR);

	/*
	 * Sessions dir: 0730 = owner rwx, group wx, other nothing.
	 * Jailed children run as TMATE_JAIL_USER and need group-write
	 * to create symlinks via sessions_dir_fd.  Other local users
	 * get no access (fixes CVE-2021-44512).
	 */
	{
		struct passwd *jail_pw = getpwnam(TMATE_JAIL_USER);
		if (!jail_pw)
			tmate_fatal("Cannot find user %s", TMATE_JAIL_USER);
		if (chown(TMATE_WORKDIR "/sessions", 0, jail_pw->pw_gid) < 0)
			tmate_fatal("Cannot chown sessions dir");
	}

	if ((chmod(TMATE_WORKDIR, 0711)             < 0) ||
	    (chmod(TMATE_WORKDIR "/sessions", 0730) < 0) ||
	    (chmod(TMATE_WORKDIR "/jail", 0700)     < 0))
		tmate_fatal("Cannot prepare session in " TMATE_WORKDIR);

	if (check_owned_directory_mode(TMATE_WORKDIR, 0711) ||
	    check_owned_directory_mode(TMATE_WORKDIR "/sessions", 0730) ||
	    check_owned_directory_mode(TMATE_WORKDIR "/jail", 0700))
		tmate_fatal(TMATE_WORKDIR " and subdirectories has incorrect ownership/mode. "
			    "Try deleting " TMATE_WORKDIR " and try again");

	/*
	 * No startup wipe: session entries from a previous run are left
	 * in place so that reconnecting clients can reclaim their tokens.
	 * The periodic GC in the main loop (gc_stale_sessions(), every 60s)
	 * will clean up any truly orphaned entries once it's clear no
	 * client is coming back.
	 */

	tmate_info("tmtv-server %s starting: ssh_port=%d sse_port=%d "
		   "bind=%s proxy_protocol=%s authorized_keys_only=%s "
		   "idle_timeout=%ds max_lifetime=%ds",
		   TMTV_VERSION,
		   tmate_settings->ssh_port,
		   tmate_settings->websocket_port,
		   tmate_settings->bind_addr ? tmate_settings->bind_addr : "*",
		   tmate_settings->use_proxy_protocol ? "on" : "off",
		   tmate_settings->authorized_keys_only ? "on" : "off",
		   tmate_settings->idle_timeout,
		   tmate_settings->max_lifetime);

	tmate_ssh_server_main(tmate_session,
			      tmate_settings->keys_dir, tmate_settings->bind_addr, tmate_settings->ssh_port);
	return 0;
}

char *get_socket_path(const char *_token)
{
	char *path;
	char *token = xstrdup(_token);

	for (char *c = token; *c; c++) {
		if (*c == '/' || *c == '.')
			*c = '=';
	}

	xasprintf(&path, TMATE_WORKDIR "/sessions/%s", token);
	free(token);
	return path;
}

void set_session_token(struct tmate_session *session, const char *token)
{
	session->session_token = xstrdup(token);
	socket_path = get_socket_path(token);

	xasprintf((char **)&session->obfuscated_session_token, "%.4s...",
		  session->session_token);

	size_t size = cmdline_end - cmdline;
	memset(cmdline, 0, size);
	snprintf(cmdline, size-1, "tmtv-server [%s] %s %s",
		tmate_session->obfuscated_session_token,
		session->ssh_client.role == TMATE_ROLE_DAEMON ? "(daemon)" : "(pty client)",
		session->ssh_client.ip_address);

	/*
	 * tmux 3.6a: no set_log_prefix(). Log prefix not supported.
	 * Session token is visible in the process title instead.
	 */
}

void close_fds_except(int *fd_to_preserve, int num_fds)
{
	int fd, i, preserve;
	int max_fd = (int)sysconf(_SC_OPEN_MAX);

	if (max_fd < 0 || max_fd > 65536)
		max_fd = 1024;

	for (fd = 0; fd < max_fd; fd++) {
		preserve = 0;
		for (i = 0; i < num_fds; i++)
			if (fd_to_preserve[i] == fd)
				preserve = 1;

		if (!preserve)
			close(fd);
	}
}

void get_in_jail(void)
{
	struct passwd *pw;
	uid_t uid;
	gid_t gid;

	pw = getpwnam(TMATE_JAIL_USER);
	if (!pw) {
		tmate_fatal("Cannot get the /etc/passwd entry for %s",
			    TMATE_JAIL_USER);
	}
	uid = pw->pw_uid;
	gid = pw->pw_gid;

	if (getuid() != 0)
		tmate_fatal("Need root privileges to create the jail");

	if (chroot(TMATE_WORKDIR "/jail") < 0)
		tmate_fatal("Cannot chroot()");

	if (chdir("/") < 0)
		tmate_fatal("Cannot chdir()");

#ifdef IS_LINUX
	if (unshare(CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET) < 0)
		tmate_fatal("Cannot create new namespace");
#endif

	if (setgroups(1, (gid_t[]){gid}) < 0)
		tmate_fatal("Cannot setgroups()");

#if defined(HAVE_SETRESGID)
	if (setresgid(gid, gid, gid) < 0)
		tmate_fatal("Cannot setresgid() %d", gid);
#elif defined(HAVE_SETREGID)
	if (setregid(gid, gid) < 0)
		tmate_fatal("Cannot setregid()");
#else
	if (setgid(gid) < 0)
		tmate_fatal("Cannot setgid()");
#endif

#if defined(HAVE_SETRESUID)
	if (setresuid(uid, uid, uid) < 0)
		tmate_fatal("Cannot setresuid()");
#elif defined(HAVE_SETREUID)
	if (setreuid(uid, uid) < 0)
		tmate_fatal("Cannot setreuid()");
#else
	if (setuid(uid) < 0)
		tmate_fatal("Cannot setuid()");
#endif

	if (nice(1) == -1 && errno != 0)
		tmate_debug("nice(1) failed: %s", strerror(errno));

	tmate_debug("Dropped privileges to %s (%d,%d), jailed in %s",
		    TMATE_JAIL_USER, uid, gid, TMATE_WORKDIR "/jail");
}

void setup_ncurse(int fd, const char *name)
{
	int error;
	if (setupterm((char *)name, fd, &error) != OK)
		tmate_fatal("Cannot setup terminal");
}
