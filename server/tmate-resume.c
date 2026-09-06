#include "tmate-resume.h"
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

int tmtv_resume_tokens(const char *secret, char rw[33], char ro[36])
{
	unsigned char key[32], digest[EVP_MAX_MD_SIZE];
	unsigned int len;
	const char *labels[] = {"tmtv:rw:v1", "tmtv:ro:v1"};
	char *outputs[] = {rw, ro + 3};
	int result = -1;

	if (strlen(secret) != 64)
		return -1;
	for (int i = 0; i < 32; i++) {
		int hi = hex_digit(secret[2*i]), lo = hex_digit(secret[2*i+1]);
		if (hi < 0 || lo < 0)
			goto done;
		key[i] = (hi << 4) | lo;
	}
	memcpy(ro, "ro-", 3);
	for (int n = 0; n < 2; n++) {
		if (!HMAC(EVP_sha256(), key, sizeof(key),
		    (const unsigned char *)labels[n], strlen(labels[n]), digest, &len))
			goto done;
		for (int i = 0; i < 16; i++)
			snprintf(outputs[n] + 2*i, 3, "%02x", digest[i]);
	}
	result = 0;
done:
	OPENSSL_cleanse(key, sizeof(key));
	OPENSSL_cleanse(digest, sizeof(digest));
	return result;
}

/* Callers serialize alias operations with flock on the sessions directory. */
int tmtv_alias_points_to(int dirfd, const char *alias, const char *target)
{
	char buf[128];
	ssize_t len;
	if (!alias || !target)
		return 0;
	len = readlinkat(dirfd, alias, buf, sizeof(buf));
	return len >= 0 && (size_t)len == strlen(target) &&
	    memcmp(buf, target, len) == 0;
}

int tmtv_alias_replace(int dirfd, const char *alias, const char *target)
{
	static unsigned counter;
	char temp[64];
	int error;
	for (int tries = 0; tries < 100; tries++) {
		snprintf(temp, sizeof(temp), ".resume-%ld-%u", (long)getpid(), counter++);
		if (symlinkat(target, dirfd, temp) == 0) {
			if (renameat(dirfd, temp, dirfd, alias) == 0)
				return 0;
			error = errno;
			unlinkat(dirfd, temp, 0);
			errno = error;
			return -1;
		}
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

void tmtv_alias_remove_owned(int dirfd, const char *alias, const char *target)
{
	if (tmtv_alias_points_to(dirfd, alias, target))
		unlinkat(dirfd, alias, 0);
}
