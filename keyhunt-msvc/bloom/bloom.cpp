/*
 *  Copyright (c) 2012-2019, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

 /*
  * Refer to bloom.h for documentation on the public interfaces.
  */

#include <assert.h>
#include <fcntl.h>
#include <cmath>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <sys/types.h>

#include "bloom.h"
#include "../xxhash/xxhash.h"

#define MAKESTRING(n) STRING(n)
#define STRING(n) #n
#define BLOOM_MAGIC "libbloom2"
#define BLOOM_VERSION_MAJOR 2
#define BLOOM_VERSION_MINOR 1

inline static int test_bit_set_bit(unsigned char* buf, uint64_t bit, int set_bit)
{
	uint64_t byte = bit >> 3;
	uint8_t c = buf[byte];        // expensive memory access
	uint8_t mask = 1 << (bit % 8);
	if (c & mask) {
		return 1;
	}
	else {
		if (set_bit) {
			buf[byte] = c | mask;
		}
		return 0;
	}
}


static int bloom_check_add(struct bloom* bloom, const void* buffer, int len, int add)
{
	if (bloom->ready == 0) {
		printf("bloom at %p not initialized!\n", (void*)bloom);
		return -1;
	}
	uint8_t hits = 0;
	uint64_t a = XXH64(buffer, len, 0x59f2815b16f81798);
	uint64_t b = XXH64(buffer, len, a);
	uint64_t x;
	uint8_t i;
	for (i = 0; i < bloom->hashes; i++) {
		x = (a + b * i) % bloom->bits;
		if (test_bit_set_bit(bloom->bf, x, add)) {
			hits++;
		}
		else if (!add) {
			// Don't care about the presence of all the bits. Just our own.
			return 0;
		}
	}
	if (hits == bloom->hashes) {
		return 1;                // 1 == element already in (or collision)
	}
	return 0;
}


// DEPRECATED - Please migrate to bloom_init2.
int bloom_init(struct bloom* bloom, uint64_t entries, long double error)
{
	return bloom_init2(bloom, entries, error);
}


int bloom_init2(struct bloom* bloom, uint64_t entries, long double error)
{
	memset(bloom, 0, sizeof(struct bloom));
	if (entries < 1000 || error <= 0 || error >= 1) {
		return 1;
	}

	bloom->entries = entries;
	bloom->error = error;

	long double num = -log(bloom->error);
	long double denom = 0.480453013918201; // ln(2)^2
	bloom->bpe = (num / denom);

	long double dentries = (long double)entries;
	long double allbits = dentries * bloom->bpe;
	bloom->bits = (uint64_t)allbits;

	bloom->bytes = (uint64_t)bloom->bits / 8;
	if (bloom->bits % 8) {
		bloom->bytes += 1;
	}

	bloom->hashes = (uint8_t)ceil(0.693147180559945 * bloom->bpe);  // ln(2)

	bloom->bf = (uint8_t*)calloc(bloom->bytes, sizeof(uint8_t));
	if (bloom->bf == NULL) {                                   // LCOV_EXCL_START
		return 1;
	}                                                          // LCOV_EXCL_STOP

	bloom->ready = 1;
	bloom->major = BLOOM_VERSION_MAJOR;
	bloom->minor = BLOOM_VERSION_MINOR;
	return 0;
}

int bloom_dummy(struct bloom* bloom, uint64_t entries, long double error)
{
	memset(bloom, 0, sizeof(struct bloom));
	if (entries < 1000 || error <= 0 || error >= 1) {
		return 1;
	}

	bloom->entries = entries;
	bloom->error = error;

	long double num = -log(bloom->error);
	long double denom = 0.480453013918201; // ln(2)^2
	bloom->bpe = (num / denom);

	long double dentries = (long double)entries;
	long double allbits = dentries * bloom->bpe;
	bloom->bits = (uint64_t)allbits;

	bloom->bytes = (uint64_t)bloom->bits / 8;
	if (bloom->bits % 8) {
		bloom->bytes += 1;
	}

	bloom->hashes = (uint8_t)ceil(0.693147180559945 * bloom->bpe);  // ln(2)
	/*
	bloom->bf = (uint8_t *)calloc(bloom->bytes, sizeof(uint8_t));
	if (bloom->bf == NULL) {                                   // LCOV_EXCL_START
	  return 1;
	}                                                          // LCOV_EXCL_STOP

	bloom->ready = 1;
	bloom->major = BLOOM_VERSION_MAJOR;
	bloom->minor = BLOOM_VERSION_MINOR;
	*/
	return 0;
}

int bloom_check(struct bloom* bloom, const void* buffer, int len)
{
	return bloom_check_add(bloom, buffer, len, 0);
}


int bloom_add(struct bloom* bloom, const void* buffer, int len)
{
	int r;

#ifdef WIN64
	WaitForSingleObject(&bloom->mutex, INFINITE);
#else
	pthread_mutex_lock(&bloom->mutex);
#endif
	r = bloom_check_add(bloom, buffer, len, 1);
#ifdef WIN64
	ReleaseMutex(&bloom->mutex);
#else
	pthread_mutex_unlock(&bloom->mutex);
#endif
	return r;
}


void bloom_print(struct bloom* bloom)
{
	printf("bloom at %p\n", (void*)bloom);
	if (!bloom->ready) { printf(" *** NOT READY ***\n"); }
	printf(" ->version = %d.%d\n", bloom->major, bloom->minor);
#ifdef WIN64
	printf(" ->entries = %llu\n", bloom->entries);
#else
	printf(" ->entries = %"PRIu64"\n", bloom->entries);
#endif
	printf(" ->error = %Lf\n", bloom->error);
#ifdef WIN64
	printf(" ->bits = %llu\n", bloom->bits);
#else
	printf(" ->bits = %"PRIu64"\n", bloom->bits);
#endif
	printf(" ->bits per elem = %f\n", bloom->bpe);
#ifdef WIN64
	printf(" ->bytes = %llu\n", bloom->bytes);
#else
	printf(" ->bytes = %"PRIu64"\n", bloom->bytes);
#endif
	unsigned int KB = bloom->bytes / 1024;
	unsigned int MB = KB / 1024;
	printf(" (%u KB, %u MB)\n", KB, MB);
	printf(" ->hash functions = %d\n", bloom->hashes);
}


void bloom_free(struct bloom* bloom)
{
	if (bloom->ready) {
		free(bloom->bf);
	}
	bloom->ready = 0;
}


int bloom_reset(struct bloom* bloom)
{
	if (!bloom->ready) return 1;
	memset(bloom->bf, 0, bloom->bytes);
	return 0;
}

int bloom_loadcustom(struct bloom* bloom, char* filename) {
	if (filename == NULL || filename[0] == 0) {
		return 1;
	}
	FILE* fd_str, * fd_dat;
	char filename_str[70], filename_dat[70];
	memset(filename_str, 0, 70);
	memset(filename_dat, 0, 70);
	snprintf(filename_str, 68, "%s.blm", filename);
	snprintf(filename_dat, 68, "%s.dat", filename);
	fd_str = fopen(filename_str, "rb");
	fd_dat = fopen(filename_dat, "rb");
	if (fd_str == NULL || fd_dat == NULL) {
		return 1;
	}
	if (fread(bloom, 1, sizeof(struct bloom), fd_str) != sizeof(struct bloom)) {
		fclose(fd_str);
		fclose(fd_dat);
		return 1;
	}
	bloom->bf = (uint8_t*)malloc(bloom->bytes);
	if (bloom->bf == NULL) {
		memset(bloom, 0, sizeof(struct bloom));
		return 1;
	}
	if (fread(bloom->bf, 1, bloom->bytes, fd_dat) != bloom->bytes) {
		free(bloom->bf);
		memset(bloom, 0, sizeof(struct bloom));
		fclose(fd_str);
		fclose(fd_dat);
		return 1;
	}
	fclose(fd_str);
	fclose(fd_dat);
	return 0;
}

int bloom_savecustom(struct bloom* bloom, char* filename) {
	if (filename == NULL || filename[0] == 0) {
		return 1;
	}
	FILE* fd_str, * fd_dat;
	char filename_str[70], filename_dat[70];
	memset(filename_str, 0, 70);
	memset(filename_dat, 0, 70);
	snprintf(filename_str, 68, "%s.blm", filename);
	snprintf(filename_dat, 68, "%s.dat", filename);
	fd_str = fopen(filename_str, "wb");
	fd_dat = fopen(filename_dat, "wb");
	if (fd_str == NULL || fd_dat == NULL) {
		return 1;
	}

	if (fwrite(bloom, 1, sizeof(struct bloom), fd_str) != sizeof(struct bloom)) {
		fclose(fd_str);
		fclose(fd_dat);
		fprintf(stderr, "fwrite bloom\n");
		exit(0);
		return 1;
	}
	if (fwrite(bloom->bf, 1, bloom->bytes, fd_dat) != bloom->bytes) {
		fclose(fd_str);
		fclose(fd_dat);
		fprintf(stderr, "fwrite bloom->bf\n");
		exit(0);
		return 1;
	}
	fclose(fd_str);
	fclose(fd_dat);
	return 0;
}


int bloom_save(struct bloom* bloom, char* filename)
{
	if (filename == NULL || filename[0] == 0) {
		return 1;
	}

#ifdef WIN64
	FILE* fd = fopen(filename, "wb");
	if (fd == NULL) {
		return 1;
	}

	uint16_t size = sizeof(struct bloom);
	size_t out = fwrite(BLOOM_MAGIC, sizeof(char), sizeof(BLOOM_MAGIC), fd);
	if (out != strlen(BLOOM_MAGIC)) { goto save_error; }        // LCOV_EXCL_LINE

	out = fwrite(&size, sizeof(char), sizeof(uint16_t), fd);
	if (out != sizeof(uint16_t)) { goto save_error; }           // LCOV_EXCL_LINE

	out = fwrite(bloom, sizeof(char), sizeof(struct bloom), fd);
	if (out != sizeof(struct bloom)) { goto save_error; }       // LCOV_EXCL_LINE

	out = fwrite(bloom->bf, sizeof(char), bloom->bytes, fd);
	if (out != bloom->bytes) { goto save_error; }               // LCOV_EXCL_LINE

	fclose(fd);
#else
	int fd = open(filename, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		return 1;
	}

	ssize_t out = write(fd, BLOOM_MAGIC, strlen(BLOOM_MAGIC));
	if (out != strlen(BLOOM_MAGIC)) { goto save_error; }        // LCOV_EXCL_LINE

	uint16_t size = sizeof(struct bloom);
	out = write(fd, &size, sizeof(uint16_t));
	if (out != sizeof(uint16_t)) { goto save_error; }           // LCOV_EXCL_LINE

	out = write(fd, bloom, sizeof(struct bloom));
	if (out != sizeof(struct bloom)) { goto save_error; }       // LCOV_EXCL_LINE

	out = write(fd, bloom->bf, bloom->bytes);
	if (out != bloom->bytes) { goto save_error; }               // LCOV_EXCL_LINE

	close(fd);

#endif

	return 0;
	// LCOV_EXCL_START
save_error:
#ifdef WIN64
	fclose(fd);
#else
	close(fd);
#endif
	return 1;
	// LCOV_EXCL_STOP
}


int bloom_load(struct bloom* bloom, char* filename)
{
	int rv = 0;

	if (filename == NULL || filename[0] == 0) { return 1; }
	if (bloom == NULL) { return 2; }

	memset(bloom, 0, sizeof(struct bloom));

#ifdef WIN64

	FILE* fd = fopen(filename, "rb");
	if (fd == NULL) { return 3; }

	char line[30];
	memset(line, 0, 30);
	size_t in = fread(line, sizeof(char), strlen(BLOOM_MAGIC), fd);

	if (in != strlen(BLOOM_MAGIC)) {
		rv = 4;
		goto load_error;
	}

	if (strncmp(line, BLOOM_MAGIC, strlen(BLOOM_MAGIC))) {
		rv = 5;
		goto load_error;
	}

	uint16_t size;
	in = fread(&size, sizeof(char), sizeof(uint16_t), fd);
	if (in != sizeof(uint16_t)) {
		rv = 6;
		goto load_error;
	}

	if (size != sizeof(struct bloom)) {
		rv = 7;
		goto load_error;
	}

	in = fread(bloom, sizeof(char), sizeof(struct bloom), fd);
	if (in != sizeof(struct bloom)) {
		rv = 8;
		goto load_error;
	}

	bloom->bf = NULL;
	if (bloom->major != BLOOM_VERSION_MAJOR) {
		rv = 9;
		goto load_error;
	}

	bloom->bf = (unsigned char*)malloc(bloom->bytes);
	if (bloom->bf == NULL) { rv = 10; goto load_error; }        // LCOV_EXCL_LINE

	in = fread(bloom->bf, sizeof(char), bloom->bytes, fd);
	if (in != bloom->bytes) {
		rv = 11;
		free(bloom->bf);
		bloom->bf = NULL;
		goto load_error;
	}

	fclose(fd);

#else

	int fd = open(filename, O_RDONLY);
	if (fd < 0) { return 3; }

	char line[30];
	memset(line, 0, 30);
	ssize_t in = read(fd, line, strlen(BLOOM_MAGIC));

	if (in != strlen(BLOOM_MAGIC)) {
		rv = 4;
		goto load_error;
	}

	if (strncmp(line, BLOOM_MAGIC, strlen(BLOOM_MAGIC))) {
		rv = 5;
		goto load_error;
	}

	uint16_t size;
	in = read(fd, &size, sizeof(uint16_t));
	if (in != sizeof(uint16_t)) {
		rv = 6;
		goto load_error;
	}

	if (size != sizeof(struct bloom)) {
		rv = 7;
		goto load_error;
	}

	in = read(fd, bloom, sizeof(struct bloom));
	if (in != sizeof(struct bloom)) {
		rv = 8;
		goto load_error;
	}

	bloom->bf = NULL;
	if (bloom->major != BLOOM_VERSION_MAJOR) {
		rv = 9;
		goto load_error;
	}

	bloom->bf = (unsigned char*)malloc(bloom->bytes);
	if (bloom->bf == NULL) { rv = 10; goto load_error; }        // LCOV_EXCL_LINE

	in = read(fd, bloom->bf, bloom->bytes);
	if (in != bloom->bytes) {
		rv = 11;
		free(bloom->bf);
		bloom->bf = NULL;
		goto load_error;
	}

	close(fd);

#endif

	return rv;

load_error:
#ifdef WIN64
	fclose(fd);
#else
	close(fd);
#endif
	bloom->ready = 0;
	return rv;
}


const char* bloom_version()
{
	return MAKESTRING(BLOOM_VERSION);
}
