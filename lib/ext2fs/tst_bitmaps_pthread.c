#include "config.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#if HAVE_ERRNO_H
#include <errno.h>
#endif
#if HAVE_PTHREAD
#include <pthread.h>
#endif

#include "ext2_fs.h"
#include "ext2fs.h"

/*
 * In this test we first setup used_bitmap by setting some random bits.
 * This used_bitmap is then scanned in parallel by two threads, each scanning
 * upto nr_bits/2 and setting their respective child_bitmap.
 * Then once both threads finishes, we merge the child_bitmap_1/2 into
 * parent_bitmap which then is used to compare against used_bitmap.
 * In the end used_bitmap bits should match with parent_bitmap.
 *
 * Note we use EXT2FS_BMAP64_BITARRAY always for used_bitmap, this is because
 * EXT2FS_BMAP64_RBTREE does not support parallel scan due to rcursor
 * optimization.
 */

int test_fail = 0;
ext2fs_generic_bitmap child_bitmap1, child_bitmap2, parent_bitmap;
ext2fs_generic_bitmap used_bitmap;
pthread_t pthread_infos[2];

#define nr_bits 8192
int nr_threads = 2;
int bitmap_type[1] = {EXT2FS_BMAP64_RBTREE};

void dump_bitmap(ext2fs_generic_bitmap bmap, unsigned int start, unsigned num)
{
	unsigned char	*buf;
	errcode_t	retval;
	int	i, len = (num - start + 7) / 8;

	buf = malloc(len);
	if (!buf) {
		com_err("dump_bitmap", 0, "couldn't allocate buffer");
		return;
	}
	memset(buf, 0, len);
	retval = ext2fs_get_generic_bmap_range(bmap, (__u64) start, num, buf);
	if (retval) {
		com_err("dump_bitmap", retval,
			"while calling ext2fs_generic_bmap_range");
		free(buf);
		return;
	}
	for (i=len-1; i >= 0; i--)
		printf("%02x ", buf[i]);
	printf("\n");
	printf("bits set: %u\n", ext2fs_bitcount(buf, len));
	free(buf);
}

int should_mark_bit()
{
	return rand() % 2 == 0;
}

void alloc_bitmaps(int type)
{
	errcode_t retval;

	retval = ext2fs_alloc_generic_bmap(NULL, EXT2_ET_MAGIC_GENERIC_BITMAP64,
							  type, 0, nr_bits, nr_bits,
							  "child bitmap1", &child_bitmap1);
	if (retval)
		goto out;

	retval = ext2fs_alloc_generic_bmap(NULL, EXT2_ET_MAGIC_GENERIC_BITMAP64,
							  type, 0, nr_bits, nr_bits,
							  "child bitmap2", &child_bitmap2);
	if (retval)
		goto out;

	retval = ext2fs_alloc_generic_bmap(NULL, EXT2_ET_MAGIC_GENERIC_BITMAP64,
							  type, 0, nr_bits, nr_bits,
							  "parent bitmap", &parent_bitmap);
	if (retval)
		goto out;

	/*
	 * Note that EXT2FS_BMAP64_RBTREE doesn't support parallel read.
	 * this is due to a optimization of maintaining a read cursor within
	 * rbtree bitmap implementation.
	 */
	retval = ext2fs_alloc_generic_bmap(NULL, EXT2_ET_MAGIC_GENERIC_BITMAP64,
							  EXT2FS_BMAP64_BITARRAY, 0, nr_bits, nr_bits,
							  "used bitmap", &used_bitmap);
	if (retval)
		goto out;

	return;
out:
	com_err("alloc_bitmaps", retval, "while allocating bitmaps\n");
	exit(1);
}

void setup_bitmaps()
{
	int i = 0;
	errcode_t retval;

	/*
	 * Note we cannot setup used_bitmap in parallel w/o locking.
	 * Hence setting up the used_bitmap (random bits) here before
	 * starting pthreads.
	 */
	for (i = 0; i < nr_bits; i++) {
		if (should_mark_bit())
			ext2fs_mark_generic_bmap(used_bitmap, i);
	}
}

static void *run_pthread(void *arg)
{
	int i = 0, j = 0, start, end;
	ext2fs_generic_bitmap test_bitmap;
	errcode_t retval = 0;
	pthread_t id = pthread_self();

	if (pthread_equal(pthread_infos[0], id)) {
		start = 0;
		end = nr_bits/2;
		test_bitmap = child_bitmap1;
	} else {
		start = nr_bits / 2 + 1;;
		end = nr_bits - 1;
		test_bitmap = child_bitmap2;
	}

	for (i = start; i <= end; i++) {
		if (ext2fs_test_generic_bmap(used_bitmap, i)) {
			retval = ext2fs_mark_generic_bmap(test_bitmap, i);
			if (retval) {
				com_err("run_pthread", retval, "while marking child bitmaps %d\n", i);
				test_fail++;
				pthread_exit(&retval);
			}
		}
	}
	return NULL;
}

void run_pthreads()
{
	errcode_t retval;
	void *retp[2];
	int i;

	for (i = 0; i < nr_threads; i++) {
		printf("Starting thread (%d)\n", i);
		retval = pthread_create(&pthread_infos[i], NULL, &run_pthread, NULL);
		if (retval) {
			com_err("run_pthreads", retval, "while pthread_create");
			exit(1);
		}
	}

	for (i = 0; i < nr_threads; i++) {
		void *status;
		int ret;
		retval = pthread_join(pthread_infos[i], &status);
		if (retval) {
			com_err("run_pthreads", retval, "while joining pthreads");
			exit(1);

		}
		ret = status == NULL ? 0 : *(int*)status;
		if (ret) {
			com_err("run_pthreads", ret, "pthread returned error");
			test_fail++;
		}

		printf("Closing thread (%d), ret(%d)\n", i, ret);
	}

	assert(ext2fs_merge_bitmap(child_bitmap1, parent_bitmap, NULL, NULL) == 0);
	assert(ext2fs_merge_bitmap(child_bitmap2, parent_bitmap, NULL, NULL) == 0);
}

void test_bitmaps(int type)
{
	errcode_t retval;
	retval = ext2fs_compare_generic_bmap(EXT2_ET_NEQ_BLOCK_BITMAP, parent_bitmap,
				used_bitmap);
	if (retval) {
		test_fail++;
		printf("Bitmaps compare failed for bitmap type %d err %ld\n", type, retval);
		dump_bitmap(parent_bitmap, 0, nr_bits);
		dump_bitmap(used_bitmap, 0, nr_bits);
	}
}

void free_bitmaps()
{
	ext2fs_free_generic_bmap(child_bitmap1);
	ext2fs_free_generic_bmap(child_bitmap2);
	ext2fs_free_generic_bmap(parent_bitmap);
	ext2fs_free_generic_bmap(used_bitmap);
}

int main(int argc, char *argv[])
{
	int i;
	int ret = 0;

#ifndef HAVE_PTHREAD
	printf("No PTHREAD support, exiting...\n");
	return ret;
#endif

	srand(time(0));

	/* loop to test for bitmap types */
	for (i = 0; i < 1; i++) {
		test_fail = 0;
		alloc_bitmaps(bitmap_type[i]);
		setup_bitmaps();
		run_pthreads();
		test_bitmaps(bitmap_type[i]);
		free_bitmaps();

		if (test_fail)
			printf("%s: Test with bitmap (%d) NOT OK!!\n", argv[0], bitmap_type[i]);
		else
			printf("%s: Test with bitmap (%d) OK!!\n", argv[0], bitmap_type[i]);
		ret |= test_fail;
	}

	return ret;
}
