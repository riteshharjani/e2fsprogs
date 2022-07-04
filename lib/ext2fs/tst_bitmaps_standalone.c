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

#include "ext2_fs.h"
#include "ext2fs.h"
#include "bmap64.h"

ext2_filsys test_fs;
ext2fs_block_bitmap block_map_1;
ext2fs_block_bitmap block_map_2;
ext2fs_block_bitmap block_map;

static int test_fail = 0;

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

static void test_copy_run()
{
	int blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 21, 23, 26, 29, 33, 37, 38};
	errcode_t ret;
	char *buf_map = NULL;
	char *buf_copy_map = NULL;

	assert(ext2fs_allocate_block_bitmap(test_fs, "block bitmap", &block_map_1) == 0);

	for (int i = 0; i < sizeof(blocks)/sizeof(blocks[0]); i++) {
		ext2fs_mark_block_bitmap2(block_map_1, blocks[i]);
	}

	assert(ext2fs_copy_bitmap(block_map_1, &block_map) == 0);

	if (ext2fs_compare_block_bitmap(block_map_1, block_map) != 0) {
		printf("block bitmap copy test failed\n");
		test_fail++;

		dump_bitmap(block_map_1, test_fs->super->s_first_data_block,
				test_fs->super->s_blocks_count);

		dump_bitmap(block_map, test_fs->super->s_first_data_block,
				test_fs->super->s_blocks_count);
	}

	ext2fs_free_block_bitmap(block_map_1);
	ext2fs_free_block_bitmap(block_map);
}

void test_merge_run()
{
	int blocks_odd[] = {1, 3, 5, 7, 9, 21, 23, 29, 33, 37};
	int blocks_even[] = {2, 4, 6, 8, 10, 26, 38};
	ext2fs_generic_bitmap_64 tmp_map;

	assert(ext2fs_allocate_block_bitmap(test_fs, "block bitmap 1", &block_map_1) == 0);
	assert(ext2fs_allocate_block_bitmap(test_fs, "block bitmap 2", &block_map_2) == 0);
	assert(ext2fs_allocate_block_bitmap(test_fs, "block bitmap 2", &block_map) == 0);

	for (int i = 0; i < sizeof(blocks_odd) / sizeof(blocks_odd[0]); i++) {
		ext2fs_mark_block_bitmap2(block_map_1, blocks_odd[i]);
		ext2fs_mark_block_bitmap2(block_map, blocks_odd[i]);
	}

	for (int i = 0; i < sizeof(blocks_even) / sizeof(blocks_even[0]); i++) {
		ext2fs_mark_block_bitmap2(block_map_2, blocks_even[i]);
		ext2fs_mark_block_bitmap2(block_map, blocks_even[i]);
	}

	assert(ext2fs_merge_bitmap(block_map_2, block_map_1, NULL, NULL) == 0);
	if (ext2fs_compare_block_bitmap(block_map_1, block_map) != 0) {
		printf("block bitmap merge test failed\n");
		test_fail++;

		dump_bitmap(block_map_1, test_fs->super->s_first_data_block,
				test_fs->super->s_blocks_count);

		dump_bitmap(block_map, test_fs->super->s_first_data_block,
				test_fs->super->s_blocks_count);
	}

	ext2fs_free_block_bitmap(block_map_1);
	ext2fs_free_block_bitmap(block_map_2);
	ext2fs_free_block_bitmap(block_map);
}

static void setup_filesystem(const char *name, unsigned int blocks,
							 unsigned int inodes, unsigned int type,
							 unsigned int flags)
{
	struct ext2_super_block param;
	errcode_t ret;

	memset(&param, 0, sizeof(param));
	ext2fs_blocks_count_set(&param, blocks);
	param.s_inodes_count = inodes;

	ret = ext2fs_initialize(name, flags, &param, test_io_manager,
							&test_fs);
	if (ret) {
		com_err(name, ret, "while initializing filesystem");
		return;
	}

	test_fs->default_bitmap_type = type;

	ext2fs_free_block_bitmap(test_fs->block_map);
	ext2fs_free_block_bitmap(test_fs->inode_map);

	return;
errout:
	ext2fs_close_free(&test_fs);
}

int main(int argc, char **argv)
{
	unsigned int blocks = 127;
	unsigned int inodes = 0;
	unsigned int type = EXT2FS_BMAP64_RBTREE;
	unsigned int flags = EXT2_FLAG_64BITS;
	char *buf = NULL;

	setup_filesystem(argv[0], blocks, inodes, type, flags);

	/* test for EXT2FS_BMAP64_RBTREE */
	test_copy_run();
	test_merge_run();

	/* TODO: test for EXT2FS_BMAP64_BITARRAY */

	if (test_fail)
		printf("%s: Test copy & merge bitmaps -- NOT OK\n", argv[0]);
	else
		printf("%s: Test copy & merge bitmaps -- OK\n", argv[0]);

	return test_fail;
}
