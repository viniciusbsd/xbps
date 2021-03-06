/*-
 * Licensed under the SPDX BSD-2-Clause identifier.
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <sys/mman.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include <openssl/sha.h>

#include "xbps_api_impl.h"

/**
 * @file lib/util.c
 * @brief Utility routines
 * @defgroup util Utility functions
 */
static void
digest2string(const uint8_t *digest, char *string, size_t len)
{
	while (len--) {
		if (*digest / 16 < 10)
			*string++ = '0' + *digest / 16;
		else
			*string++ = 'a' + *digest / 16 - 10;
		if (*digest % 16 < 10)
			*string++ = '0' + *digest % 16;
		else
			*string++ = 'a' + *digest % 16 - 10;
		++digest;
	}
	*string = '\0';
}

bool
xbps_mmap_file(const char *file, void **mmf, size_t *mmflen, size_t *filelen)
{
	struct stat st;
	size_t pgsize = (size_t)sysconf(_SC_PAGESIZE);
	size_t pgmask = pgsize - 1, mapsize;
	unsigned char *mf;
	bool need_guard = false;
	int fd;

	assert(file);

	if ((fd = open(file, O_RDONLY|O_CLOEXEC)) == -1)
		return false;

	if (fstat(fd, &st) == -1) {
		(void)close(fd);
		return false;
	}
	if (st.st_size > SSIZE_MAX - 1) {
		(void)close(fd);
		return false;
	}
	mapsize = ((size_t)st.st_size + pgmask) & ~pgmask;
	if (mapsize < (size_t)st.st_size) {
		(void)close(fd);
		return false;
	}
	/*
	 * If the file length is an integral number of pages, then we
	 * need to map a guard page at the end in order to provide the
	 * necessary NUL-termination of the buffer.
	 */
	if ((st.st_size & pgmask) == 0)
		need_guard = true;

	mf = mmap(NULL, need_guard ? mapsize + pgsize : mapsize,
	    PROT_READ, MAP_PRIVATE, fd, 0);
	(void)close(fd);
	if (mf == MAP_FAILED) {
		(void)munmap(mf, mapsize);
		return false;
	}

	*mmf = mf;
	*mmflen = mapsize;
	*filelen = st.st_size;

	return true;
}

bool
xbps_file_sha256_raw(unsigned char *dst, size_t dstlen, const char *file)
{
	int fd;
	ssize_t len;
	char buf[65536];
	SHA256_CTX sha256;

	assert(dstlen >= XBPS_SHA256_DIGEST_SIZE);
	if (dstlen < XBPS_SHA256_DIGEST_SIZE) {
		errno = ENOBUFS;
		return false;
	}

	if ((fd = open(file, O_RDONLY)) < 0)
		return false;

	SHA256_Init(&sha256);

	while ((len = read(fd, buf, sizeof(buf))) > 0)
		SHA256_Update(&sha256, buf, len);

	(void)close(fd);

	if(len == -1)
		return false;

	SHA256_Final(dst, &sha256);

	return true;
}

bool
xbps_file_sha256(char *dst, size_t dstlen, const char *file)
{
	unsigned char digest[XBPS_SHA256_DIGEST_SIZE];

	assert(dstlen >= XBPS_SHA256_SIZE);
	if (dstlen < XBPS_SHA256_SIZE) {
		errno = ENOBUFS;
		return false;
	}

	if (!xbps_file_sha256_raw(digest, sizeof digest, file))
		return false;

	digest2string(digest, dst, XBPS_SHA256_DIGEST_SIZE);

	return true;
}

static bool
sha256_digest_compare(const char *sha256, size_t shalen,
		const unsigned char *digest, size_t digestlen)
{

	assert(shalen == XBPS_SHA256_SIZE - 1);
	if (shalen != XBPS_SHA256_SIZE -1)
		return false;

	assert(digestlen == XBPS_SHA256_DIGEST_SIZE);
	if (digestlen != XBPS_SHA256_DIGEST_SIZE)
		return false;

	for (; *sha256;) {
		if (*digest / 16 < 10) {
			if (*sha256++ != '0' + *digest / 16)
				return false;
		} else {
			if (*sha256++ != 'a' + *digest / 16 - 10)
				return false;
		}
		if (*digest % 16 < 10) {
			if (*sha256++ != '0' + *digest % 16)
				return false;
		} else {
			if (*sha256++ != 'a' + *digest % 16 - 10)
				return false;
		}
		digest++;
	}

	return true;
}

int
xbps_file_sha256_check(const char *file, const char *sha256)
{
	unsigned char digest[XBPS_SHA256_DIGEST_SIZE];

	assert(file != NULL);
	assert(sha256 != NULL);

	if (!xbps_file_sha256_raw(digest, sizeof digest, file))
		return errno;

	if (!sha256_digest_compare(sha256, strlen(sha256), digest, sizeof digest))
		return ERANGE;

	return 0;
}

static const char *
file_hash_dictionary(xbps_dictionary_t d, const char *key, const char *file)
{
	xbps_object_t obj;
	xbps_object_iterator_t iter;
	const char *curfile = NULL, *sha256 = NULL;

	assert(xbps_object_type(d) == XBPS_TYPE_DICTIONARY);
	assert(key != NULL);
	assert(file != NULL);

	iter = xbps_array_iter_from_dict(d, key);
	if (iter == NULL) {
		errno = ENOENT;
		return NULL;
	}
	while ((obj = xbps_object_iterator_next(iter)) != NULL) {
		xbps_dictionary_get_cstring_nocopy(obj,
		    "file", &curfile);
		if (strcmp(file, curfile) == 0) {
			/* file matched */
			xbps_dictionary_get_cstring_nocopy(obj,
			    "sha256", &sha256);
			break;
		}
	}
	xbps_object_iterator_release(iter);
	if (sha256 == NULL)
		errno = ENOENT;

	return sha256;
}

int HIDDEN
xbps_file_hash_check_dictionary(struct xbps_handle *xhp,
				xbps_dictionary_t d,
				const char *key,
				const char *file)
{
	const char *sha256d = NULL;
	char *buf;
	int rv;

	assert(xbps_object_type(d) == XBPS_TYPE_DICTIONARY);
	assert(key != NULL);
	assert(file != NULL);

	if ((sha256d = file_hash_dictionary(d, key, file)) == NULL) {
		if (errno == ENOENT)
			return 1; /* no match, file not found */

		return -1; /* error */
	}

	if (strcmp(xhp->rootdir, "/") == 0) {
		rv = xbps_file_sha256_check(file, sha256d);
	} else {
		buf = xbps_xasprintf("%s/%s", xhp->rootdir, file);
		rv = xbps_file_sha256_check(buf, sha256d);
		free(buf);
	}
	if (rv == 0)
		return 0; /* matched */
	else if (rv == ERANGE || rv == ENOENT)
		return 1; /* no match */
	else
		return -1; /* error */
}
