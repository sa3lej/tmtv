#include <sys/socket.h>
#include <sys/un.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "tmate.h"

#define TMATE_MAX_PASSWORD_LEN 128
#define TMATE_PW_SALT_LEN   16
#define TMATE_PW_KEY_LEN    32
#define TMATE_PW_ITERATIONS  100000
/* Stored format: salt (16 bytes) + derived key (32 bytes) = 48 bytes */
#define TMATE_PASSWORD_HASH_LEN (TMATE_PW_SALT_LEN + TMATE_PW_KEY_LEN)

/*
 * Hash a password with PBKDF2-HMAC-SHA256 and a random salt.
 * Returns a malloc'd buffer of TMATE_PASSWORD_HASH_LEN bytes
 * (salt + derived key), or NULL on failure. Caller must free.
 */
static unsigned char *hash_password(const char *password)
{
	unsigned char *buf = xmalloc(TMATE_PASSWORD_HASH_LEN);

	/* Generate random salt */
	if (RAND_bytes(buf, TMATE_PW_SALT_LEN) != 1) {
		free(buf);
		return NULL;
	}

	/* Derive key */
	if (PKCS5_PBKDF2_HMAC(password, strlen(password),
			       buf, TMATE_PW_SALT_LEN,
			       TMATE_PW_ITERATIONS, EVP_sha256(),
			       TMATE_PW_KEY_LEN,
			       buf + TMATE_PW_SALT_LEN) != 1) {
		free(buf);
		return NULL;
	}

	return buf;
}

/*
 * Derive key from password using stored salt for comparison.
 * Returns a malloc'd buffer of TMATE_PW_KEY_LEN bytes, or NULL.
 */
static unsigned char *derive_key(const char *password,
				 const unsigned char *salt)
{
	unsigned char *key = xmalloc(TMATE_PW_KEY_LEN);

	if (PKCS5_PBKDF2_HMAC(password, strlen(password),
			       salt, TMATE_PW_SALT_LEN,
			       TMATE_PW_ITERATIONS, EVP_sha256(),
			       TMATE_PW_KEY_LEN, key) != 1) {
		free(key);
		return NULL;
	}

	return key;
}

static void reset_and_enable_authorized_keys(void)
{
	ssh_key *keys = tmate_session->authorized_keys;
	if (keys) {
		for (ssh_key *k = keys; *k; k++)
			ssh_key_free(*k);
		free(keys);
	}

	keys = xreallocarray(NULL, sizeof(ssh_key), 1);
	keys[0] = NULL;

	tmate_session->authorized_keys = keys;
}

static ssh_key import_ssh_pubkey64(const char *_keystr)
{
	/* key is formatted as "type base64_key" */

	char * const keystr = xstrdup(_keystr);
	char *s = keystr;
	ssh_key ret = NULL;

	char *key_type = strsep(&s, " ");
	char *key_content = strsep(&s, " ");

	if (!key_content)
		goto out;

	enum ssh_keytypes_e type = ssh_key_type_from_name(key_type);
	if (type == SSH_KEYTYPE_UNKNOWN)
		goto out;

	if (ssh_pki_import_pubkey_base64(key_content, type, &ret) != SSH_OK) {
		ret = NULL;
		goto out;
	}
out:
	free(keystr);
	return ret;
}

int get_num_authorized_keys(ssh_key *keys)
{
	if (!keys)
		return 0;

	int count = 0;
	for (ssh_key *k = keys; *k; k++)
		count++;
	return count;
}

static void append_authorized_key(const char *keystr)
{
	if (!tmate_session->authorized_keys)
		reset_and_enable_authorized_keys();

	ssh_key pkey = import_ssh_pubkey64(keystr);
	if (!pkey)
		return;

	ssh_key *keys = tmate_session->authorized_keys;
	int count = get_num_authorized_keys(keys);
	keys = xreallocarray(keys, sizeof(ssh_key), count+2);
	tmate_session->authorized_keys = keys;

	keys[count++] = pkey;
	keys[count] = NULL;
}

extern void tmate_send_web_url(struct tmate_session *session);
extern void tmate_disconnect_ws_clients(struct tmate_session *session);

extern void tmate_register_session_name(struct tmate_session *session,
					const char *name);

static void tmate_set(char *key, char *value)
{
	if (!strcmp(key, "authorized_keys"))
		append_authorized_key(value);
	else if (!strcmp(key, "session_password")) {
		if (strlen(value) > TMATE_MAX_PASSWORD_LEN) {
			tmate_info("Session password too long (max %d)",
				   TMATE_MAX_PASSWORD_LEN);
			return;
		}
		free(tmate_session->session_password_hash);
		tmate_session->session_password_hash = hash_password(value);
		if (!tmate_session->session_password_hash) {
			tmate_info("Failed to hash session password");
			return;
		}
		tmate_info("Session password set");
	} else if (!strcmp(key, "session_name")) {
		tmate_register_session_name(tmate_session, value);
	} else if (!strcmp(key, "web_sharing")) {
		bool enabled = !strcmp(value, "true") || !strcmp(value, "1") || !strcmp(value, "on");
		tmate_session->web_sharing_enabled = enabled;
		if (enabled) {
			if (tmate_session->urls_sent)
				tmate_send_web_url(tmate_session);
		} else {
			tmate_notify("Web sharing disabled");
			tmate_disconnect_ws_clients(tmate_session);
		}
	} else if (!strcmp(key, "web_input")) {
		bool enabled = !strcmp(value, "true") || !strcmp(value, "1") || !strcmp(value, "on");
		tmate_session->web_input_enabled = enabled;
		tmate_info("Web input toggled: %s", enabled ? "on" : "off");
		if (enabled)
			tmate_notify("Web input enabled — RW web viewers can type");
		else
			tmate_notify("Web input disabled");
	} else if (!strcmp(key, "prefix") || !strcmp(key, "prefix2")) {
		key_code kc = (key_code)strtoull(value, NULL, 0);
		struct session *s = RB_MIN(sessions, &sessions);
		if (s != NULL)
			options_set_number(s->options, key, kc);
		options_set_number(global_s_options, key, kc);
		tmate_info("Session %s set to 0x%llx", key,
			   (unsigned long long)kc);
	} else if (!strcmp(key, "link_ttl")) {
		int ttl = atoi(value);
		tmate_session->link_ttl = ttl > 0 ? ttl : 0;
		if (ttl > 0) {
			tmate_info("Session TTL set: %d seconds", ttl);
			tmate_ensure_idle_timer(tmate_session);
		} else {
			tmate_info("Session TTL cleared");
		}
	}
}

void tmate_hook_set_option_auth(const char *name, const char *val)
{
	if (!strcmp(name, "tmtv-authorized-keys")) {
		reset_and_enable_authorized_keys();
	} else if (!strcmp(name, "tmtv-set")) {
		char *key_value = xstrdup(val);
		char *s = key_value;

		char *key = strsep(&s, "=");
		char *value = s;
		if (value)
			tmate_set(key, value);

		free(key_value);
	}
}

bool tmate_allow_auth(const char *pubkey)
{
	/*
	 * Note that we don't accept connections on the tmux socket until we
	 * get the tmate ready message.
	 */
	if (!tmate_session->authorized_keys)
		return true;

	if (!pubkey)
		return false;

	ssh_key client_pkey = import_ssh_pubkey64(pubkey);
	if (!client_pkey)
		return false;

	bool ret = false;
	for (ssh_key *k = tmate_session->authorized_keys; *k; k++) {
		if (!ssh_key_cmp(client_pkey, *k, SSH_KEY_CMP_PUBLIC)) {
			ret = true;
			break;
		}
	}

	ssh_key_free(client_pkey);

	return ret;
}

bool tmate_check_session_password(const char *password)
{
	unsigned char *candidate_key;
	const unsigned char *stored;

	if (tmate_session->session_password_hash == NULL)
		return true;
	if (password == NULL)
		return false;
	if (strlen(password) > TMATE_MAX_PASSWORD_LEN)
		return false;

	stored = tmate_session->session_password_hash;
	candidate_key = derive_key(password, stored /* salt is first 16 bytes */);
	if (!candidate_key)
		return false;

	/* Constant-time comparison to prevent timing attacks */
	int result = CRYPTO_memcmp(stored + TMATE_PW_SALT_LEN,
				   candidate_key, TMATE_PW_KEY_LEN);
	free(candidate_key);
	return (result == 0);
}

bool tmate_has_session_password(void)
{
	return (tmate_session->session_password_hash != NULL);
}

static int write_all(int fd, const char *buf, size_t len)
{
	for (size_t i = 0; i < len;)  {
		ssize_t ret = write(fd, buf+i, len-i);
		if (ret <= 0)
			return -1;
		i += ret;
	}
	return 0;
}

static int read_all(int fd, char *buf, size_t len)
{
	for (size_t i = 0; i < len;)  {
		ssize_t ret = read(fd, buf+i, len-i);
		if (ret <= 0)
			return -1;
		i += ret;
	}

	return 0;
}

/*
 * The following is executed in the context of the SSH server
 */
bool would_tmate_session_allow_auth(const char *token, const char *pubkey)
{
	/*
	 * The existance of this function is a bit unpleasant:
	 * In order to have the right SSH public key from the SSH client,
	 * we need to ask the tmate session for a match. Denying the key
	 * to the SSH client will make it cycle through its keys.
	 * We briefly connect to the session to get an answer.
	 *
	 * Note that the client will get reauthenticated later (see
	 * server-client.c when identifying the client).
	 */
	int sock_fd = -1;
	int ret = true;

	if (tmate_validate_session_token(token) < 0)
		goto out;

	char *sock_path = get_socket_path(token);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	size_t size = strlcpy(sa.sun_path, sock_path, sizeof(sa.sun_path));
	free(sock_path);
	if (size >= sizeof sa.sun_path)
		goto out;

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0)
		goto out;

	if (connect(sock_fd, (struct sockaddr *)&sa, sizeof sa) == -1)
		goto out;

	struct imsg_hdr hdr = {
		.type = pubkey ? MSG_IDENTIFY_TMATE_AUTH_PUBKEY :
				 MSG_IDENTIFY_TMATE_AUTH_NONE,
		.len = IMSG_HEADER_SIZE + (pubkey ? strlen(pubkey)+1 : 0),
		.peerid = PROTOCOL_VERSION,
		.pid = -1,
	};

	if (write_all(sock_fd, (void*)&hdr, sizeof(hdr)) < 0)
		goto out;

	if (pubkey) {
		if (write_all(sock_fd, pubkey, strlen(pubkey)+1) < 0)
			goto out;
	}

	struct {
		struct imsg_hdr hdr;
		bool allow;
	} __packed recv_msg;

	if (read_all(sock_fd, (void*)&recv_msg, sizeof(recv_msg)) < 0)
		goto out;

	if (recv_msg.hdr.type == MSG_TMATE_AUTH_STATUS &&
	    recv_msg.hdr.len == sizeof(recv_msg))
		ret = recv_msg.allow;

	tmate_debug("(preauth) allow=%d", ret);

out:
	if (sock_fd != -1)
		close(sock_fd);
	return ret;
}

bool would_tmate_session_allow_password(const char *token, const char *password)
{
	/*
	 * IPC to the daemon to check if the provided password matches
	 * the session password. Same pattern as would_tmate_session_allow_auth().
	 */
	int sock_fd = -1;
	int ret = false;
	size_t pw_len;

	if (tmate_validate_session_token(token) < 0)
		goto out;

	char *sock_path = get_socket_path(token);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	size_t size = strlcpy(sa.sun_path, sock_path, sizeof(sa.sun_path));
	free(sock_path);
	if (size >= sizeof sa.sun_path)
		goto out;

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0)
		goto out;

	if (connect(sock_fd, (struct sockaddr *)&sa, sizeof sa) == -1)
		goto out;

	pw_len = password ? strlen(password) + 1 : 0;

	struct imsg_hdr hdr = {
		.type = MSG_IDENTIFY_TMATE_AUTH_PASSWORD,
		.len = IMSG_HEADER_SIZE + pw_len,
		.peerid = PROTOCOL_VERSION,
		.pid = -1,
	};

	if (write_all(sock_fd, (void *)&hdr, sizeof(hdr)) < 0)
		goto out;

	if (password) {
		if (write_all(sock_fd, password, pw_len) < 0)
			goto out;
	}

	struct {
		struct imsg_hdr hdr;
		bool allow;
	} __packed recv_msg;

	if (read_all(sock_fd, (void *)&recv_msg, sizeof(recv_msg)) < 0)
		goto out;

	if (recv_msg.hdr.type == MSG_TMATE_AUTH_STATUS &&
	    recv_msg.hdr.len == sizeof(recv_msg))
		ret = recv_msg.allow;

	tmate_debug("(preauth-password) allow=%d", ret);

out:
	if (sock_fd != -1)
		close(sock_fd);
	return ret;
}

bool tmate_session_requires_password(const char *token)
{
	/*
	 * IPC to the daemon to check if this session has a password set.
	 * Returns true if a password is required.
	 */
	int sock_fd = -1;
	bool ret = false;

	if (tmate_validate_session_token(token) < 0)
		goto out;

	char *sock_path = get_socket_path(token);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	size_t size = strlcpy(sa.sun_path, sock_path, sizeof(sa.sun_path));
	free(sock_path);
	if (size >= sizeof sa.sun_path)
		goto out;

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0)
		goto out;

	if (connect(sock_fd, (struct sockaddr *)&sa, sizeof sa) == -1)
		goto out;

	struct imsg_hdr hdr = {
		.type = MSG_IDENTIFY_TMATE_HAS_PASSWORD,
		.len = IMSG_HEADER_SIZE,
		.peerid = PROTOCOL_VERSION,
		.pid = -1,
	};

	if (write_all(sock_fd, (void *)&hdr, sizeof(hdr)) < 0)
		goto out;

	struct {
		struct imsg_hdr hdr;
		bool allow;
	} __packed recv_msg;

	if (read_all(sock_fd, (void *)&recv_msg, sizeof(recv_msg)) < 0)
		goto out;

	if (recv_msg.hdr.type == MSG_TMATE_AUTH_STATUS &&
	    recv_msg.hdr.len == sizeof(recv_msg))
		ret = recv_msg.allow;

	tmate_debug("(has-password) has_password=%d", ret);

out:
	if (sock_fd != -1)
		close(sock_fd);
	return ret;
}
