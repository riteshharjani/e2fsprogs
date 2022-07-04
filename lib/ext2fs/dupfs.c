/*
 * dupfs.c --- duplicate a ext2 filesystem handle
 *
 * Copyright (C) 1997, 1998, 2001, 2003, 2005 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Library
 * General Public License, version 2.
 * %End-Header%
 */

#include "config.h"
#include <stdio.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_PTHREAD_H
#include <pthread.h>
#endif
#include <time.h>
#include <string.h>
#include <assert.h>

#include "ext2_fs.h"
#include "ext2fsP.h"

errcode_t ext2fs_dup_handle(ext2_filsys src, ext2_filsys *dest)
{
	ext2_filsys	fs;
	errcode_t	retval;

	EXT2_CHECK_MAGIC(src, EXT2_ET_MAGIC_EXT2FS_FILSYS);

	retval = ext2fs_get_mem(sizeof(struct struct_ext2_filsys), &fs);
	if (retval)
		return retval;

	*fs = *src;
	fs->device_name = 0;
	fs->super = 0;
	fs->orig_super = 0;
	fs->group_desc = 0;
	fs->inode_map = 0;
	fs->block_map = 0;
	fs->badblocks = 0;
	fs->dblist = 0;
	fs->mmp_buf = 0;
	fs->mmp_cmp = 0;
	fs->mmp_fd = -1;

	io_channel_bumpcount(fs->io);
	if (fs->icache)
		fs->icache->refcount++;

	retval = ext2fs_get_mem(strlen(src->device_name)+1, &fs->device_name);
	if (retval)
		goto errout;
	strcpy(fs->device_name, src->device_name);

	retval = ext2fs_get_mem(SUPERBLOCK_SIZE, &fs->super);
	if (retval)
		goto errout;
	memcpy(fs->super, src->super, SUPERBLOCK_SIZE);

	retval = ext2fs_get_mem(SUPERBLOCK_SIZE, &fs->orig_super);
	if (retval)
		goto errout;
	memcpy(fs->orig_super, src->orig_super, SUPERBLOCK_SIZE);

	retval = ext2fs_get_array(fs->desc_blocks, fs->blocksize,
				&fs->group_desc);
	if (retval)
		goto errout;
	memcpy(fs->group_desc, src->group_desc,
	       (size_t) fs->desc_blocks * fs->blocksize);

	if (src->inode_map) {
		retval = ext2fs_copy_bitmap(src->inode_map, &fs->inode_map);
		if (retval)
			goto errout;
	}
	if (src->block_map) {
		retval = ext2fs_copy_bitmap(src->block_map, &fs->block_map);
		if (retval)
			goto errout;
	}
	if (src->badblocks) {
		retval = ext2fs_badblocks_copy(src->badblocks, &fs->badblocks);
		if (retval)
			goto errout;
	}
	if (src->dblist) {
		retval = ext2fs_copy_dblist(src->dblist, &fs->dblist);
		if (retval)
			goto errout;
	}
	if (src->mmp_buf) {
		retval = ext2fs_get_mem(src->blocksize, &fs->mmp_buf);
		if (retval)
			goto errout;
		memcpy(fs->mmp_buf, src->mmp_buf, src->blocksize);
	}
	if (src->mmp_fd >= 0) {
		fs->mmp_fd = dup(src->mmp_fd);
		if (fs->mmp_fd < 0) {
			retval = EXT2_ET_MMP_OPEN_DIRECT;
			goto errout;
		}
	}
	if (src->mmp_cmp) {
		int align = ext2fs_get_dio_alignment(src->mmp_fd);

		retval = ext2fs_get_memalign(src->blocksize, align,
					     &fs->mmp_cmp);
		if (retval)
			goto errout;
		memcpy(fs->mmp_cmp, src->mmp_cmp, src->blocksize);
	}
	*dest = fs;
	return 0;
errout:
	ext2fs_free(fs);
	return retval;

}

#ifdef HAVE_PTHREAD
errcode_t ext2fs_clone_fs(ext2_filsys fs, ext2_filsys *dest, unsigned int flags)
{
	errcode_t retval;
	ext2_filsys childfs;

	EXT2_CHECK_MAGIC(fs, EXT2_ET_MAGIC_EXT2FS_FILSYS);

	retval = ext2fs_get_mem(sizeof(struct struct_ext2_filsys), &childfs);
	if (retval)
		return retval;

	/* make an exact copy implying lists and memory structures are shared */
	memcpy(childfs, fs, sizeof(struct struct_ext2_filsys));
	childfs->inode_map = NULL;
	childfs->block_map = NULL;
	childfs->badblocks = NULL;
	childfs->dblist = NULL;

	pthread_mutex_lock(&fs->refcount_mutex);
	fs->refcount++;
	pthread_mutex_unlock(&fs->refcount_mutex);

	if ((flags & EXT2FS_CLONE_INODE) && fs->inode_map) {
		retval = ext2fs_copy_bitmap(fs->inode_map, &childfs->inode_map);
		if (retval)
			return retval;
		childfs->inode_map->fs = childfs;
	}

	if ((flags & EXT2FS_CLONE_BLOCK) && fs->block_map) {
		retval = ext2fs_copy_bitmap(fs->block_map, &childfs->block_map);
		if (retval)
			return retval;
		childfs->block_map->fs = childfs;
	}

	if ((flags & EXT2FS_CLONE_BADBLOCKS) && fs->badblocks) {
		retval = ext2fs_badblocks_copy(fs->badblocks, &childfs->badblocks);
		if (retval)
			return retval;
	}

	if ((flags & EXT2FS_CLONE_DBLIST) && fs->dblist) {
		retval = ext2fs_copy_dblist(fs->dblist, &childfs->dblist);
		if (retval)
			return retval;
		childfs->dblist->fs = childfs;
	}

	/* icache when NULL will be rebuilt if needed */
	childfs->icache = NULL;

	childfs->clone_flags = flags;
	childfs->parent = fs;
	*dest = childfs;

	return 0;
}

errcode_t ext2fs_merge_fs(ext2_filsys *thread_fs)
{
	ext2_filsys fs = *thread_fs;
	errcode_t retval = 0;
	ext2_filsys dest = fs->parent;
	ext2_filsys src = fs;
	unsigned int flags = fs->clone_flags;
	struct ext2_inode_cache *icache;
	io_channel dest_io;
	io_channel dest_image_io;
	ext2fs_inode_bitmap inode_map;
	ext2fs_block_bitmap block_map;
	ext2_badblocks_list badblocks;
	ext2_dblist dblist;
	void *priv_data;
	int fsflags;

	pthread_mutex_lock(&fs->refcount_mutex);
	fs->refcount--;
	assert(fs->refcount >= 0);
	pthread_mutex_unlock(&fs->refcount_mutex);

	icache = dest->icache;
	dest_io = dest->io;
	dest_image_io = dest->image_io;
	inode_map = dest->inode_map;
	block_map = dest->block_map;
	badblocks = dest->badblocks;
	dblist = dest->dblist;
	priv_data = dest->priv_data;
	fsflags = dest->flags;

	memcpy(dest, src, sizeof(struct struct_ext2_filsys));

	dest->io = dest_io;
	dest->image_io = dest_image_io;
	dest->icache = icache;
	dest->inode_map = inode_map;
	dest->block_map = block_map;
	dest->badblocks = badblocks;
	dest->dblist = dblist;
	dest->priv_data = priv_data;
	if (dest->dblist)
		dest->dblist->fs = dest;
	dest->flags = src->flags | fsflags;
	if (!(src->flags & EXT2_FLAG_VALID) || !(dest->flags & EXT2_FLAG_VALID))
		ext2fs_unmark_valid(dest);

	if ((flags & EXT2FS_CLONE_INODE) && src->inode_map) {
		if (dest->inode_map == NULL) {
			dest->inode_map = src->inode_map;
			src->inode_map = NULL;
		} else {
			retval = ext2fs_merge_bitmap(src->inode_map, dest->inode_map, NULL, NULL);
			if (retval)
				goto out;
		}
		dest->inode_map->fs = dest;
	}

	if ((flags & EXT2FS_CLONE_BLOCK) && src->block_map) {
		if (dest->block_map == NULL) {
			dest->block_map = src->block_map;
			src->block_map = NULL;
		} else {
			retval = ext2fs_merge_bitmap(src->block_map, dest->block_map, NULL, NULL);
			if (retval)
				goto out;
		}
		dest->block_map->fs = dest;
	}

	if ((flags & EXT2FS_CLONE_BADBLOCKS) && src->badblocks) {
		if (dest->badblocks == NULL)
			retval = ext2fs_badblocks_copy(src->badblocks, &dest->badblocks);
		else
			retval = ext2fs_badblocks_merge(src->badblocks, dest->badblocks);
		if (retval)
			goto out;
	}

	if ((flags & EXT2FS_CLONE_DBLIST) && src->dblist) {
		if (dest->dblist == NULL) {
			dest->dblist = src->dblist;
			src->dblist = NULL;
		} else {
			retval = ext2fs_merge_dblist(src->dblist, dest->dblist);
			if (retval)
				goto out;
		}
		dest->dblist->fs = dest;
	}

	if (src->icache) {
		ext2fs_free_inode_cache(src->icache);
		src->icache = NULL;
	}

out:
	if (src->io)
		io_channel_close(src->io);

	if ((flags & EXT2FS_CLONE_INODE) && src->inode_map)
		ext2fs_free_generic_bmap(src->inode_map);
	if ((flags & EXT2FS_CLONE_BLOCK) && src->block_map)
		ext2fs_free_generic_bmap(src->block_map);
	if ((flags & EXT2FS_CLONE_BADBLOCKS) && src->badblocks)
		ext2fs_badblocks_list_free(src->badblocks);
	if ((flags & EXT2FS_CLONE_DBLIST) && src->dblist) {
		ext2fs_free_dblist(src->dblist);
		src->dblist = NULL;
	}

	ext2fs_free_mem(&src);
	*thread_fs = NULL;

	return retval;
}
#endif
