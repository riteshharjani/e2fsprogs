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

#ifdef HAVE_PTHREAD
int test_fail = 0;
ext2_filsys testfs;
ext2fs_inode_bitmap	inode_used_map;
ext2fs_block_bitmap block_used_map;
ext2_filsys childfs[2];
pthread_t pthread_infos[2];

#define nr_bits 16384
int nr_threads = 2;

int should_mark_bit()
{
	return rand() % 2 == 0;
}

void setupfs()
{
	errcode_t retval;
	struct ext2_super_block param;

	initialize_ext2_error_table();

	memset(&param, 0, sizeof(param));
	ext2fs_blocks_count_set(&param, nr_bits);
	retval = ext2fs_initialize("test fs", EXT2_FLAG_64BITS, &param,
							   test_io_manager, &testfs);
	if (retval) {
		com_err("setup", retval, "while initializing filesystem");
		exit(1);
	}

	retval = ext2fs_allocate_tables(testfs);
	if (retval) {
		com_err("setup", retval, "while allocating tables for testfs");
		exit(1);
	}
}

void setup_used_bitmaps()
{
	int saved_type = testfs->default_bitmap_type;
	ext2_inode_scan scan;
	struct ext2_inode inode;
	ext2_ino_t ino;
	errcode_t retval;
	int i;

	testfs->default_bitmap_type = EXT2FS_BMAP64_BITARRAY;

	/* allocate block and inode used bitmaps */
	retval = ext2fs_allocate_block_bitmap(testfs, "block used map", &block_used_map);
	if (retval)
		goto out;

	retval = ext2fs_allocate_inode_bitmap(testfs, "inode used map", &inode_used_map);
	if (retval)
		goto out;

	/* setup block and inode used bitmaps */
	for (i = 1; i < nr_bits; i++) {
		/*
		 * we check for testfs->block_map as well since there could be some
		 * blocks already set as part of the FS metadata.
		 */
		if (should_mark_bit() || ext2fs_test_block_bitmap2(testfs->block_map, i)) {
			ext2fs_mark_block_bitmap2(block_used_map, i);
		}
	}

	retval = ext2fs_open_inode_scan(testfs, 8, &scan);
	if (retval) {
		com_err("setup_inode_map", retval, "while open inode scan");
		exit(1);
	}

	retval = ext2fs_get_next_inode(scan, &ino, &inode);
	if (retval) {
		com_err("setup_inode_map", retval, "while getting next inode");
		exit(1);
	}

	while (ino) {
		if (should_mark_bit())
			ext2fs_mark_inode_bitmap2(inode_used_map, ino);

		retval = ext2fs_get_next_inode(scan, &ino, &inode);
		if (retval) {
			com_err("setup_inode_map", retval, "while getting next inode");
			exit(1);
		}
	}
	ext2fs_close_inode_scan(scan);

	testfs->default_bitmap_type = saved_type;
	return;
out:
	com_err("setup_used_bitmaps", retval, "while setting up bitmaps\n");
	exit(1);
}

void setup_childfs()
{
	errcode_t retval;
	int i;

	for (i = 0; i < nr_threads; i++) {
		retval = ext2fs_clone_fs(testfs, &childfs[i], EXT2FS_CLONE_INODE | EXT2FS_CLONE_BLOCK);
		if (retval) {
			com_err("setup_childfs", retval, "while clone testfs for childfs");
			exit(1);
		}

		retval = childfs[i]->io->manager->open(childfs[i]->device_name,
											IO_FLAG_THREADS | IO_FLAG_NOCACHE, &childfs[i]->io);
		if (retval) {
			com_err("setup_pthread", retval, "while opening childfs");
			exit(1);
		}
		assert(childfs[i]->parent == testfs);
	}
}

static errcode_t scan_callback(ext2_filsys fs,
			       ext2_inode_scan scan EXT2FS_ATTR((unused)),
			       dgrp_t group, void *priv_data)
{
	pthread_t id = *((pthread_t *)priv_data);

	printf("%s: Called for group %d via thread %d\n", __func__, group,
			pthread_equal(pthread_infos[1], id));
	if (pthread_equal(pthread_infos[0], id)) {
		if (group >= fs->group_desc_count / 2 - 1)
			return 1;
	}
	return 0;
}

static void *run_pthread(void *arg)
{
	errcode_t retval = 0;
	int i = 0, start, end;
	ext2fs_block_bitmap test_block_bitmap;
	ext2fs_inode_bitmap test_inode_bitmap;
	ext2_inode_scan scan;
	struct ext2_inode inode;
	ext2_ino_t ino;
	pthread_t id = pthread_self();

	if (pthread_equal(pthread_infos[0], id)) {
		start = 1;
		end = nr_bits/2;
		test_block_bitmap = childfs[0]->block_map;
		test_inode_bitmap = childfs[0]->inode_map;

		retval = ext2fs_open_inode_scan(childfs[0], 8, &scan);
		if (retval) {
			com_err("setup_inode_map", retval, "while open inode scan");
			exit(1);
		}

	} else {
		start = nr_bits / 2 + 1;;
		end = nr_bits - 1;
		test_block_bitmap = childfs[1]->block_map;
		test_inode_bitmap = childfs[1]->inode_map;

		retval = ext2fs_open_inode_scan(childfs[1], 8, &scan);
		if (retval) {
			com_err("setup_inode_map", retval, "while open inode scan");
			exit(1);
		}
		ext2fs_inode_scan_goto_blockgroup(scan, testfs->group_desc_count/2);
	}

	ext2fs_set_inode_callback(scan, scan_callback, &id);

	/* blocks scan */
	for (i = start; i <= end; i++) {
		if (ext2fs_test_block_bitmap2(block_used_map, i)) {
			ext2fs_mark_block_bitmap2(test_block_bitmap, i);
		}
	}

	/* inodes scan */
	retval = ext2fs_get_next_inode(scan, &ino, &inode);
	if (retval) {
		com_err("setup_inode_map", retval, "while getting next inode");
		exit(1);
	}

	while (ino) {
		if (ext2fs_test_inode_bitmap2(inode_used_map, ino)) {
			ext2fs_mark_inode_bitmap2(test_inode_bitmap, ino);
		}

		retval = ext2fs_get_next_inode(scan, &ino, &inode);
		if (retval)
			break;
	}
	ext2fs_close_inode_scan(scan);
	return NULL;
}

void run_pthreads()
{
	errcode_t retval;
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

	assert(ext2fs_merge_fs(&childfs[0]) == 0);
	assert(ext2fs_merge_fs(&childfs[1]) == 0);
}

void test_bitmaps()
{
	errcode_t retval;
	retval = ext2fs_compare_block_bitmap(testfs->block_map, block_used_map);
	if (retval) {
		printf("Block bitmap compare -- NOT OK!! (%ld)\n", retval);
		test_fail++;
	}

	printf("Block compare bitmap  -- OK!!\n");
	retval = ext2fs_compare_inode_bitmap(testfs->inode_map, inode_used_map);
	if (retval) {
		printf("Inode bitmap compare -- NOT OK!! (%ld)\n", retval);
		test_fail++;
	}
	printf("Inode compare bitmap  -- OK!!\n");
}

void free_used_bitmaps()
{
	ext2fs_free_block_bitmap(block_used_map);
	ext2fs_free_inode_bitmap(inode_used_map);
}

#endif

int main(int argc, char *argv[])
{
	int i;

#ifndef HAVE_PTHREAD
	printf("No PTHREAD support, exiting...\n");
	return 0;
#else

	srand(time(0));

	setupfs();
	setup_used_bitmaps();

	setup_childfs();
	run_pthreads();
	test_bitmaps(i);

	if (test_fail)
		printf("%s: Test libext2fs clone/merge with pthreads NOT OK!!\n", argv[0]);
	else
		printf("%s: Test libext2fs clone/merge with pthreads OK!!\n", argv[0]);
	free_used_bitmaps();
	ext2fs_free(testfs);

	return test_fail;
#endif
}
