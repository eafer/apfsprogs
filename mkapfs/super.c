/*
 *  apfsprogs/mkapfs/super.c
 *
 * Copyright (C) 2019 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <apfs/raw.h>
#include "btree.h"
#include "mkapfs.h"
#include "object.h"
#include "super.h"

/**
 * set_uuid - Set a UUID field
 * @field:	on-disk field to set
 * @uuid:	pointer to the UUID string in standard format
 */
static void set_uuid(char *field, char *uuid)
{
	int ret;
	char *stdformat = "%2hhx%2hhx%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-"
			  "%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx";

	ret = sscanf(uuid, stdformat, &field[0], &field[1], &field[2],
				      &field[3], &field[4], &field[5],
				      &field[6], &field[7], &field[8],
				      &field[9], &field[10], &field[11],
				      &field[12], &field[13], &field[14],
				      &field[15]);
	if (ret == 16)
		return;

	printf("Please provide a UUID in standard format.\n");
	exit(1);
}

/**
 * set_checkpoint_areas - Set all sb fields describing the checkpoint areas
 * @sb: pointer to the superblock copy on disk
 */
static void set_checkpoint_areas(struct apfs_nx_superblock *sb)
{
	/* First set the checkpoint descriptor area fields */
	sb->nx_xp_desc_base = cpu_to_le64(CPOINT_DESC_BASE);
	sb->nx_xp_desc_blocks = cpu_to_le32(CPOINT_DESC_BLOCKS);
	/* The first two blocks hold the superblock and the mappings */
	sb->nx_xp_desc_len = cpu_to_le32(2);
	sb->nx_xp_desc_next = cpu_to_le32(2);
	sb->nx_xp_desc_index = 0;

	/* Now set the checkpoint data area fields */
	sb->nx_xp_data_base = cpu_to_le64(CPOINT_DATA_BASE);
	sb->nx_xp_data_blocks = cpu_to_le32(CPOINT_DATA_BLOCKS);
	/* Room for the space manager, the two free queues, and the reaper */
	sb->nx_xp_data_len = cpu_to_le32(4);
	sb->nx_xp_data_next = cpu_to_le32(4);
	sb->nx_xp_data_index = 0;
}

/**
 * get_max_volumes - Calculate the maximum number of volumes for the container
 * @size: the container size, in bytes
 */
static u32 get_max_volumes(u64 size)
{
	u32 max_vols;

	/* Divide by 512 MiB and round up, as the reference requires */
	max_vols = DIV_ROUND_UP(size, 512 * 1024 * 1024);
	if (max_vols > APFS_NX_MAX_FILE_SYSTEMS)
		max_vols = APFS_NX_MAX_FILE_SYSTEMS;
	return max_vols;
}

/**
 * set_ephemeral_info - Set the container's array of ephemeral info
 * @info: pointer to the nx_ephemeral_info array on the container superblock
 */
static void set_ephemeral_info(__le64 *info)
{
	/* TODO: add support for small containers */
	u64 min_block_count = APFS_NX_EPH_MIN_BLOCK_COUNT;

	/* Only the first entry is documented, leave the others as zero */
	*info = cpu_to_le64((min_block_count << 32)
			    | (APFS_NX_MAX_FILE_SYSTEM_EPH_STRUCTS << 16)
			    | APFS_NX_EPH_INFO_VERSION_1);
}

/**
 * make_volume - Make a volume
 * @bno: block number for the volume superblock
 * @oid: object id for the volume superblock
 */
static void make_volume(u64 bno, u64 oid)
{
	struct apfs_superblock *vsb = get_zeroed_block(bno);

	vsb->apfs_magic = cpu_to_le32(APFS_MAGIC);

	vsb->apfs_features = cpu_to_le64(APFS_FEATURE_HARDLINK_MAP_RECORDS);
	if (param->case_sensitive)
		vsb->apfs_incompatible_features =
			cpu_to_le64(APFS_INCOMPAT_NORMALIZATION_INSENSITIVE);
	else
		vsb->apfs_incompatible_features =
			cpu_to_le64(APFS_INCOMPAT_CASE_INSENSITIVE);

	/* Just two catalog records: the root and private directories */
	vsb->apfs_next_obj_id = cpu_to_le64(APFS_MIN_USER_INO_NUM);
	vsb->apfs_num_directories = cpu_to_le64(2);

	set_uuid(vsb->apfs_vol_uuid, param->vol_uuid);

	set_object_header(&vsb->apfs_o, oid,
			  APFS_OBJ_VIRTUAL | APFS_OBJECT_TYPE_FS,
			  APFS_OBJECT_TYPE_INVALID);
	munmap(vsb, param->blocksize);
}

/**
 * make_cpoint_map_block - Make the mapping block for the one checkpoint
 * @bno: block number to use
 */
static void make_cpoint_map_block(u64 bno)
{
	struct apfs_checkpoint_map_phys *block = get_zeroed_block(bno);
	struct apfs_checkpoint_mapping *map;

	block->cpm_flags = cpu_to_le32(APFS_CHECKPOINT_MAP_LAST);
	block->cpm_count = cpu_to_le32(1); /* For the moment, just the reaper */

	/* Set the checkpoint mapping for the reaper */
	map = &block->cpm_map[0];
	map->cpm_type = cpu_to_le32(APFS_OBJ_EPHEMERAL |
				    APFS_OBJECT_TYPE_NX_REAPER);
	map->cpm_subtype = cpu_to_le32(APFS_OBJECT_TYPE_INVALID);
	map->cpm_size = cpu_to_le32(param->blocksize);
	map->cpm_oid = cpu_to_le64(REAPER_OID);
	map->cpm_paddr = cpu_to_le64(REAPER_BNO);

	set_object_header(&block->cpm_o, bno,
			  APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CHECKPOINT_MAP,
			  APFS_OBJECT_TYPE_INVALID);
	munmap(block, param->blocksize);
}

/**
 * make_cpoint_superblock - Make the one checkpoint superblock
 * @bno:	block number to use
 * @sb_copy:	copy of the superblock at block zero
 *
 * For now just copies @sb_copy into @bno.  TODO: figure out what to do with
 * the nx_counters array.
 */
static void make_cpoint_superblock(u64 bno, struct apfs_nx_superblock *sb_copy)
{
	struct apfs_nx_superblock *sb = get_zeroed_block(bno);

	memcpy(sb, sb_copy, sizeof(*sb));
	munmap(sb, param->blocksize);
}

/**
 * make_empty_reaper - Make an empty reaper
 * @bno: block number to use
 * @oid: object id
 */
static void make_empty_reaper(u64 bno, u64 oid)
{
	struct apfs_nx_reaper_phys *reaper = get_zeroed_block(bno);

	reaper->nr_next_reap_id = cpu_to_le64(1);
	reaper->nr_flags = cpu_to_le32(APFS_NR_BHM_FLAG);
	reaper->nr_state_buffer_size = cpu_to_le32(param->blocksize -
						   sizeof(*reaper));

	set_object_header(&reaper->nr_o, oid,
			  APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_NX_REAPER,
			  APFS_OBJECT_TYPE_INVALID);
	munmap(reaper, param->blocksize);
}

/**
 * make_container - Make the whole filesystem
 */
void make_container(void)
{
	struct apfs_nx_superblock *sb_copy;
	u64 size = param->blocksize * param->block_count;

	sb_copy = get_zeroed_block(APFS_NX_BLOCK_NUM);

	sb_copy->nx_magic = cpu_to_le32(APFS_NX_MAGIC);
	sb_copy->nx_block_size = cpu_to_le32(param->blocksize);
	sb_copy->nx_block_count = cpu_to_le64(param->block_count);

	/* We only support version 2 of APFS */
	sb_copy->nx_incompatible_features |=
					cpu_to_le64(APFS_NX_INCOMPAT_VERSION2);

	set_uuid(sb_copy->nx_uuid, param->main_uuid);

	/* Leave some room for the objects created by the mkfs */
	sb_copy->nx_next_oid = cpu_to_le64(APFS_OID_RESERVED_COUNT + 100);
	sb_copy->nx_next_xid = cpu_to_le64(MKFS_XID + 1);

	set_checkpoint_areas(sb_copy);

	sb_copy->nx_spaceman_oid = cpu_to_le64(SPACEMAN_OID);
	sb_copy->nx_reaper_oid = cpu_to_le64(REAPER_OID);
	make_empty_reaper(REAPER_BNO, REAPER_OID);
	sb_copy->nx_omap_oid = cpu_to_le64(MAIN_OMAP_BNO);
	make_omap_btree(MAIN_OMAP_BNO, false /* is_vol */);

	sb_copy->nx_max_file_systems = cpu_to_le32(get_max_volumes(size));
	sb_copy->nx_fs_oid[0] = cpu_to_le64(FIRST_VOL_OID);
	make_volume(FIRST_VOL_BNO, FIRST_VOL_OID);

	set_ephemeral_info(&sb_copy->nx_ephemeral_info[0]);

	set_object_header(&sb_copy->nx_o, APFS_OID_NX_SUPERBLOCK,
			  APFS_OBJ_EPHEMERAL | APFS_OBJECT_TYPE_NX_SUPERBLOCK,
			  APFS_OBJECT_TYPE_INVALID);

	make_cpoint_map_block(CPOINT_MAP_BNO);
	make_cpoint_superblock(CPOINT_SB_BNO, sb_copy);

	munmap(sb_copy, param->blocksize);
}
