/*
** ntfs
** The Sleuth Kit 
**
** Content and meta data layer support for the NTFS file system
**
** Brian Carrier [carrier <at> sleuthkit [dot] org]
** Copyright (c) 2006-2008 Brian Carrier, Basis Technology.  All Rights reserved
** Copyright (c) 2003-2005 Brian Carrier.  All rights reserved
**
** TASK
** Copyright (c) 2002 Brian Carrier, @stake Inc.  All rights reserved
**
** This software is distributed under the Common Public License 1.0
**
** Unicode added with support from I.D.E.A.L. Technology Corp (Aug '05)
**
*/
#include "tsk_fs_i.h"
#include "tsk_ntfs.h"

#include <ctype.h>

/**
 * \file ntfs.c
 * Contains the TSK internal general NTFS processing code
 */
/*
 * NOTES TO SELF:
 *
 * - multiple ".." entries may exist
 */

/* 
 * How are we to handle the META flag? Is the MFT $Data Attribute META?
 */


/* needs to be predefined for proc_attrseq */
static uint8_t ntfs_proc_attrlist(NTFS_INFO *, TSK_FS_FILE *,
    const TSK_FS_ATTR *);



/* mini-design note:
 * The MFT has entries for every file and dir in the fs.
 * The first entry ($MFT) is for the MFT itself and it is used to find
 * the location of the entire table because it can become fragmented.
 * Therefore, the $Data attribute of $MFT is saved in the NTFS_INFO
 * structure for easy access.  We also use the size of the MFT as
 * a way to calculate the maximum MFT entry number (last_inum).
 *
 * Ok, that is simple, but getting the full $Data attribute can be tough
 * because $MFT may not fit into one MFT entry (i.e. an attribute list). 
 * We need to process the attribute list attribute to find out which
 * other entries to process.  But, the attribute list attribute comes
 * before any $Data attribute (so it could refer to an MFT that has not
 * yet been 'defined').  Although, the $Data attribute seems to always 
 * exist and define at least the run for the entry in the attribute list.
 *
 * So, the way this is solved is that generic mft_lookup is used to get
 * any MFT entry, even $MFT.  If $MFT is not cached then we calculate 
 * the address of where to read based on mutliplication and guessing.  
 * When we are loading the $MFT, we set 'loading_the_MFT' to 1 so
 * that we can update things as we go along.  When we read $MFT we
 * read all the attributes and save info about the $Data one.  If
 * there is an attribute list, we will have the location of the
 * additional MFT in the cached $Data location, which will be 
 * updated as we process the attribute list.  After each MFT
 * entry that we process while loading the MFT, the 'final_inum'
 * value is updated to reflect what we can currently load so 
 * that the sanity checks still work.
 */


/**********************************************************************
 *
 *  MISC FUNCS
 *
 **********************************************************************/

/* convert the NT Time (UTC hundred nanoseconds from 1/1/1601)
 * to UNIX (UTC seconds from 1/1/1970)
 *
 * The basic calculation is to remove the nanoseconds and then
 * subtract the number of seconds between 1601 and 1970
 * i.e. TIME - DELTA
 *
 */
uint32_t
nt2unixtime(uint64_t ntdate)
{
// (369*365 + 89) * 24 * 3600 * 10000000
#define	NSEC_BTWN_1601_1970	(uint64_t)(116444736000000000ULL)

    ntdate -= (uint64_t) NSEC_BTWN_1601_1970;
    ntdate /= (uint64_t) 10000000;

    return (uint32_t) ntdate;
}



/**********************************************************************
 *
 * Lookup Functions
 *
 **********************************************************************/




/**
 * Read an MFT entry and save it in raw form in the given buffer.
 * NOTE: This will remove the update sequence integrity checks in the
 * structure.
 *
 * @param a_ntfs File system to read from
 * @param a_mft Buffer to save raw data to
 * @param a_mftnum Address of MFT entry to read
 *
 * @returns Error value
 */
static TSK_RETVAL_ENUM
ntfs_dinode_lookup(NTFS_INFO * a_ntfs, ntfs_mft * a_mft,
    TSK_INUM_T a_mftnum)
{
    TSK_OFF_T mftaddr_b, mftaddr2_b, offset;
    size_t mftaddr_len = 0;
    int i;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & a_ntfs->fs_info;
    TSK_FS_ATTR_RUN *data_run;
    ntfs_upd *upd;
    uint16_t sig_seq;

    /* sanity checks */
    if (!a_mft) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "mft_lookup: null mft buffer");
        return TSK_ERR;
    }

    if (a_mftnum < fs->first_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "mft_lookup: inode number is too small (%" PRIuINUM ")",
            a_mftnum);
        return TSK_ERR;
    }

    /* Because this code reads teh actual MFT, we need to make sure we 
     * decrement the last_inum because the last value is a special value
     * for the ORPHANS directory */
    if (a_mftnum > fs->last_inum - 1) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "mft_lookup: inode number is too large (%" PRIuINUM ")",
            a_mftnum);
        return TSK_ERR;
    }


    if (tsk_verbose)
        tsk_fprintf(stderr,
            "ntfs_dinode_lookup: Processing MFT %" PRIuINUM "\n",
            a_mftnum);

    /* If mft_data (the cached $Data attribute of $MFT) is not there yet, 
     * then we have not started to load $MFT yet.  In that case, we will
     * 'cheat' and calculate where it goes.  This should only be for
     * $MFT itself, in which case the calculation is easy
     */
    if (!a_ntfs->mft_data) {

        /* This is just a random check with the assumption being that
         * we don't want to just do a guess calculation for a very large
         * MFT entry
         */
        if (a_mftnum > NTFS_LAST_DEFAULT_INO) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_ARG;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "Error trying to load a high MFT entry when the MFT itself has not been loaded (%"
                PRIuINUM ")", a_mftnum);
            return TSK_ERR;
        }

        mftaddr_b = a_ntfs->root_mft_addr + a_mftnum * a_ntfs->mft_rsize_b;
        mftaddr2_b = 0;
    }
    else {
        /* The MFT may not be in consecutive clusters, so we need to use its
         * data attribute run list to find out what address to read
         *
         * This is why we cached it
         */

        // will be set to the address of the MFT entry
        mftaddr_b = mftaddr2_b = 0;

        /* The byte offset within the $Data stream */
        offset = a_mftnum * a_ntfs->mft_rsize_b;

        /* NOTE: data_run values are in clusters 
         *
         * cycle through the runs in $Data and identify which
         * has the MFT entry that we want
         */
        for (data_run = a_ntfs->mft_data->nrd.run;
            data_run != NULL; data_run = data_run->next) {

            /* The length of this specific run */
            TSK_OFF_T run_len = data_run->len * a_ntfs->csize_b;

            /* Is our MFT entry is in this run somewhere ? */
            if (offset < run_len) {

                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "ntfs_dinode_lookup: Found in offset: %"
                        PRIuDADDR "  size: %" PRIuDADDR " at offset: %"
                        PRIuOFF "\n", data_run->addr, data_run->len,
                        offset);

                /* special case where the MFT entry crosses
                 * a run (only happens when cluster size is 512-bytes
                 * and there are an odd number of clusters in the run)
                 */
                if (run_len < offset + a_ntfs->mft_rsize_b) {

                    if (tsk_verbose)
                        tsk_fprintf(stderr,
                            "ntfs_dinode_lookup: Entry crosses run border\n");

                    if (data_run->next == NULL) {
                        tsk_error_reset();
                        tsk_errno = TSK_ERR_FS_INODE_COR;
                        snprintf(tsk_errstr, TSK_ERRSTR_L,
                            "mft_lookup: MFT entry crosses a cluster and there are no more clusters!");
                        return TSK_COR;
                    }

                    /* Assign address where the remainder of the entry is */
                    mftaddr2_b = data_run->next->addr * a_ntfs->csize_b;
                    /* this should always be 512, but just in case */
                    mftaddr_len = (size_t) (run_len - offset);
                }

                /* Assign address of where the MFT entry starts */
                mftaddr_b = data_run->addr * a_ntfs->csize_b + offset;
                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "ntfs_dinode_lookup: Entry address at: %"
                        PRIuOFF "\n", mftaddr_b);
                break;
            }

            /* decrement the offset we are looking for */
            offset -= run_len;
        }

        /* Did we find it? */
        if (!mftaddr_b) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_INODE_NUM;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "mft_lookup: Error finding MFT entry %"
                PRIuINUM " in $MFT", a_mftnum);
            return TSK_ERR;
        }
    }


    /* can we do just one read or do we need multiple? */
    if (mftaddr2_b) {
        ssize_t cnt;
        /* read the first part into mft */
        cnt =
            tsk_fs_read(&a_ntfs->fs_info, mftaddr_b, (char *) a_mft,
            mftaddr_len);
        if (cnt != mftaddr_len) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "ntfs_dinode_lookup: Error reading MFT Entry (part 1) at %"
                PRIuOFF, mftaddr_b);
            return TSK_ERR;
        }

        /* read the second part into mft */
        cnt = tsk_fs_read
            (&a_ntfs->fs_info, mftaddr2_b,
            (char *) ((uintptr_t) a_mft + (uintptr_t) mftaddr_len),
            a_ntfs->mft_rsize_b - mftaddr_len);
        if (cnt != a_ntfs->mft_rsize_b - mftaddr_len) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "ntfs_dinode_lookup: Error reading MFT Entry (part 2) at %"
                PRIuOFF, mftaddr2_b);
            return TSK_ERR;
        }
    }
    else {
        ssize_t cnt;
        /* read the raw entry into mft */
        cnt =
            tsk_fs_read(&a_ntfs->fs_info, mftaddr_b, (char *) a_mft,
            a_ntfs->mft_rsize_b);
        if (cnt != a_ntfs->mft_rsize_b) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "ntfs_dinode_lookup: Error reading MFT Entry at %"
                PRIuOFF, mftaddr_b);
            return TSK_ERR;
        }
    }

    /* if we are saving into the NTFS_INFO structure, assign mnum too */
    if ((uintptr_t) a_mft == (uintptr_t) a_ntfs->mft)
        a_ntfs->mnum = a_mftnum;
    /* Sanity Check */
#if 0
    /* This is no longer applied because it caused too many problems
     * with images that had 0 and 1 etc. as values.  Testing shows that
     * even Windows XP doesn't care if entries have an invalid entry, so
     * this is no longer checked.  The update sequence check should find
     * corrupt entries
     * */
    if ((tsk_getu32(fs->endian, mft->magic) != NTFS_MFT_MAGIC)
        && (tsk_getu32(fs->endian, mft->magic) != NTFS_MFT_MAGIC_BAAD)
        && (tsk_getu32(fs->endian, mft->magic) != NTFS_MFT_MAGIC_ZERO)) {
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "entry %d has an invalid MFT magic: %x",
            mftnum, tsk_getu32(fs->endian, mft->magic));
        return 1;
    }
#endif
    /* The MFT entries have error and integrity checks in them
     * called update sequences.  They must be checked and removed
     * so that later functions can process the data as normal. 
     * They are located in the last 2 bytes of each 512-byte sector
     *
     * We first verify that the the 2-byte value is a give value and
     * then replace it with what should be there
     */
    /* sanity check so we don't run over in the next loop */
    if ((tsk_getu16(fs->endian, a_mft->upd_cnt) > 0) &&
        (((uint32_t) (tsk_getu16(fs->endian,
                        a_mft->upd_cnt) - 1) * a_ntfs->ssize_b) >
            a_ntfs->mft_rsize_b)) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "dinode_lookup: More Update Sequence Entries than MFT size");
        return TSK_COR;
    }
    if (tsk_getu16(fs->endian, a_mft->upd_off) > a_ntfs->mft_rsize_b) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "dinode_lookup: Update sequence offset larger than MFT size");
        return TSK_COR;
    }

    /* Apply the update sequence structure template */
    upd =
        (ntfs_upd *) ((uintptr_t) a_mft + tsk_getu16(fs->endian,
            a_mft->upd_off));
    /* Get the sequence value that each 16-bit value should be */
    sig_seq = tsk_getu16(fs->endian, upd->upd_val);
    /* cycle through each sector */
    for (i = 1; i < tsk_getu16(fs->endian, a_mft->upd_cnt); i++) {
        uint8_t *new_val, *old_val;
        /* The offset into the buffer of the value to analyze */
        size_t offset = i * a_ntfs->ssize_b - 2;
        /* get the current sequence value */
        uint16_t cur_seq =
            tsk_getu16(fs->endian, (uintptr_t) a_mft + offset);
        if (cur_seq != sig_seq) {
            /* get the replacement value */
            uint16_t cur_repl =
                tsk_getu16(fs->endian, &upd->upd_seq + (i - 1) * 2);
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_GENFS;

            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "Incorrect update sequence value in MFT entry\nSignature Value: 0x%"
                PRIx16 " Actual Value: 0x%" PRIx16
                " Replacement Value: 0x%" PRIx16
                "\nThis is typically because of a corrupted entry",
                sig_seq, cur_seq, cur_repl);
            return TSK_COR;
        }

        new_val = &upd->upd_seq + (i - 1) * 2;
        old_val = (uint8_t *) ((uintptr_t) a_mft + offset);
        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_dinode_lookup: upd_seq %i   Replacing: %.4"
                PRIx16 "   With: %.4" PRIx16 "\n", i,
                tsk_getu16(fs->endian, old_val), tsk_getu16(fs->endian,
                    new_val));
        *old_val++ = *new_val++;
        *old_val = *new_val;
    }

    return TSK_OK;
}



/*
 * given a cluster, return the allocation status or
 * -1 if an error occurs
 */
static int
is_clustalloc(NTFS_INFO * ntfs, TSK_DADDR_T addr)
{
    int bits_p_clust, b;
    TSK_DADDR_T base;
    bits_p_clust = 8 * ntfs->fs_info.block_size;

    /* While we are loading the MFT, assume that everything
     * is allocated.  This should only be needed when we are
     * dealing with an attribute list ... 
     */
    if (ntfs->loading_the_MFT == 1) {
        return 1;
    }
    else if (ntfs->bmap == NULL) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;

        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "is_clustalloc: Bitmap pointer is null: %" PRIuDADDR
            "\n", addr);
        return -1;
    }

    /* Is the cluster too big? */
    if (addr > ntfs->fs_info.last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "is_clustalloc: cluster too large");
        return -1;
    }

    /* identify the base cluster in the bitmap file */
    base = addr / bits_p_clust;
    b = (int) (addr % bits_p_clust);

    /* is this the same as in the cached buffer? */
    if (base != ntfs->bmap_buf_off) {
        TSK_DADDR_T c = base;
        TSK_FS_ATTR_RUN *run;
        TSK_DADDR_T fsaddr = 0;
        ssize_t cnt;

        /* get the file system address of the bitmap cluster */
        for (run = ntfs->bmap; run; run = run->next) {
            if (run->len <= c) {
                c -= run->len;
            }
            else {
                fsaddr = run->addr + c;
                break;
            }
        }

        if (fsaddr == 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_BLK_NUM;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "is_clustalloc: cluster not found in bitmap: %"
                PRIuDADDR "", c);
            return -1;
        }
        if (fsaddr > ntfs->fs_info.last_block) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_BLK_NUM;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "is_clustalloc: Cluster in bitmap too large for image: %"
                PRIuDADDR, fsaddr);
            return -1;
        }
        ntfs->bmap_buf_off = base;
        cnt = tsk_fs_read_block
            (&ntfs->fs_info, fsaddr, ntfs->bmap_buf,
            ntfs->fs_info.block_size);
        if (cnt != ntfs->fs_info.block_size) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_READ;
            }
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "is_clustalloc: Error reading bitmap at %"
                PRIuDADDR, fsaddr);
            return -1;
        }
    }

    /* identify if the cluster is allocated or not */
    return (isset(ntfs->bmap_buf, b)) ? 1 : 0;
}



/**********************************************************************
 *
 *  TSK_FS_ATTR functions
 *
 **********************************************************************/


/**
 * Process a non-resident runlist and convert its contents into the generic fs_attr_run
 * structure. 
 * @param ntfs File system that attribute is located in.
 * @param start_vcn The starting VCN for this run. 
 * @param runlist The raw runlist data from the MFT entry.
 * @param a_data_run_head [out] Pointer to pointer of run that is created. (NULL on error and for $BadClust - special case because it is a sparse file for the entire FS).
 * @param totlen [out] Pointer to location where total length of run (in bytes) can be returned (or NULL)
 *
 * @returns Return status of error, corrupt, or OK (note a_data_run can be NULL even when OK is returned if $BadClust is encountered)
 */
static TSK_RETVAL_ENUM
ntfs_make_data_run(NTFS_INFO * ntfs, TSK_OFF_T start_vcn,
    ntfs_runlist * runlist_head, TSK_FS_ATTR_RUN ** a_data_run_head,
    TSK_OFF_T * totlen)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) ntfs;
    ntfs_runlist *run;
    TSK_FS_ATTR_RUN *data_run, *data_run_prev = NULL;
    unsigned int i, idx;
    TSK_DADDR_T prev_addr = 0;
    TSK_OFF_T file_offset = start_vcn;

    run = runlist_head;
    *a_data_run_head = NULL;

    /* initialize if non-NULL */
    if (totlen)
        *totlen = 0;

    /* Cycle through each run in the runlist 
     * We go until we find an entry with no length
     * An entry with offset of 0 is for a sparse run
     */
    while (NTFS_RUNL_LENSZ(run) != 0) {
        int64_t addr_offset = 0;

        /* allocate a new tsk_fs_attr_run */
        if ((data_run = tsk_fs_attr_run_alloc()) == NULL) {
            tsk_fs_attr_run_free(*a_data_run_head);
            *a_data_run_head = NULL;
            return TSK_ERR;
        }

        /* make the list, unless its the first pass & then we set the head */
        if (data_run_prev)
            data_run_prev->next = data_run;
        else
            *a_data_run_head = data_run;
        data_run_prev = data_run;

        /* These fields are a variable number of bytes long
         * these for loops are the equivalent of the getuX macros
         */
        idx = 0;
        /* Get the length of this run */
        for (i = 0, data_run->len = 0; i < NTFS_RUNL_LENSZ(run); i++) {
            data_run->len |= (run->buf[idx++] << (i * 8));
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "ntfs_make_data_run: Len idx: %i cur: %"
                    PRIu8 " (%" PRIx8 ") tot: %" PRIuDADDR
                    " (%" PRIxDADDR ")\n", i,
                    run->buf[idx - 1], run->buf[idx - 1],
                    data_run->len, data_run->len);
        }

        /* Sanity check on length */
        if (data_run->len > fs->block_count) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_INODE_COR;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "ntfs_make_run: Run length is larger than file system");
            tsk_fs_attr_run_free(*a_data_run_head);
            *a_data_run_head = NULL;
            return TSK_COR;
        }

        data_run->offset = file_offset;
        file_offset += data_run->len;

        /* Update the length if we were passed a value */
        if (totlen)
            *totlen += (data_run->len * ntfs->csize_b);

        /* Get the address of this run */
        for (i = 0, data_run->addr = 0; i < NTFS_RUNL_OFFSZ(run); i++) {
            //data_run->addr |= (run->buf[idx++] << (i * 8));
            addr_offset |= (run->buf[idx++] << (i * 8));
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "ntfs_make_data_run: Off idx: %i cur: %"
                    PRIu8 " (%" PRIx8 ") tot: %" PRIuDADDR
                    " (%" PRIxDADDR ")\n", i,
                    run->buf[idx - 1], run->buf[idx - 1], addr_offset,
                    addr_offset);
        }

        /* addr_offset value is signed so extend it to 64-bits */
        if ((int8_t) run->buf[idx - 1] < 0) {
            for (; i < sizeof(addr_offset); i++)
                addr_offset |= (int64_t) ((int64_t) 0xff << (i * 8));
        }

        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_make_data_run: Signed addr_offset: %"
                PRIdDADDR " Previous address: %"
                PRIdDADDR "\n", addr_offset, prev_addr);

        /* The NT 4.0 version of NTFS uses an offset of -1 to represent
         * a hole, so add the sparse flag and make it look like the 2K
         * version with a offset of 0
         *
         * A user reported an issue where the $Bad file started with
         * its offset as -1 and it was not NT (maybe a conversion)
         * Change the check now to not limit to NT, but make sure
         * that it is the first run
         */
        if (((addr_offset == -1) && (prev_addr == 0))
            || ((addr_offset == -1)
                && (ntfs->ver == NTFS_VINFO_NT))) {
            data_run->flags |= TSK_FS_ATTR_RUN_FLAG_SPARSE;
            data_run->addr = 0;
            if (tsk_verbose)
                tsk_fprintf(stderr, "ntfs_make_data_run: Sparse Run\n");
        }

        /* A Sparse file has a run with an offset of 0
         * there is a special case though of the BOOT MFT entry which
         * is the super block and has a legit offset of 0.
         *
         * The value given is a delta of the previous offset, so add 
         * them for non-sparse files
         *
         * For sparse files the next run will have its offset relative 
         * to the current "prev_addr" so skip that code
         */
        else if ((addr_offset) || (ntfs->mnum == NTFS_MFT_BOOT)) {

            data_run->addr = prev_addr + addr_offset;
            prev_addr = data_run->addr;

            /* Sanity check on length and offset */
            if (data_run->addr + data_run->len > fs->block_count) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_INODE_COR;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "ntfs_make_run: Run offset and length is larger than file system");
                tsk_fs_attr_run_free(*a_data_run_head);
                *a_data_run_head = NULL;
                return TSK_COR;
            }

        }
        else {
            data_run->flags |= TSK_FS_ATTR_RUN_FLAG_SPARSE;
            if (tsk_verbose)
                tsk_fprintf(stderr, "ntfs_make_data_run: Sparse Run\n");
        }

        /* Advance run */
        run = (ntfs_runlist *) ((uintptr_t) run + (1 + NTFS_RUNL_LENSZ(run)
                + NTFS_RUNL_OFFSZ(run)));
    }

    /* special case for $BADCLUST, which is a sparse file whose size is
     * the entire file system.
     *
     * If there is only one run entry and it is sparse, then there are no
     * bad blocks, so get rid of it.
     */
    if ((*a_data_run_head != NULL)
        && ((*a_data_run_head)->next == NULL)
        && ((*a_data_run_head)->flags & TSK_FS_ATTR_RUN_FLAG_SPARSE)
        && ((*a_data_run_head)->len == fs->last_block + 1)) {
        tsk_fs_attr_run_free(*a_data_run_head);
        *a_data_run_head = NULL;
    }

    return TSK_OK;
}



/*********** UNCOMPRESSION CODE *************/


/*
 * NTFS Breaks compressed data into compression units, which are
 * typically 16 clusters in size. If the data in the comp  unit
 * compresses to something smaller than 16 clusters then the
 * compresed data is stored and the rest of the compression unit
 * is filled with sparse clusters. The entire compression unit
 * can also be sparse. 
 *
 * When the data is compressed, it is broken up into 4k blocks. Each
 * of the blocks is compressed and the resulting data is stored with
 * a 2-byte header that identifies the compressed size.   The 
 * compressed data contains token groups, which contain a 1 byte
 * header followed by 8 variable length tokens.  There are two types
 * of tokens. Symbol tokens are 1 byte in length and the 1byte value
 * should be directly copied into the uncompressed data.  Phrase tokens
 * identify a run of data in the same compression unit that should be 
 * copied to the current location.  Each bit in the 1-byte header identifies
 * the type of the 8 tokens in the group. 
 *
 */

 /* Variables used for ntfs_uncompress() method */
typedef struct {
    char *uncomp_buf;           // Buffer for uncompressed data
    char *comp_buf;             // buffer for compressed data
    size_t comp_len;            // number of bytes used in compressed data
    size_t uncomp_idx;          // Index into buffer for next byte
    size_t buf_size_b;          // size of buffer in bytes (1 compression unit)
} NTFS_COMP_INFO;


/**
 * Reset the values in the NTFS_COMP_INFO structure.  We need to 
 * do this in between every compression unit that we process in the file.
 *
 * @param comp Structure to reset
 */
static void
ntfs_uncompress_reset(NTFS_COMP_INFO * comp)
{
    memset(comp->uncomp_buf, 0, comp->buf_size_b);
    comp->uncomp_idx = 0;
    memset(comp->comp_buf, 0, comp->buf_size_b);
    comp->comp_len = 0;
}

/**
 * Setup the NTFS_COMP_INFO structure with a buffer and 
 * initialize the basic settings.
 *
 * @param fs File system state information
 * @param comp Compression state information to initialize
 * @param compunit_size_c The size (in clusters) of a compression
 * unit
 * @return 1 on error and 0 on success
 */
static int
ntfs_uncompress_setup(TSK_FS_INFO * fs, NTFS_COMP_INFO * comp,
    uint32_t compunit_size_c)
{
    comp->buf_size_b = fs->block_size * compunit_size_c;
    if ((comp->uncomp_buf = tsk_malloc(comp->buf_size_b)) == NULL) {
        comp->buf_size_b = 0;
        return 1;
    }
    if ((comp->comp_buf = tsk_malloc(comp->buf_size_b)) == NULL) {
        comp->buf_size_b = 0;
        return 1;
    }

    ntfs_uncompress_reset(comp);

    return 0;
}

static void
ntfs_uncompress_done(NTFS_COMP_INFO * comp)
{
    if (comp->uncomp_buf)
        free(comp->uncomp_buf);
    comp->uncomp_buf = NULL;
    if (comp->comp_buf)
        free(comp->comp_buf);
    comp->comp_buf = NULL;
    comp->buf_size_b = 0;
}


 /**
  * Uncompress the block of data in comp->comp_buf, 
  * which has a size of comp->comp_len.
  * Store the result in the comp->uncomp_buf. 
  *
  * @param comp Compression unit structure
  *
  * @returns 1 on error and 0 on success
  */
static uint8_t
ntfs_uncompress_compunit(NTFS_COMP_INFO * comp)
{
    size_t cl_index;

    tsk_error_reset();

    comp->uncomp_idx = 0;

    /* Cycle through the compressed data 
     * We maintain state using different levels of loops.
     * We use +1 here because the size value at start of block is 2 bytes.
     */
    for (cl_index = 0; cl_index + 1 < comp->comp_len;) {
        size_t blk_end;         // index into the buffer to where block ends
        size_t blk_size;        // size of the current block 
        uint8_t iscomp;         // set to 1 if block is compressed
        size_t blk_st_uncomp;   // index into uncompressed buffer where block started

        /* The first two bytes of each block contain the size
         * information.*/
        blk_size = ((((unsigned char) comp->comp_buf[cl_index + 1] << 8) |
                ((unsigned char) comp->comp_buf[cl_index])) & 0x0FFF) + 3;

        // this seems to indicate end of block
        if (blk_size == 3)
            break;

        blk_end = cl_index + blk_size;
        if (blk_end > comp->comp_len) {
            tsk_errno = TSK_ERR_FS_FWALK;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "ntfs_uncompress_compunit: Block length longer than buffer length: %"
                PRIuSIZE "", blk_end);
            return 1;
        }

        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_uncompress_compunit: Block size is %" PRIuSIZE "\n",
                blk_size);

        /* The MSB identifies if the block is compressed */
        if ((comp->comp_buf[cl_index + 1] & 0x8000) == 0)
            iscomp = 0;
        else
            iscomp = 1;

        // keep track of where this block started in the buffer
        blk_st_uncomp = comp->uncomp_idx;
        cl_index += 2;

        // the 4096 size seems to occur at the same times as no compression
        if ((iscomp) || (blk_size - 2 != 4096)) {

            // cycle through the block
            while (cl_index < blk_end) {
                int a;

                // get the header header
                unsigned char header = comp->comp_buf[cl_index];
                cl_index++;

                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "ntfs_uncompress_compunit: New Tag: %x\n", header);

                for (a = 0; a < 8 && cl_index < blk_end; a++) {

                    /* Determine token type and parse appropriately. *
                     * Symbol tokens are the symbol themselves, so copy it
                     * into the umcompressed buffer 
                     */
                    if ((header & NTFS_TOKEN_MASK) == NTFS_SYMBOL_TOKEN) {
                        if (tsk_verbose)
                            tsk_fprintf(stderr,
                                "ntfs_uncompress_compunit: Symbol Token: %"
                                PRIuSIZE "\n", cl_index);

                        if (comp->uncomp_idx >= comp->buf_size_b) {
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_uncompress_compunit: Trying to write past end of uncompression buffer: %"
                                PRIuSIZE "", comp->uncomp_idx);
                            return 1;
                        }
                        comp->uncomp_buf[comp->uncomp_idx++] =
                            comp->comp_buf[cl_index];

                        cl_index++;
                    }

                    /* Otherwise, it is a phrase token, which points back
                     * to a previous sequence of bytes. 
                     */
                    else {
                        size_t i;
                        int shift;
                        size_t start_position_index = 0;
                        size_t end_position_index = 0;
                        unsigned int offset = 0;
                        unsigned int length = 0;
                        uint16_t pheader;

                        if (cl_index + 1 >= blk_end) {
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_uncompress_compunit: Phrase token index is past end of block: %d",
                                a);
                            return 1;
                        }

                        pheader =
                            ((((comp->comp_buf[cl_index +
                                            1]) << 8) & 0xFF00) |
                            (comp->comp_buf[cl_index] & 0xFF));
                        cl_index += 2;


                        /* The number of bits for the start and length              
                         * in the 2-byte header change depending on the 
                         * location in the compression unit.  This identifies
                         * how many bits each has */
                        shift = 0;
                        for (i =
                            comp->uncomp_idx -
                            blk_st_uncomp - 1; i >= 0x10; i >>= 1) {
                            shift++;
                        }

                        //tsk_fprintf(stderr, "Start: %X  Shift: %d  UnComp_IDX %d  BlkStart: %lu  BlkIdx: %d  BlkSize: %d\n", (int)(comp->uncomp_idx - comp->blk_st - 1), shift, comp->uncomp_idx, comp->blk_st, comp->blk_idx, comp->blk_size);

                        offset = (pheader >> (12 - shift)) + 1;
                        length = (pheader & (0xFFF >> shift)) + 2;

                        start_position_index = comp->uncomp_idx - offset;
                        end_position_index = start_position_index + length;

                        if (tsk_verbose)
                            tsk_fprintf(stderr,
                                "ntfs_uncompress_compunit: Phrase Token: %"
                                PRIuSIZE "\t%d\t%d\t%x\n", cl_index,
                                length, offset, pheader);

                        /* Sanity checks on values */
                        if (offset > comp->uncomp_idx) {
                            tsk_error_reset();
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_uncompress_compunit: Phrase token offset is too large:  %d (max: %"
                                PRIuSIZE ")", offset, comp->uncomp_idx);
                            return 1;
                        }
                        else if (length + start_position_index >
                            comp->buf_size_b) {
                            tsk_error_reset();
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_uncompress_compunit: Phrase token length is too large:  %d (max: %zu)",
                                length,
                                comp->buf_size_b - start_position_index);
                            return 1;
                        }
                        else if (end_position_index -
                            start_position_index + 1 >
                            comp->buf_size_b - comp->uncomp_idx) {
                            tsk_error_reset();
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_uncompress_compunit: Phrase token length is too large for rest of uncomp buf:  %zu (max: %"
                                PRIuSIZE ")",
                                end_position_index - start_position_index +
                                1, comp->buf_size_b - comp->uncomp_idx);
                            return 1;
                        }

                        for (;
                            start_position_index <= end_position_index
                            && comp->uncomp_idx < comp->buf_size_b;
                            start_position_index++) {

                            // Copy the previous data to the current position
                            comp->uncomp_buf[comp->uncomp_idx++]
                                = comp->uncomp_buf[start_position_index];
                        }
                    }
                    header >>= 1;
                }               // end of loop inside of token group

            }                   // end of loop inside of block
        }

        // this block contains uncompressed data uncompressed data
        else {
            while (cl_index < blk_end && cl_index < comp->comp_len) {
                /* This seems to happen only with corrupt data -- such as
                 * when an unallocated file is being processed... */
                if (comp->uncomp_idx >= comp->buf_size_b) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_FWALK;
                    snprintf(tsk_errstr, TSK_ERRSTR_L,
                        "ntfs_uncompress_compunit: Trying to write past end of uncompression buffer (1) -- corrupt data?)");
                    return 1;
                }

                // Place data in uncompression_buffer
                comp->uncomp_buf[comp->uncomp_idx++] =
                    comp->comp_buf[cl_index++];
            }
        }
    }                           // end of loop inside of compression unit

    return 0;
}



/**
 * Process a compression unit and return the decompressed data in a buffer in comp. 
 *
 * @param ntfs File system
 * @param comp Compression state info (output will be stored in here)
 * @param comp_unit List of addresses that store compressed data
 * @param comp_unit_size Number of addresses in comp_unit
 * @returns 1 on error and 0 on success
 */
static uint8_t
ntfs_proc_compunit(NTFS_INFO * ntfs, NTFS_COMP_INFO * comp,
    TSK_DADDR_T * comp_unit, uint32_t comp_unit_size)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) ntfs;
    int sparse;
    uint64_t a;

    /* With compressed attributes, there are three scenarios.
     * 1: The compression unit is not compressed,
     * 2: The compression unit is sparse
     * 3: The compression unit is compressed
     */

    /* Check if the entire compression unit is sparse */
    sparse = 1;
    for (a = 0; a < comp_unit_size && sparse == 1; a++) {
        if (comp_unit[a]) {
            sparse = 0;
            break;
        }
    }

    /* Entire comp unit is sparse... */
    if (sparse) {
        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_proc_compunit: Unit is fully sparse\n");

        memset(comp->uncomp_buf, 0, comp->buf_size_b);
        comp->uncomp_idx = comp->buf_size_b;
    }

    /* Check if the end of the unit is sparse, which means the
     * unit is compressed */
    else if (comp_unit[comp_unit_size - 1] == 0) {

        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_proc_compunit: Unit is compressed\n");

        // load up the compressed buffer so we can decompress it
        ntfs_uncompress_reset(comp);
        for (a = 0; a < comp_unit_size; a++) {
            ssize_t cnt;

            if (comp_unit[a] == 0)
                break;

            /* To get the uncompressed size, we must uncompress the
             * data -- even if addresses are only needed */
            cnt =
                tsk_fs_read_block(fs, comp_unit[a],
                &comp->comp_buf[comp->comp_len], fs->block_size);
            if (cnt != fs->block_size) {
                if (cnt >= 0) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_READ;
                }
                snprintf(tsk_errstr2, TSK_ERRSTR_L,
                    "ntfs_proc_compunit: Error reading block at %"
                    PRIuDADDR, comp_unit[a]);
                return 1;
            }
            comp->comp_len += fs->block_size;
        }

        if (ntfs_uncompress_compunit(comp)) {
            return 1;
        }
    }

    /* Uncompressed data */
    else {
        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_proc_compunit: Unit is not compressed\n");

        comp->uncomp_idx = 0;
        for (a = 0; a < comp_unit_size; a++) {
            ssize_t cnt;

            cnt =
                tsk_fs_read_block(fs, comp_unit[a],
                &comp->uncomp_buf[comp->uncomp_idx], fs->block_size);
            if (cnt != fs->block_size) {
                if (cnt >= 0) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_READ;
                }
                snprintf(tsk_errstr2, TSK_ERRSTR_L,
                    "ntfs_proc_compunit: Error reading block at %"
                    PRIuDADDR, comp_unit[a]);
                return 1;
            }
            comp->uncomp_idx += fs->block_size;
        }
    }
    return 0;
}



/**
 * Currently ignores the SPARSE flag
 */
uint8_t
ntfs_attr_walk_special(const TSK_FS_ATTR * fs_attr,
    int flags, TSK_FS_FILE_WALK_CB a_action, void *ptr)
{
    TSK_FS_INFO *fs;
    NTFS_INFO *ntfs;

    // clean up any error messages that are lying around
    tsk_error_reset();
    if ((fs_attr == NULL) || (fs_attr->fs_file == NULL)
        || (fs_attr->fs_file->meta == NULL)
        || (fs_attr->fs_file->fs_info == NULL)) {
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_attr_walk_special: Null arguments given\n");
        return 1;
    }

    fs = fs_attr->fs_file->fs_info;
    ntfs = (NTFS_INFO *) fs;

    /* Process the compressed buffer 
     *
     * The compsize value equal to 0 can occur if we are processing an
     * isolated entry that is part of an attribute list.  The first
     * sequence of the attribute has the compsize and the latter ones
     * do not. So, if one of the non-base MFT entries is processed by
     * itself, we have that case.  I tried to assume it was 16, but it
     * caused decompression problems -- likely because this sequence
     * did not start on a compression unit boundary.  So, now we just
     * dump the compressed data instead of giving an error.
     */
    if (fs_attr->flags & TSK_FS_ATTR_COMP) {
        TSK_DADDR_T addr;
        TSK_FS_ATTR_RUN *fs_attr_run;
        TSK_DADDR_T *comp_unit;
        uint32_t comp_unit_idx = 0;
        NTFS_COMP_INFO comp;
        TSK_OFF_T off = 0;
        int retval;
        uint8_t stop_loop = 0;

        if (fs_attr->nrd.compsize <= 0) {
            tsk_errno = TSK_ERR_FS_FWALK;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "ntfs_attrwalk_special: Compressed attribute has compsize of 0");
            return 1;
        }

        /* Allocate the buffers and state structure */
        if (ntfs_uncompress_setup(fs, &comp, fs_attr->nrd.compsize)) {
            return 1;
        }

        comp_unit =
            (TSK_DADDR_T *) tsk_malloc(fs_attr->nrd.compsize *
            sizeof(TSK_DADDR_T));
        if (comp_unit == NULL) {
            ntfs_uncompress_done(&comp);
            return 1;
        }
        retval = TSK_WALK_CONT;

        /* cycle through the number of runs we have */
        for (fs_attr_run = fs_attr->nrd.run; fs_attr_run;
            fs_attr_run = fs_attr_run->next) {
            size_t len_idx;

            /* We may get a FILLER entry at the beginning of the run
             * if we are processing a non-base file record since 
             * this $DATA attribute could not be the first sequence in the 
             * attribute. Therefore, do not error if it starts at 0 */
            if (fs_attr_run->flags & TSK_FS_ATTR_RUN_FLAG_FILLER) {
                if (fs_attr_run->addr != 0) {
                    tsk_error_reset();

                    if (fs_attr->fs_file->meta->
                        flags & TSK_FS_META_FLAG_UNALLOC)
                        tsk_errno = TSK_ERR_FS_RECOVER;
                    else
                        tsk_errno = TSK_ERR_FS_GENFS;
                    snprintf(tsk_errstr, TSK_ERRSTR_L,
                        "Filler Entry exists in fs_attr_run %"
                        PRIuDADDR "@%" PRIuDADDR
                        " - type: %" PRIu32 "  id: %d", fs_attr_run->len,
                        fs_attr_run->addr, fs_attr->type, fs_attr->id);
                    free(comp_unit);
                    ntfs_uncompress_done(&comp);
                    return 1;
                }
                else {
                    off += (fs_attr_run->len * fs->block_size);
                    continue;
                }
            }

            addr = fs_attr_run->addr;

            /* cycle through each cluster in the run */
            for (len_idx = 0; len_idx < fs_attr_run->len; len_idx++) {

                if (addr > fs->last_block) {
                    tsk_error_reset();

                    if (fs_attr->fs_file->meta->
                        flags & TSK_FS_META_FLAG_UNALLOC)
                        tsk_errno = TSK_ERR_FS_RECOVER;
                    else
                        tsk_errno = TSK_ERR_FS_BLK_NUM;
                    snprintf(tsk_errstr, TSK_ERRSTR_L,
                        "Invalid address in run (too large): %"
                        PRIuDADDR "", addr);

                    free(comp_unit);
                    ntfs_uncompress_done(&comp);
                    return 1;
                }

                // queue up the addresses until we get a full unit
                comp_unit[comp_unit_idx++] = addr;

                // time to decompress (if queue is full or this is the last block)
                if ((comp_unit_idx == fs_attr->nrd.compsize)
                    || ((len_idx == fs_attr_run->len - 1)
                        && (fs_attr_run->next == NULL))) {
                    size_t i;

                    // decompress the unit
                    if (ntfs_proc_compunit(ntfs, &comp, comp_unit,
                            comp_unit_idx)) {
                        free(comp_unit);
                        ntfs_uncompress_done(&comp);
                        return 1;
                    }

                    // now call the callback with the uncompressed data
                    for (i = 0; i < comp_unit_idx; i++) {
                        int myflags;
                        size_t read_len;

                        myflags =
                            TSK_FS_BLOCK_FLAG_CONT |
                            TSK_FS_BLOCK_FLAG_COMP;
                        retval = is_clustalloc(ntfs, comp_unit[i]);
                        if (retval == -1) {
                            if (fs_attr->fs_file->meta->
                                flags & TSK_FS_META_FLAG_UNALLOC)
                                tsk_errno = TSK_ERR_FS_RECOVER;
                            free(comp_unit);
                            ntfs_uncompress_done(&comp);
                            return 1;
                        }
                        else if (retval == 1) {
                            myflags |= TSK_FS_BLOCK_FLAG_ALLOC;
                        }
                        else if (retval == 0) {
                            myflags |= TSK_FS_BLOCK_FLAG_UNALLOC;
                        }

                        if (fs_attr->size - off > fs->block_size)
                            read_len = fs->block_size;
                        else
                            read_len = (size_t) (fs_attr->size - off);

                        if (i * fs->block_size + read_len >
                            comp.uncomp_idx) {
                            tsk_errno = TSK_ERR_FS_FWALK;
                            snprintf(tsk_errstr, TSK_ERRSTR_L,
                                "ntfs_attrwalk_special: Trying to read past end of uncompressed buffer: %"
                                PRIuSIZE " %" PRIuSIZE "",
                                i * fs->block_size + read_len,
                                comp.uncomp_idx);
                            free(comp_unit);
                            ntfs_uncompress_done(&comp);
                            return 1;
                        }

                        // call the callback
                        retval =
                            a_action(fs_attr->fs_file, off, comp_unit[i],
                            &comp.uncomp_buf[i * fs->block_size], read_len,
                            myflags, ptr);

                        off += read_len;

                        if (off >= fs_attr->size) {
                            stop_loop = 1;
                            break;
                        }
                        if (retval != TSK_WALK_CONT) {
                            stop_loop = 1;
                            break;
                        }
                    }
                    comp_unit_idx = 0;
                }

                if (stop_loop)
                    break;

                /* If it is a sparse run, don't increment the addr so that
                 * it remains 0 */
                if (((fs_attr_run->flags & TSK_FS_ATTR_RUN_FLAG_SPARSE) ==
                        0)
                    && ((fs_attr_run->
                            flags & TSK_FS_ATTR_RUN_FLAG_FILLER) == 0))
                    addr++;
            }

            if (stop_loop)
                break;
        }

        ntfs_uncompress_done(&comp);
        free(comp_unit);

        if (retval == TSK_WALK_ERROR)
            return 1;
        else
            return 0;
    }
    else {
        tsk_errno = TSK_ERR_FS_FWALK;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_attrwalk_special: called with non-special attribute: %x",
            fs_attr->flags);
        return 1;
    }
}


ssize_t
ntfs_file_read_special(const TSK_FS_ATTR * a_fs_attr,
    TSK_OFF_T a_offset, char *a_buf, size_t a_len)
{
    TSK_FS_INFO *fs;
    NTFS_INFO *ntfs;

    if ((a_fs_attr == NULL) || (a_fs_attr->fs_file == NULL)
        || (a_fs_attr->fs_file->meta == NULL)
        || (a_fs_attr->fs_file->fs_info == NULL)) {
    }

    fs = a_fs_attr->fs_file->fs_info;
    ntfs = (NTFS_INFO *) fs;

    if (a_fs_attr->flags & TSK_FS_ATTR_COMP) {
        TSK_FS_ATTR_RUN *data_run_cur;
        TSK_OFF_T cu_blkoffset; // block offset of starting compression unit to start reading from
        size_t byteoffset;      // byte offset in compression unit of where we want to start reading from
        TSK_DADDR_T *comp_unit;
        uint32_t comp_unit_idx = 0;
        NTFS_COMP_INFO comp;
        size_t buf_idx = 0;

        if (a_fs_attr->nrd.compsize <= 0) {
            tsk_errno = TSK_ERR_FS_FWALK;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "ntfs_file_read_special: Compressed attribute has compsize of 0");
            return -1;
        }

        if (a_offset > a_fs_attr->nrd.allocsize) {
            return 0;
        }

        // we return 0s for reads past the initsize
        if (a_offset >= a_fs_attr->nrd.initsize) {
            ssize_t len;

            if (tsk_verbose)
                fprintf(stderr,
                    "ntfs_file_read_special: Returning 0s for read past end of initsize (%"
                    PRIuINUM ")\n", a_fs_attr->fs_file->meta->addr);

            if (a_offset + a_len > a_fs_attr->nrd.allocsize)
                len = (ssize_t) (a_fs_attr->nrd.allocsize - a_offset);
            else
                len = (ssize_t) a_len;
            memset(a_buf, 0, a_len);
            return len;
        }

        /* Allocate the buffers and state structure */
        if (ntfs_uncompress_setup(fs, &comp, a_fs_attr->nrd.compsize)) {
            return -1;
        }

        comp_unit =
            (TSK_DADDR_T *) tsk_malloc(a_fs_attr->nrd.compsize *
            sizeof(TSK_DADDR_T));
        if (comp_unit == NULL) {
            ntfs_uncompress_done(&comp);
            return -1;
        }

        // figure out the needed offsets
        cu_blkoffset = a_offset / fs->block_size;
        if (cu_blkoffset) {
            cu_blkoffset /= a_fs_attr->nrd.compsize;
            cu_blkoffset *= a_fs_attr->nrd.compsize;
        }
        byteoffset = (size_t) (a_offset - cu_blkoffset * fs->block_size);

        // cycle through the run until we find where we can start to process the clusters
        for (data_run_cur = a_fs_attr->nrd.run;
            (data_run_cur) && (buf_idx < a_len);
            data_run_cur = data_run_cur->next) {

            TSK_DADDR_T addr;
            size_t a;

            // See if this run contains the starting offset they requested
            if (data_run_cur->offset + data_run_cur->len <
                (TSK_DADDR_T) cu_blkoffset)
                continue;


            // seek to the start of where we want to read (we may need to read several runs)
            if (data_run_cur->offset > (TSK_DADDR_T) cu_blkoffset)
                a = 0;
            else
                a = (size_t) (cu_blkoffset - data_run_cur->offset);

            addr = data_run_cur->addr;
            // don't increment addr if it is 0 -- sparse
            if (addr)
                addr += a;

            /* cycle through the relevant in the run */
            for (; a < data_run_cur->len && buf_idx < a_len; a++) {

                // queue up the addresses until we get a full unit
                comp_unit[comp_unit_idx++] = addr;

                // time to decompress (if queue is full or this is the last block)
                if ((comp_unit_idx == a_fs_attr->nrd.compsize)
                    || ((a == data_run_cur->len - 1)
                        && (data_run_cur->next == NULL))) {
                    size_t cpylen;

                    // decompress the unit
                    if (ntfs_proc_compunit(ntfs, &comp, comp_unit,
                            comp_unit_idx)) {
                        free(comp_unit);
                        ntfs_uncompress_done(&comp);
                        return -1;
                    }

                    // copy uncompressed data to the output buffer
                    if (comp.uncomp_idx < byteoffset) {

                        // @@ ERROR
                        free(comp_unit);
                        ntfs_uncompress_done(&comp);
                        return -1;
                    }
                    else if (comp.uncomp_idx - byteoffset <
                        a_len - buf_idx) {
                        cpylen = comp.uncomp_idx - byteoffset;
                    }
                    else {
                        cpylen = a_len - buf_idx;
                    }
                    memcpy(&a_buf[buf_idx], &comp.uncomp_buf[byteoffset],
                        cpylen);

                    // reset this in case we need to also read from the next run 
                    byteoffset = 0;
                    buf_idx += cpylen;
                    comp_unit_idx = 0;
                }
                /* If it is a sparse run, don't increment the addr so that
                 * it remains 0 */
                if (((data_run_cur->flags & TSK_FS_ATTR_RUN_FLAG_SPARSE) ==
                        0)
                    && ((data_run_cur->
                            flags & TSK_FS_ATTR_RUN_FLAG_FILLER) == 0))
                    addr++;
            }
        }

        free(comp_unit);
        ntfs_uncompress_done(&comp);
        return (ssize_t) buf_idx;
    }
    else {
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_file_read_special: called with non-special attribute: %x",
            a_fs_attr->flags);
        return -1;
    }
}



/* 
 * Process an NTFS attribute sequence and load the data into data
 * structures. 
 * An attribute sequence is a linked list of the attributes in an MFT entry.
 * This is called by copy_inode and proc_attrlist.
 *
 * @param ntfs File system to analyze
 * @param fs_file Generic metadata structure to add the attribute info to
 * @param attrseq Start of the attribute sequence to analyze
 * @param len Length of the attribute sequence buffer
 * @returns Error code
 */
static TSK_RETVAL_ENUM
ntfs_proc_attrseq(NTFS_INFO * ntfs,
    TSK_FS_FILE * fs_file, ntfs_attr * attrseq, size_t len)
{
    ntfs_attr *attr = attrseq;
    const TSK_FS_ATTR *fs_attr_attrl = NULL;
    char name[NTFS_MAXNAMLEN_UTF8 + 1];
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ntfs->fs_info;

    if (tsk_verbose)
        tsk_fprintf(stderr,
            "ntfs_proc_attrseq: Processing entry %"
            PRIuINUM "\n", fs_file->meta->addr);

    if (fs_file->meta->attr == NULL) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Null attribute list in ntfs_proc_attrseq");
        return TSK_ERR;
    }
    /* Cycle through the list of attributes */
    for (; ((uintptr_t) attr >= (uintptr_t) attrseq)
        && ((uintptr_t) attr <= ((uintptr_t) attrseq + len))
        && (tsk_getu32(fs->endian, attr->len) > 0
            && (tsk_getu32(fs->endian, attr->type) !=
                0xffffffff));
        attr =
        (ntfs_attr *) ((uintptr_t) attr + tsk_getu32(fs->endian,
                attr->len))) {

        UTF16 *name16;
        UTF8 *name8;
        int retVal;
        uint8_t isDefaultData = 0;

        /* Get the type of this attribute */
        uint32_t type = tsk_getu32(fs->endian, attr->type);

        /* Copy the name and convert it to UTF8 */
        if (attr->nlen) {
            int i;

            name8 = (UTF8 *) name;
            name16 =
                (UTF16 *) ((uintptr_t) attr + tsk_getu16(fs->endian,
                    attr->name_off));

            retVal =
                tsk_UTF16toUTF8(fs->endian, (const UTF16 **) &name16,
                (UTF16 *) ((uintptr_t) name16 +
                    attr->nlen * 2),
                &name8,
                (UTF8 *) ((uintptr_t) name8 +
                    sizeof(name)), TSKlenientConversion);

            if (retVal != TSKconversionOK) {
                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "ntfs_proc_attrseq: Error converting NTFS attribute name to UTF8: %d %"
                        PRIuINUM, retVal, fs_file->meta->addr);
                *name = '\0';
            }

            /* Make sure it is NULL Terminated */
            else if ((uintptr_t) name8 > (uintptr_t) name + sizeof(name))
                name[sizeof(name)] = '\0';
            else
                *name8 = '\0';

            /* Clean up name */
            i = 0;
            while (name[i] != '\0') {
                if (TSK_IS_CNTRL(name[i]))
                    name[i] = '^';
                i++;
            }
        }
        /* Call the unnamed $Data attribute, $Data */
        else if (type == NTFS_ATYPE_DATA) {
            name[0] = '$';
            name[1] = 'D';
            name[2] = 'a';
            name[3] = 't';
            name[4] = 'a';
            name[5] = '\0';
            isDefaultData = 1;  // save this so we know later in the loop
        }
        else {
            name[0] = 'N';
            name[1] = '/';
            name[2] = 'A';
            name[3] = '\0';
        }


        /* For resident attributes, we will copy the buffer into
         * a TSK_FS_ATTR buffer, which is stored in the TSK_FS_META
         * structure
         */
        if (attr->res == NTFS_MFT_RES) {
            TSK_FS_ATTR *fs_attr;

            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "ntfs_proc_attrseq: Resident Attribute in %"
                    PRIuINUM " Type: %" PRIu32 " Id: %"
                    PRIu16 " Name: %s\n", ntfs->mnum,
                    type, tsk_getu16(fs->endian, attr->id), name);

            /* Validate the offset lengths */
            if (((tsk_getu16(fs->endian,
                            attr->c.r.soff) + (uintptr_t) attr) >
                    ((uintptr_t) attrseq + len))
                || ((tsk_getu16(fs->endian,
                            attr->c.r.soff) + tsk_getu32(fs->endian,
                            attr->c.r.ssize) + (uintptr_t) attr) >
                    ((uintptr_t) attrseq + len))) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_CORRUPT;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "ntfs_attr_walk: Resident attribute %" PRIuINUM
                    "-%" PRIu32 " starting offset and length too large",
                    fs_file->meta->addr, type);
                return TSK_COR;
            }

            // Get a free fs_attr structure 
            if ((fs_attr =
                    tsk_fs_attrlist_getnew(fs_file->meta->attr,
                        TSK_FS_ATTR_RES)) == NULL) {
                strncat(tsk_errstr2, " - proc_attrseq",
                    TSK_ERRSTR_L - strlen(tsk_errstr2));
                return TSK_ERR;
            }

            // set the details in the fs_attr structure
            if (tsk_fs_attr_set_str(fs_file, fs_attr, name, type,
                    tsk_getu16(fs->endian, attr->id),
                    (void *) ((uintptr_t) attr +
                        tsk_getu16(fs->endian,
                            attr->c.r.soff)), tsk_getu32(fs->endian,
                        attr->c.r.ssize))) {
                strncat(tsk_errstr2, " - proc_attrseq",
                    TSK_ERRSTR_L - strlen(tsk_errstr2));
                return TSK_ERR;
            }

            // set the meta size if we find the relevant attribute 
            if ((fs_file->meta->type == TSK_FS_META_TYPE_DIR)
                && (type == NTFS_ATYPE_IDXROOT)) {
                fs_file->meta->size =
                    tsk_getu32(fs->endian, attr->c.r.ssize);
            }
            else if ((fs_file->meta->type == TSK_FS_META_TYPE_REG)
                && (type == NTFS_ATYPE_DATA) && (isDefaultData)) {
                fs_file->meta->size =
                    tsk_getu32(fs->endian, attr->c.r.ssize);
            }
        }

        /* For non-resident attributes, we will copy the runlist
         * to the generic form and then save it in the TSK_FS_META->attr
         * list
         */
        else {
            TSK_FS_ATTR *fs_attr;
            TSK_FS_ATTR_RUN *fs_attr_run;
            uint8_t data_flag = 0;
            uint16_t id = tsk_getu16(fs->endian, attr->id);
            uint32_t compsize = 0;
            TSK_RETVAL_ENUM retval;

            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "ntfs_proc_attrseq: Non-Resident Attribute in %"
                    PRIuINUM " Type: %" PRIu32 " Id: %"
                    PRIu16 " Name: %s  Start VCN: %"
                    PRIu64 "\n", ntfs->mnum, type, id,
                    name, tsk_getu64(fs->endian, attr->c.nr.start_vcn));

            /* convert the run to generic form */
            retval = ntfs_make_data_run(ntfs,
                tsk_getu64(fs->endian, attr->c.nr.start_vcn),
                (ntfs_runlist *) ((uintptr_t)
                    attr + tsk_getu16(fs->endian,
                        attr->c.nr.run_off)), &fs_attr_run, NULL);
            if (retval != TSK_OK) {
                strncat(tsk_errstr2, " - proc_attrseq",
                    TSK_ERRSTR_L - strlen(tsk_errstr2));
                return retval;
            }

            /* Determine the flags based on compression and stuff */
            data_flag = 0;
            if (tsk_getu16(fs->endian, attr->flags) & NTFS_ATTR_FLAG_COMP) {
                data_flag |= TSK_FS_ATTR_COMP;
                fs_file->meta->flags |= TSK_FS_META_FLAG_COMP;
            }

            if (tsk_getu16(fs->endian, attr->flags) & NTFS_ATTR_FLAG_ENC)
                data_flag |= TSK_FS_ATTR_ENC;

            if (tsk_getu16(fs->endian, attr->flags) & NTFS_ATTR_FLAG_SPAR)
                data_flag |= TSK_FS_ATTR_SPARSE;

            /* SPECIAL CASE 
             * We are in non-res section, so we know this
             * isn't $STD_INFO and $FNAME
             *
             * When we are processing a non-base entry, we may
             * find an attribute with an id of 0 and it is an
             * extention of a previous run (i.e. non-zero start VCN)
             * 
             * We will lookup if we already have such an attribute
             * and get its ID
             *
             * We could also check for a start_vcn if this does
             * not fix the problem
             */
            if (id == 0) {
                int cnt, i;

                // cycle through the attributes
                cnt = tsk_fs_file_attr_getsize(fs_file);
                for (i = 0; i < cnt; i++) {

                    const TSK_FS_ATTR *fs_attr2 =
                        tsk_fs_file_attr_get_idx(fs_file, i);
                    if (!fs_attr2)
                        continue;

                    /* We found an attribute with the same name and type */
                    if ((fs_attr2->type == type) &&
                        (strcmp(fs_attr2->name, name) == 0)) {
                        id = fs_attr2->id;
                        if (tsk_verbose)
                            tsk_fprintf(stderr,
                                "ntfs_proc_attrseq: Updating id from 0 to %"
                                PRIu16 "\n", id);
                        break;
                    }
                }
            }

            /* the compression unit size is stored in the header
             * it is stored as the power of 2 (if it is not 0)
             */
            if (tsk_getu16(fs->endian, attr->c.nr.compusize) > 0) {
                compsize =
                    1 << (tsk_getu16(fs->endian, attr->c.nr.compusize));
            }
            else {
                compsize = 0;
                /* if this is 0, be sure to cancel out the COMP flag.
                 * This occurs when we process an extended attribute
                 * that has compressed data -- the attributes in the
                 * latter MFT entries do not have compsize set.
                 */
                if (data_flag & TSK_FS_ATTR_COMP) {
                    if (tsk_verbose)
                        fprintf(stderr,
                            "ntfs_proc_attrseq: Clearing compression setting for attribute %"
                            PRIuINUM "-%d because compsize is 0\n",
                            fs_file->meta->addr, type);
                    data_flag &= ~TSK_FS_ATTR_COMP;
                }
            }

            /* Add the run to the list */
            // see if this attribute has already been partially defined
            // @@@ This is bad design, we are casting away the const...
            fs_attr =
                (TSK_FS_ATTR *) tsk_fs_attrlist_get_id(fs_file->meta->attr,
                type, id);
            if (fs_attr == NULL) {
                uint64_t ssize; // size
                uint64_t alen;  // allocated length

                if ((fs_attr =
                        tsk_fs_attrlist_getnew(fs_file->meta->attr,
                            TSK_FS_ATTR_RES)) == NULL) {
                    strncat(tsk_errstr2, " - proc_attrseq: getnew",
                        TSK_ERRSTR_L - strlen(tsk_errstr2));
                    return TSK_ERR;
                }

                ssize = tsk_getu64(fs->endian, attr->c.nr.ssize);
                /* This can happen with extended attributes, so
                 * we set it based on what we currently have.
                 * fs_attr_run can be NULL for $BadClust file. */
                if ((ssize == 0) && (fs_attr_run)) {
                    TSK_FS_ATTR_RUN *fs_attr_run_tmp;

                    ssize = fs_attr_run->offset * fs->block_size;
                    fs_attr_run_tmp = fs_attr_run;
                    while (fs_attr_run_tmp) {
                        ssize += (fs_attr_run_tmp->len * fs->block_size);
                        fs_attr_run_tmp = fs_attr_run_tmp->next;
                    }
                }

                // update the meta->size value if this is the default $Data attribute
                if ((fs_file->meta->type == TSK_FS_META_TYPE_REG)
                    && (type == NTFS_ATYPE_DATA) && (isDefaultData)) {
                    fs_file->meta->size = ssize;
                }

                alen = tsk_getu64(fs->endian, attr->c.nr.alen);
                /* This can also happen with extended attributes.
                 * set it to what we know about */
                if (alen == 0) {
                    alen = ssize;
                }

                if (tsk_fs_attr_set_run(fs_file, fs_attr,
                        fs_attr_run, name,
                        type, id, ssize, alen, data_flag, compsize)) {
                    strncat(tsk_errstr2, " - proc_attrseq: set run",
                        TSK_ERRSTR_L - strlen(tsk_errstr2));
                    return TSK_ERR;
                }
                // set the special functions
                if (fs_file->meta->flags & TSK_FS_META_FLAG_COMP) {
                    fs_attr->w = ntfs_attr_walk_special;
                    fs_attr->r = ntfs_file_read_special;
                }

            }
            else {
                if (tsk_fs_attr_add_run(fs, fs_attr, fs_attr_run)) {
                    strncat(tsk_errstr2, " - proc_attrseq: put run",
                        TSK_ERRSTR_L - strlen(tsk_errstr2));
                    return TSK_ERR;
                }
            }
        }


        /* 
         * Special Cases, where we grab additional information
         * regardless if they are resident or not
         */

        /* Standard Information (is always resident) */
        if (type == NTFS_ATYPE_SI) {
            ntfs_attr_si *si;
            if (attr->res != NTFS_MFT_RES) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_INODE_COR;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "proc_attrseq: Standard Information Attribute is not resident!");
                return TSK_COR;
            }
            si = (ntfs_attr_si *) ((uintptr_t) attr +
                tsk_getu16(fs->endian, attr->c.r.soff));
            fs_file->meta->mtime =
                nt2unixtime(tsk_getu64(fs->endian, si->mtime));
            fs_file->meta->atime =
                nt2unixtime(tsk_getu64(fs->endian, si->atime));
            fs_file->meta->ctime =
                nt2unixtime(tsk_getu64(fs->endian, si->ctime));
            fs_file->meta->crtime =
                nt2unixtime(tsk_getu64(fs->endian, si->crtime));
            fs_file->meta->uid = tsk_getu32(fs->endian, si->own_id);
            fs_file->meta->mode |=
                (TSK_FS_META_MODE_IXUSR | TSK_FS_META_MODE_IXGRP |
                TSK_FS_META_MODE_IXOTH);
            if ((tsk_getu32(fs->endian, si->dos) & NTFS_SI_RO) == 0)
                fs_file->meta->mode |=
                    (TSK_FS_META_MODE_IRUSR | TSK_FS_META_MODE_IRGRP |
                    TSK_FS_META_MODE_IROTH);
            if ((tsk_getu32(fs->endian, si->dos) & NTFS_SI_HID) == 0)
                fs_file->meta->mode |=
                    (TSK_FS_META_MODE_IWUSR | TSK_FS_META_MODE_IWGRP |
                    TSK_FS_META_MODE_IWOTH);
        }

        /* File Name (always resident) */
        else if (type == NTFS_ATYPE_FNAME) {
            ntfs_attr_fname *fname;
            TSK_FS_META_NAME_LIST *fs_name;
            UTF16 *name16;
            UTF8 *name8;
            if (attr->res != NTFS_MFT_RES) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_INODE_COR;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "proc_attr_seq: File Name Attribute is not resident!");
                return TSK_COR;
            }
            fname =
                (ntfs_attr_fname *) ((uintptr_t) attr +
                tsk_getu16(fs->endian, attr->c.r.soff));
            if (fname->nspace == NTFS_FNAME_DOS) {
                continue;
            }

            /* Seek to the end of the fs_name structures in TSK_FS_META */
            if (fs_file->meta->name2) {
                for (fs_name = fs_file->meta->name2;
                    (fs_name) && (fs_name->next != NULL);
                    fs_name = fs_name->next) {
                }

                /* add to the end of the existing list */
                fs_name->next = (TSK_FS_META_NAME_LIST *)
                    tsk_malloc(sizeof(TSK_FS_META_NAME_LIST));
                if (fs_name->next == NULL) {
                    return TSK_ERR;
                }
                fs_name = fs_name->next;
                fs_name->next = NULL;
            }
            else {
                /* First name, so we start a list */
                fs_file->meta->name2 = fs_name = (TSK_FS_META_NAME_LIST *)
                    tsk_malloc(sizeof(TSK_FS_META_NAME_LIST));
                if (fs_name == NULL) {
                    return TSK_ERR;
                }
                fs_name->next = NULL;
            }

            name16 = (UTF16 *) & fname->name;
            name8 = (UTF8 *) fs_name->name;
            retVal =
                tsk_UTF16toUTF8(fs->endian, (const UTF16 **) &name16,
                (UTF16 *) ((uintptr_t) name16 +
                    fname->nlen * 2),
                &name8,
                (UTF8 *) ((uintptr_t) name8 +
                    sizeof(fs_name->name)), TSKlenientConversion);
            if (retVal != TSKconversionOK) {
                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "proc_attr_seq: Error converting NTFS name in $FNAME to UTF8: %d",
                        retVal);
                *name8 = '\0';
            }
            /* Make sure it is NULL Terminated */
            else if ((uintptr_t) name8 >
                (uintptr_t) fs_name->name + sizeof(fs_name->name))
                fs_name->name[sizeof(fs_name->name)] = '\0';
            else
                *name8 = '\0';

            fs_name->par_inode = tsk_getu48(fs->endian, fname->par_ref);
            fs_name->par_seq = tsk_getu16(fs->endian, fname->par_seq);
        }

        /* If this is an attribute list than we need to process
         * it to get the list of other entries to read.  But, because
         * of the wierd scenario of the $MFT having an attribute list
         * and not knowing where the other MFT entires are yet, we wait 
         * until the end of the attrseq to processes the list and then
         * we should have the $Data attribute loaded
         */
        else if (type == NTFS_ATYPE_ATTRLIST) {
            if (fs_attr_attrl) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_UNSUPFUNC;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "Multiple instances of attribute lists in the same MFT\n"
                    "I didn't realize that could happen, contact the developers");
                return TSK_ERR;
            }
            fs_attr_attrl = tsk_fs_attrlist_get_id(fs_file->meta->attr,
                NTFS_ATYPE_ATTRLIST, tsk_getu16(fs->endian, attr->id));
            if (fs_attr_attrl == NULL) {
                strncat(tsk_errstr2,
                    " - proc_attrseq: getting attribute list",
                    TSK_ERRSTR_L - strlen(tsk_errstr2));
                return TSK_ERR;
            }
        }
    }


    /* Are we currently in the process of loading $MFT? */
    if (ntfs->loading_the_MFT == 1) {

        /* If we don't even have a mini cached version, get it now 
         * Even if we are not done because of attribute lists, then we
         * should at least have the head of the list 
         */
        if (!ntfs->mft_data) {
            int cnt, i;

            // cycle through the attributes
            cnt = tsk_fs_file_attr_getsize(fs_file);
            for (i = 0; i < cnt; i++) {
                const TSK_FS_ATTR *fs_attr =
                    tsk_fs_file_attr_get_idx(fs_file, i);
                if (!fs_attr)
                    continue;

                if ((fs_attr->type == NTFS_ATYPE_DATA) &&
                    (strcmp(fs_attr->name, "$Data") == 0)) {
                    ntfs->mft_data = fs_attr;
                    break;
                }
            }

            // @@@ Is this needed here -- maybe it should be only in _open
            if (!ntfs->mft_data) {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_GENFS;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "$Data not found while loading the MFT");
                return TSK_ERR;
            }
        }

        /* Update the inode count based on the current size 
         * IF $MFT has an attribute list, this value will increase each
         * time
         */
        fs->inum_count = ntfs->mft_data->size / ntfs->mft_rsize_b;
        fs->last_inum = fs->inum_count - 1;
    }

    /* If there was an attribute list, process it now, we wait because
     * the list can contain MFT entries that are described in $Data
     * of this MFT entry.  For example, part of the $DATA attribute
     * could follow the ATTRLIST entry, so we read it first and then 
     * process the attribute list
     */
    if (fs_attr_attrl) {
        if (ntfs_proc_attrlist(ntfs, fs_file, fs_attr_attrl)) {
            return TSK_ERR;
        }
    }

    fs_file->meta->attr_state = TSK_FS_META_ATTR_STUDIED;
    return TSK_OK;
}



/********   Attribute List Action and Function ***********/


/*
 * Attribute lists are used when all of the attribute  headers can not
 * fit into one MFT entry.  This contains an entry for every attribute
 * and where they are located.  We process this to get the locations
 * and then call proc_attrseq on each of those, which adds the data
 * to the fs_file structure.
 *
 * Return 1 on error and 0 on success
 */
static uint8_t
ntfs_proc_attrlist(NTFS_INFO * ntfs,
    TSK_FS_FILE * fs_file, const TSK_FS_ATTR * fs_attr_attrlist)
{
    ntfs_attrlist *list;
    char *buf;
    uintptr_t endaddr;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ntfs->fs_info;
    ntfs_mft *mft;
    TSK_FS_LOAD_FILE load_file;
    TSK_INUM_T hist[256];
    uint16_t histcnt = 0;

    if (tsk_verbose)
        tsk_fprintf(stderr,
            "ntfs_proc_attrlist: Processing entry %"
            PRIuINUM "\n", fs_file->meta->addr);

    if ((mft = (ntfs_mft *) tsk_malloc(ntfs->mft_rsize_b)) == NULL) {
        return 1;
    }

    /* Clear the contents of the history buffer */
    memset(hist, 0, sizeof(hist));

    /* add ourselves to the history */
    hist[histcnt++] = ntfs->mnum;

    /* Get a copy of the attribute list stream using the above action */
    load_file.left = load_file.total = (size_t) fs_attr_attrlist->size;
    load_file.base = load_file.cur = buf =
        tsk_malloc((size_t) fs_attr_attrlist->size);
    if (buf == NULL) {
        free(mft);
        return 1;
    }
    endaddr = (uintptr_t) buf + (uintptr_t) fs_attr_attrlist->size;
    if (tsk_fs_attr_walk(fs_attr_attrlist, 0, tsk_fs_load_file_action,
            (void *) &load_file)) {
        strncat(tsk_errstr2, " - processing attrlist",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        free(mft);
        return 1;
    }

    /* this value should be zero, if not then we didn't read all of the
     * buffer
     */
    if (load_file.left > 0) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_FWALK;
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "processing attrlist of entry %" PRIuINUM, ntfs->mnum);
        free(mft);
        free(buf);
        return 1;
    }


    /* Process the list & and call ntfs_proc_attr */
    for (list = (ntfs_attrlist *) buf;
        (list) && ((uintptr_t) list < endaddr)
        && (tsk_getu16(fs->endian, list->len) > 0);
        list =
        (ntfs_attrlist *) ((uintptr_t) list + tsk_getu16(fs->endian,
                list->len))) {
        TSK_INUM_T mftnum;
        uint32_t type;
        uint16_t id, i;
        TSK_RETVAL_ENUM retval;

        /* Which MFT is this attribute in? */
        mftnum = tsk_getu48(fs->endian, list->file_ref);
        /* Check the history to see if we have already processed this
         * one before (if we have then we can skip it as we grabbed all
         * of them last time
         */
        for (i = 0; i < histcnt; i++) {
            if (hist[i] == mftnum)
                break;
        }

        if (hist[i] == mftnum)
            continue;
        /* This is a new one, add it to the history, and process it */
        if (histcnt < 256)
            hist[histcnt++] = mftnum;
        type = tsk_getu32(fs->endian, list->type);
        id = tsk_getu16(fs->endian, list->id);
        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ntfs_proc_attrlist: mft: %" PRIuINUM
                " type %" PRIu32 " id %" PRIu16
                "  VCN: %" PRIu64 "\n", mftnum, type,
                id, tsk_getu64(fs->endian, list->start_vcn));
        /* 
         * Read the MFT entry 
         */
        /* Sanity check. */
        if (mftnum < ntfs->fs_info.first_inum ||
            mftnum > ntfs->fs_info.last_inum) {

            if (tsk_verbose) {
                /* this case can easily occur if the attribute list was non-resident and the cluster has been reallocated */

                tsk_fprintf(stderr,
                    "Invalid MFT file reference (%"
                    PRIuINUM
                    ") in the unallocated attribute list of MFT %"
                    PRIuINUM "", mftnum, ntfs->mnum);
            }

            continue;
        }

        if ((retval = ntfs_dinode_lookup(ntfs, mft, mftnum)) != TSK_OK) {
            // if the entry is corrupt, then continue
            if (retval == TSK_COR) {
                if (tsk_verbose)
                    tsk_error_print(stderr);
                tsk_error_reset();
                continue;
            }

            free(mft);
            free(buf);
            strncat(tsk_errstr2, " - proc_attrlist",
                TSK_ERRSTR_L - strlen(tsk_errstr2));
            return 1;
        }

        /* verify that this entry refers to the original one */
        if (tsk_getu48(fs->endian, mft->base_ref) != ntfs->mnum) {

            /* Before we raise alarms, check if the original was
             * unallocated.  If so, then the list entry could 
             * have been reallocated, so we will just ignore it
             */
            if ((tsk_getu16(fs->endian,
                        ntfs->mft->flags) & NTFS_MFT_INUSE) == 0) {
                continue;
            }
            else {
                tsk_error_reset();
                tsk_errno = TSK_ERR_FS_INODE_COR;
                snprintf(tsk_errstr, TSK_ERRSTR_L,
                    "Extension record %" PRIuINUM
                    " (file ref = %" PRIuINUM
                    ") is not for attribute list of %"
                    PRIuINUM "", mftnum, tsk_getu48(fs->endian,
                        mft->base_ref), ntfs->mnum);
                free(mft);
                free(buf);
                return 1;
            }
        }
        /* 
         * Process the attribute seq for this MFT entry and add them
         * to the TSK_FS_META structure
         */

        if ((retval =
                ntfs_proc_attrseq(ntfs, fs_file, (ntfs_attr *) ((uintptr_t)
                        mft + tsk_getu16(fs->endian, mft->attr_off)),
                    ntfs->mft_rsize_b - tsk_getu16(fs->endian,
                        mft->attr_off))) != TSK_OK) {

            if (retval == TSK_COR) {
                if (tsk_verbose)
                    tsk_error_print(stderr);
                tsk_error_reset();
                continue;
            }
            strncat(tsk_errstr2, "- proc_attrlist",
                TSK_ERRSTR_L - strlen(tsk_errstr2));
            free(mft);
            free(buf);
            return 1;
        }
    }

    free(mft);
    free(buf);
    return 0;
}



/**
 * Copy the MFT entry saved in ntfs->mft into the generic structure.
 * 
 * @param ntfs File system structure that contains entry to copy
 * @param fs_file Structure to copy processed data to.
 * 
 * @returns error code
 */
static TSK_RETVAL_ENUM
ntfs_dinode_copy(NTFS_INFO * ntfs, TSK_FS_FILE * a_fs_file)
{
    ntfs_mft *mft = ntfs->mft;
    ntfs_attr *attr;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ntfs->fs_info;
    TSK_RETVAL_ENUM retval;

    if ((a_fs_file == NULL) || (a_fs_file->meta == NULL)) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_dinode_copy: NULL fs_file given");
        return TSK_ERR;
    }

    /* if the attributes list has been used previously, then make sure the 
     * flags are cleared 
     */
    if (a_fs_file->meta->attr) {
        tsk_fs_attrlist_markunused(a_fs_file->meta->attr);
    }
    else {
        a_fs_file->meta->attr = tsk_fs_attrlist_alloc();
        if (a_fs_file->meta->attr == NULL)
            return TSK_ERR;
    }
    a_fs_file->meta->attr_state = TSK_FS_META_ATTR_EMPTY;

    /* If there are any name structures allocated, then free 'em */
    if (a_fs_file->meta->name2) {
        TSK_FS_META_NAME_LIST *fs_name1, *fs_name2;
        fs_name1 = a_fs_file->meta->name2;

        while (fs_name1) {
            fs_name2 = fs_name1->next;
            free(fs_name1);
            fs_name1 = fs_name2;
        }
        a_fs_file->meta->name2 = NULL;
    }

    /* Set the a_fs_file->meta values from mft */
    a_fs_file->meta->nlink = tsk_getu16(fs->endian, mft->link);
    a_fs_file->meta->seq = tsk_getu16(fs->endian, mft->seq);
    a_fs_file->meta->addr = ntfs->mnum;

    /* Set the mode for file or directory */
    if (tsk_getu16(fs->endian, ntfs->mft->flags) & NTFS_MFT_DIR)
        a_fs_file->meta->type = TSK_FS_META_TYPE_DIR;
    else
        a_fs_file->meta->type = TSK_FS_META_TYPE_REG;
    a_fs_file->meta->mode = 0;  // will be set by proc_attrseq

    /* the following will be changed once we find the correct attribute,
     * but initialize them now just in case 
     */
    a_fs_file->meta->uid = 0;
    a_fs_file->meta->gid = 0;
    a_fs_file->meta->size = 0;
    a_fs_file->meta->mtime = 0;
    a_fs_file->meta->atime = 0;
    a_fs_file->meta->ctime = 0;
    a_fs_file->meta->crtime = 0;

    /* add the flags */
    a_fs_file->meta->flags =
        ((tsk_getu16(fs->endian, ntfs->mft->flags) &
            NTFS_MFT_INUSE) ? TSK_FS_META_FLAG_ALLOC :
        TSK_FS_META_FLAG_UNALLOC);

    /* Process the attribute sequence to fill in the fs_meta->attr
     * list and the other info such as size and times
     */
    attr =
        (ntfs_attr *) ((uintptr_t) mft + tsk_getu16(fs->endian,
            mft->attr_off));
    if ((retval = ntfs_proc_attrseq(ntfs, a_fs_file, attr,
                ntfs->mft_rsize_b - tsk_getu16(fs->endian,
                    mft->attr_off))) != TSK_OK) {
        return retval;
    }

    /* The entry has been 'used' if it has attributes */
    if (a_fs_file->meta->attr != NULL)
        a_fs_file->meta->flags |= TSK_FS_META_FLAG_USED;
    else
        a_fs_file->meta->flags |= TSK_FS_META_FLAG_UNUSED;

    return 0;
}


/**
 * Read the mft entry and put it into the local ntfs->mft structure
 * Also sets the ntfs->mnum value. 
 *
 * @param ntfs NTFS file system to get inode from
 * @param mftnum Address of inode to copy
 * @returns TSK_OK, TSK_CORR, or TSK_ERR
 */
static TSK_RETVAL_ENUM
ntfs_dinode_load(NTFS_INFO * a_ntfs, TSK_INUM_T a_mftnum)
{
    TSK_RETVAL_ENUM retval;

    /* mft_lookup does a sanity check, so we can skip it here */
    if ((retval =
            ntfs_dinode_lookup(a_ntfs, a_ntfs->mft, a_mftnum)) != TSK_OK)
        return retval;
    a_ntfs->mnum = a_mftnum;
    return 0;
}


/** \internal
 * Load the attributes.  In NTFS, the attributes are already loaded
 * so return error values based on current state.
 * @param a_fs_file File to load attributes for.
 * @returns 1 on error
 */
static uint8_t
ntfs_load_attrs(TSK_FS_FILE * a_fs_file)
{
    if ((a_fs_file == NULL) || (a_fs_file->meta == NULL)) {
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_load_attrs: called with NULL pointers");
        return 1;
    }

    /* Verify the file has attributes */
    if (a_fs_file->meta->attr == NULL) {
        if (a_fs_file->meta->flags & TSK_FS_META_FLAG_UNALLOC)
            tsk_errno = TSK_ERR_FS_RECOVER;
        else
            tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_load_attrs: attributes are NULL");
        return 1;
    }
    return 0;
}

/**
 * Read an MFT entry and save it in the generic TSK_FS_META format.
 * 
 * @param fs File system to read from.
 * @param mftnum Address of mft entry to read
 * @returns 1 on error 
 */
static uint8_t
ntfs_inode_lookup(TSK_FS_INFO * fs, TSK_FS_FILE * a_fs_file,
    TSK_INUM_T mftnum)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if (a_fs_file == NULL) {
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_inode_lookup: fs_file is NULL");
        return 1;
    }

    if (a_fs_file->meta == NULL) {
        a_fs_file->meta = tsk_fs_meta_alloc(NTFS_FILE_CONTENT_LEN);
        if (a_fs_file->meta == NULL)
            return 1;
    }
    else {
        tsk_fs_meta_reset(a_fs_file->meta);
    }

    // see if they are looking for the special "orphans" directory
    if (mftnum == TSK_FS_ORPHANDIR_INUM(fs)) {
        if (tsk_fs_dir_make_orphan_dir_meta(fs, a_fs_file->meta))
            return 1;
        else
            return 0;
    }

    /* Lookup inode and store it in the ntfs structure */
    if (ntfs_dinode_load(ntfs, mftnum) != TSK_OK) {
        return 1;
    }

    /* Copy the structure in ntfs to generic a_fs_file->meta */
    if (ntfs_dinode_copy(ntfs, a_fs_file) != TSK_OK) {
        return 1;
    }

    return 0;
}




/**********************************************************************
 *
 *  Load special MFT structures into the NTFS_INFO structure
 *
 **********************************************************************/

/* The attrdef structure defines the types of attributes and gives a 
 * name value to the type number.
 *
 * We currently do not use this during the analysis (Because it has not
 * historically changed, but we do display it in fsstat 
 *
 * Return 1 on error and 0 on success
 */
static uint8_t
ntfs_load_attrdef(NTFS_INFO * ntfs)
{
    TSK_FS_FILE *fs_file;
    const TSK_FS_ATTR *fs_attr;
    TSK_FS_INFO *fs = &ntfs->fs_info;
    TSK_FS_LOAD_FILE load_file;

    /* if already loaded, return now */
    if (ntfs->attrdef)
        return 1;

    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, NTFS_MFT_ATTR)) == NULL)
        return 1;

    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_DATA);
    if (!fs_attr) {
        //("Data attribute not found in $Attr");
        tsk_fs_file_close(fs_file);
        return 1;
    }

// @@@ We need to do a sanity check on the size of fs_attr->size

    /* Get a copy of the attribute list stream using the above action */
    load_file.left = load_file.total = (size_t) fs_attr->size;
    load_file.base = load_file.cur = tsk_malloc((size_t) fs_attr->size);
    if (load_file.cur == NULL) {
        tsk_fs_file_close(fs_file);
        return 1;
    }
    ntfs->attrdef = (ntfs_attrdef *) load_file.base;

    if (tsk_fs_attr_walk(fs_attr,
            0, tsk_fs_load_file_action, (void *) &load_file)) {
        strncat(tsk_errstr2, " - load_attrdef",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        tsk_fs_file_close(fs_file);
        free(ntfs->attrdef);
        ntfs->attrdef = NULL;
        return 1;
    }
    else if (load_file.left > 0) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_FWALK;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "load_attrdef: space still left after walking $Attr data");
        tsk_fs_file_close(fs_file);
        free(ntfs->attrdef);
        ntfs->attrdef = NULL;
        return 1;
    }

    ntfs->attrdef_len = (size_t) fs_attr->size;
    tsk_fs_file_close(fs_file);
    return 0;
}


/* 
 * return the name of the attribute type.  If the attribute has not
 * been loaded yet, it will be.
 *
 * Return 1 on error and 0 on success
 */
uint8_t
ntfs_attrname_lookup(TSK_FS_INFO * fs, uint16_t type, char *name, int len)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    ntfs_attrdef *attrdef;
    if (!ntfs->attrdef) {
        if (ntfs_load_attrdef(ntfs))
            return 1;
    }

    attrdef = ntfs->attrdef;
    while (
        (((uintptr_t) attrdef - (uintptr_t) ntfs->attrdef +
                sizeof(ntfs_attrdef)) < ntfs->attrdef_len) &&
        (tsk_getu32(fs->endian, attrdef->type))) {
        if (tsk_getu32(fs->endian, attrdef->type) == type) {

            UTF16 *name16 = (UTF16 *) attrdef->label;
            UTF8 *name8 = (UTF8 *) name;
            int retVal;
            retVal =
                tsk_UTF16toUTF8(fs->endian, (const UTF16 **) &name16,
                (UTF16 *) ((uintptr_t) name16 +
                    sizeof(attrdef->
                        label)),
                &name8,
                (UTF8 *) ((uintptr_t) name8 + len), TSKlenientConversion);
            if (retVal != TSKconversionOK) {
                if (tsk_verbose)
                    tsk_fprintf(stderr,
                        "attrname_lookup: Error converting NTFS attribute def label to UTF8: %d",
                        retVal);
                break;
            }

            /* Make sure it is NULL Terminated */
            else if ((uintptr_t) name8 > (uintptr_t) name + len)
                name[len] = '\0';
            else
                *name8 = '\0';
            return 0;
        }
        attrdef++;
    }
    /* If we didn't find it, then call it '?' */
    snprintf(name, len, "?");
    return 0;
}


/* Load the block bitmap $Data run  and allocate a buffer for a cache 
 *
 * return 1 on error and 0 on success
 * */
static uint8_t
ntfs_load_bmap(NTFS_INFO * ntfs)
{
    ssize_t cnt;
    ntfs_attr *attr;
    TSK_FS_INFO *fs = &ntfs->fs_info;

    /* Get data on the bitmap */
    if (ntfs_dinode_load(ntfs, NTFS_MFT_BMAP) != TSK_OK) {
        return 1;
    }

    attr = (ntfs_attr *) ((uintptr_t) ntfs->mft +
        tsk_getu16(fs->endian, ntfs->mft->attr_off));

    /* cycle through them */
    while (((uintptr_t) attr >= (uintptr_t) ntfs->mft)
        && ((uintptr_t) attr <=
            ((uintptr_t) ntfs->mft + (uintptr_t) ntfs->mft_rsize_b))
        && (tsk_getu32(fs->endian, attr->len) > 0
            && (tsk_getu32(fs->endian, attr->type) != 0xffffffff)
            && (tsk_getu32(fs->endian, attr->type) != NTFS_ATYPE_DATA))) {
        attr =
            (ntfs_attr *) ((uintptr_t) attr + tsk_getu32(fs->endian,
                attr->len));
    }

    /* did we get it? */
    if (tsk_getu32(fs->endian, attr->type) != NTFS_ATYPE_DATA) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Error Finding Bitmap Data Attribute");
        return 1;
    }

    /* convert to generic form */
    if ((ntfs_make_data_run(ntfs,
                tsk_getu64(fs->endian, attr->c.nr.start_vcn),
                (ntfs_runlist
                    *) ((uintptr_t) attr + tsk_getu16(fs->endian,
                        attr->c.nr.run_off)), &(ntfs->bmap),
                NULL)) != TSK_OK) {
        return 1;
    }

    ntfs->bmap_buf = (char *) tsk_malloc(fs->block_size);
    if (ntfs->bmap_buf == NULL) {
        return 1;
    }

    /* Load the first cluster so that we have something there */
    ntfs->bmap_buf_off = 0;
    if (ntfs->bmap->addr > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_GENFS;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_load_bmap: Bitmap too large for image size: %"
            PRIuDADDR "", ntfs->bmap->addr);
        return 1;
    }
    cnt =
        tsk_fs_read_block(fs,
        ntfs->bmap->addr, ntfs->bmap_buf, fs->block_size);
    if (cnt != fs->block_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "ntfs_load_bmap: Error reading block at %"
            PRIuDADDR, ntfs->bmap->addr);
        return 1;
    }
    return 0;
}


/*
 * Load the VOLUME MFT entry and the VINFO attribute so that we
 * can identify the volume version of this.  
 *
 * Return 1 on error and 0 on success
 */
static uint8_t
ntfs_load_ver(NTFS_INFO * ntfs)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ntfs->fs_info;
    TSK_FS_FILE *fs_file;
    const TSK_FS_ATTR *fs_attr;

    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, NTFS_MFT_VOL)) == NULL) {
        return 1;
    }

    /* cache the data attribute */
    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_VINFO);
    if (!fs_attr) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Volume Info attribute not found in $Volume");
        tsk_fs_file_close(fs_file);
        return 1;
    }

    if ((fs_attr->flags & TSK_FS_ATTR_RES)
        && (fs_attr->size)) {
        ntfs_attr_vinfo *vinfo = (ntfs_attr_vinfo *) fs_attr->rd.buf;

        if ((vinfo->maj_ver == 1)
            && (vinfo->min_ver == 2)) {
            ntfs->ver = NTFS_VINFO_NT;
        }
        else if ((vinfo->maj_ver == 3)
            && (vinfo->min_ver == 0)) {
            ntfs->ver = NTFS_VINFO_2K;
        }
        else if ((vinfo->maj_ver == 3)
            && (vinfo->min_ver == 1)) {
            ntfs->ver = NTFS_VINFO_XP;
        }
        else {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_GENFS;
            snprintf(tsk_errstr, TSK_ERRSTR_L,
                "unknown version: %d.%d\n",
                vinfo->maj_ver, vinfo->min_ver);
            tsk_fs_file_close(fs_file);
            return 1;
        }
    }
    else {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_GENFS;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "load_version: VINFO is a non-resident attribute");
        return 1;
    }

    tsk_fs_file_close(fs_file);
    return 0;
}



#if TSK_USE_SID

/* 
 * Process the SDS entry from the $Secure File and save to
 * the sid structure in NTFS_INFO
 *
 * Return 1 on error and 0 on success 
 */
static int
ntfs_load_sid(TSK_FS_INFO * fs, ntfs_attr_sds * sds)
{
    char *sid_str = NULL;
    ntfs_sid *sid = NULL;

    // "S-"
    unsigned int sid_str_len = 2;
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    unsigned int owner_offset =
        tsk_getu32(fs->endian, sds->self_rel_sec_desc.owner);

    if (((uintptr_t) & sds->self_rel_sec_desc + owner_offset) >
        ((uintptr_t) sds + tsk_getu32(fs->endian, sds->ent_size))) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_load_sid: owner offset larger than sds length");
        return 1;
    }

    sid =
        (ntfs_sid *) ((uint8_t *) & sds->self_rel_sec_desc + owner_offset);

    // "1-"
    sid_str_len += 2;
    //tsk_fprintf(stderr, "Revision: %i\n", sid->revision);

    // This check helps not process invalid data, which was noticed while testing
    // a failing harddrive
    if (sid->revision == 1) {
        NTFS_SID_ENTRY *sid_entry;
        int index;
        int len;
        uint64_t authority;
        int i, j;
        char *sid_str_offset;

        //tsk_fprintf(stderr, "Sub-Authority Count: %i\n", sid->sub_auth_count);

        for (authority = i = 0, j = 40; i < 6; i++, j -= 8)
            authority += (uint64_t) sid->ident_auth[i] << j;

        //tsk_fprintf(stderr, "NT Authority: %" PRIu64 "\n", authority);

        // "-XXXXXXXXXX"
        sid_str_len += (1 + 10) * sid->sub_auth_count;

        if ((sid_str = (char *) tsk_malloc(sid_str_len)) == NULL) {
            return 1;
        }

        len = sprintf(sid_str, "S-1-%" PRIu64, authority);
        sid_str_offset = sid_str + len;

        for (index = 0; index < sid->sub_auth_count; index++) {
            len =
                sprintf(sid_str_offset, "-%" PRIu32, sid->sub_auth[index]);
            sid_str_offset += len;
        }
        //tsk_fprintf(stderr, "SID: %s\n\n", sid_str);

        if ((sid_entry =
                (NTFS_SID_ENTRY *) tsk_malloc(sizeof(NTFS_SID_ENTRY))) ==
            NULL) {
            free(sid_str);
            return 1;
        }

        // malloc size of ntfs_sid plus extra for each sub_auth_count above 1 because
        // 1 is already expected as a minimum in the ntfs_sid struct.
        if ((sid_entry->data =
                (ntfs_sid *) tsk_malloc(sizeof(ntfs_sid) +
                    ((int) sid->sub_auth_count -
                        1) * (sizeof(uint32_t) * 10))) == NULL) {
            free(sid_str);
            free(sid_entry);
            return 1;
        }

        memcpy(sid_entry->data, sid,
            sizeof(ntfs_sid) + ((int) sid->sub_auth_count -
                1) * sizeof(uint32_t));
        sid_entry->sid_str = sid_str;
        sid_entry->sec_id = tsk_getu32(fs->endian, sds->sec_id);
        sid_entry->next = NULL;

        if (ntfs->sid == NULL) {
            ntfs->sid = sid_entry;
        }
        else {
            NTFS_SID_ENTRY *sid_tmp;
            sid_tmp = ntfs->sid;
            while (sid_tmp) {
                if (sid_tmp->next == NULL) {
                    sid_tmp->next = sid_entry;
                    break;
                }
                sid_tmp = sid_tmp->next;
            }
        }
    }
    return 0;
}


/* Process the $SDS attribute in the $Secure file and load results
 * into sds structure in NTFS_INFO
 *
 * return 1 on error and 0 on success
 * */
static int
ntfs_proc_sds(TSK_FS_INFO * fs, NTFS_SXX_BUFFER * sds_buffer)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;

    unsigned int ent_len = 0;
    unsigned int total_bytes_processed = 0;
    uint64_t current_offset = 0;

    NTFS_SDS_ENTRY *previous_sds_entry = NULL;
    ntfs_attr_sds *sds = (ntfs_attr_sds *) sds_buffer->buffer;


    while (total_bytes_processed < sds_buffer->size) {
        current_offset = (uintptr_t) sds - (uintptr_t) sds_buffer->buffer;

        ent_len = tsk_getu32(fs->endian, sds->ent_size);
        if (ent_len % 16) {
            ent_len = ((ent_len / 16) + 1) * 16;
        }

        // verify the length value is valid 
        if ((ent_len > 0) && (current_offset + ent_len < sds_buffer->size)
            && (tsk_getu64(fs->endian, sds->file_off) < sds_buffer->size)) {

            NTFS_SDS_ENTRY *sds_entry;

            if ((sds_entry =
                    (NTFS_SDS_ENTRY *) tsk_malloc(sizeof(NTFS_SDS_ENTRY)))
                == NULL) {
                return 1;
            }
            if ((sds_entry->data =
                    (uint8_t *) tsk_malloc(ent_len)) == NULL) {
                free(sds_entry);
                return 1;
            }
            memcpy(sds_entry->data, sds, ent_len);
            sds_entry->len = ent_len;
            sds_entry->next = NULL;

            if (previous_sds_entry == NULL) {
                ntfs->sds = sds_entry;
            }
            else {
                previous_sds_entry->next = sds_entry;
            }
            previous_sds_entry = sds_entry;

            if (ntfs_load_sid(fs, sds)) {
                return 1;
            }

            sds = (ntfs_attr_sds *) ((uint8_t *) sds + ent_len);
            total_bytes_processed += ent_len;
        }
        else {
            total_bytes_processed =
                ((total_bytes_processed / NTFS_SDS_BLOCK_OFFSET) +
                1) * NTFS_SDS_BLOCK_OFFSET;
            sds =
                (ntfs_attr_sds *) ((uint8_t *) sds_buffer->buffer +
                total_bytes_processed);
        }
    }

    return 0;
}


#if 0
void
ntfs_load_sii(TSK_FS_INFO * fs, NTFS_SXX_BUFFER * sii_buffer)
{
    unsigned int total_bytes_processed = 0;
    unsigned int idx_buffer_length = 0;
    unsigned int sii_buffer_offset = 0;

    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    NTFS_SII_ENTRY *previous_sii_entry = NULL;

    /* Loop by cluster size */
    for (sii_buffer_offset = 0; sii_buffer_offset < sii_buffer->size;
        sii_buffer_offset += ntfs->csize_b) {
        ntfs_idxrec *idxrec =
            (ntfs_idxrec *) & sii_buffer->buffer[sii_buffer_offset];

        idx_buffer_length = tsk_getu32(fs->endian, idxrec->list.buf_off);

        ntfs_attr_sii *sii =
            (ntfs_attr_sii *) ((uintptr_t) & idxrec->list +
            tsk_getu32(fs->endian,
                idxrec->list.begin_off));

        total_bytes_processed =
            (uint8_t) ((uintptr_t) sii - (uintptr_t) idxrec);

        do {
            NTFS_SII_ENTRY *sii_entry;

            if ((sii_entry =
                    (NTFS_SII_ENTRY *) tsk_malloc(sizeof(NTFS_SII_ENTRY)))
                == NULL) {
                return 1;
            }

            if ((sii_entry->data =
                    (ntfs_attr_sii *) tsk_malloc(sizeof(ntfs_attr_sii))) ==
                NULL) {
                free(sii_entry);
                return 1;
            }
            memcpy(sii_entry->data, sii, sizeof(ntfs_attr_sii));

            if (previous_sii_entry == NULL) {
                previous_sii_entry = sii_entry;
                previous_sii_entry->next = NULL;
                ntfs->sii = previous_sii_entry;
            }
            else {
                previous_sii_entry->next = sii_entry;
                previous_sii_entry = previous_sii_entry->next;
                previous_sii_entry->next = NULL;
            }

            sii++;
            total_bytes_processed += sizeof(ntfs_attr_sii);
        } while (total_bytes_processed + sizeof(ntfs_attr_sii) <=
            idx_buffer_length);
    }
}



void
ntfs_load_sdh(TSK_FS_INFO * fs, NTFS_SXX_BUFFER * sdh_buffer)
{
    unsigned int total_bytes_processed = 0;
    unsigned int idx_buffer_length = 0;
    unsigned int sdh_buffer_offset = 0;

    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    NTFS_SDH_ENTRY *previous_sdh_entry = NULL;

    /* Loop by cluster size */
    for (sdh_buffer_offset = 0; sdh_buffer_offset < sdh_buffer->size;
        sdh_buffer_offset += ntfs->csize_b) {
        ntfs_idxrec *idxrec =
            (ntfs_idxrec *) & sdh_buffer->buffer[sdh_buffer_offset];

        idx_buffer_length = tsk_getu32(fs->endian, idxrec->list.buf_off);

        ntfs_attr_sdh *sdh =
            (ntfs_attr_sdh *) ((uintptr_t) & idxrec->list +
            tsk_getu32(fs->endian,
                idxrec->list.begin_off));

        total_bytes_processed =
            (uint8_t) ((uintptr_t) sdh - (uintptr_t) idxrec);

        do {
            NTFS_SDH_ENTRY *sdh_entry =
                (NTFS_SDH_ENTRY *) malloc(sizeof(NTFS_SDH_ENTRY));
            sdh_entry->data =
                (ntfs_attr_sdh *) malloc(sizeof(ntfs_attr_sdh));
            memcpy(sdh_entry->data, sdh, sizeof(ntfs_attr_sdh));

            if (previous_sdh_entry == NULL) {
                previous_sdh_entry = sdh_entry;
                previous_sdh_entry->next = NULL;
                ntfs->sdh = previous_sdh_entry;
            }
            else {
                previous_sdh_entry->next = sdh_entry;
                previous_sdh_entry = previous_sdh_entry->next;
                previous_sdh_entry->next = NULL;
            }

            sdh++;
            total_bytes_processed += sizeof(ntfs_attr_sdh);
            //tsk_fprintf(stderr, "total_bytes_processed: %u\tidx_buffer_length: %u\n", total_bytes_processed, idx_buffer_length);
        } while (total_bytes_processed + sizeof(ntfs_attr_sdh) <=
            idx_buffer_length);
    }
}
#endif

uint8_t
ntfs_load_sxx_buffer(TSK_FS_INFO * fs, TSK_DADDR_T addr, char *buf,
    size_t size, TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr)
{
    NTFS_SXX_BUFFER *sxx_buffer = (NTFS_SXX_BUFFER *) ptr;

    if (sxx_buffer->used + size > sxx_buffer->size) {
        sxx_buffer->size = sxx_buffer->used + 8 * size; //this should never happen..
        if ((sxx_buffer->buffer =
                (char *) tsk_realloc(sxx_buffer->buffer,
                    sxx_buffer->size)) == NULL) {
            return TSK_WALK_ERROR;
        }
    }

    memcpy(&(sxx_buffer->buffer[sxx_buffer->used]), buf, size);
    sxx_buffer->used += size;
    return TSK_WALK_CONT;
}


/*
 * Load the $Secure attributes so that we can identify the user.
 */
static int
ntfs_load_secure(NTFS_INFO * ntfs)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ntfs->fs_info;
    TSK_FS_META *fs_meta;
    TSK_FS_ATTR *fs_attr;
    NTFS_SXX_BUFFER sds_buffer;

    ntfs->sds = NULL;
    ntfs->sid = NULL;
    //ntfs->sdh = NULL;
    //ntfs->sii = NULL;

    fs_meta = ntfs_inode_lookup(fs, NTFS_MFT_SECURE);
    if (!fs_meta) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_NUM;
        strncat(tsk_errstr2,
            " - load_secure: Error finding $Secure MFT Entry",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        return 1;
    }

    fs_attr = tsk_fs_attrlist_get(fs_meta->attr, NTFS_ATYPE_DATA);
    if (!fs_attr) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ntfs_load_secure: Error finding $Data attribute");
        return 1;
    }

    sds_buffer.size = (size_t) roundup(fs_attr->size, fs->block_size);
    sds_buffer.used = 0;
    if ((sds_buffer.buffer = tsk_malloc(sds_buffer.size)) == NULL) {
        return 1;
    }

    if (tsk_fs_attr_walk(fs_attr, 0,
            ntfs_load_sxx_buffer, (void *) &sds_buffer)) {
        tsk_fs_meta_close(fs_meta);
        return 1;
    }

    if (ntfs_proc_sds(fs, &sds_buffer)) {
        free(sds_buffer.buffer);
        tsk_fs_meta_close(fs_meta);
        return 1;
    }

    free(sds_buffer.buffer);
    tsk_fs_meta_close(fs_meta);
    return 0;
}

char *
ntfs_get_sid_as_string(TSK_FS_INFO * fs, uint32_t security_id)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    NTFS_SID_ENTRY *sid_entry = ntfs->sid;

    while ((sid_entry) && sid_entry->sec_id != security_id) {
        sid_entry = sid_entry->next;
    }

    if (sid_entry) {
        return sid_entry->sid_str;
    }

    return "";
}

ntfs_secure_data_free(NTFS_INFO * ntfs)
{
    NTFS_SDS_ENTRY *nsds;
    NTFS_SID_ENTRY *nsid;

    // Iterate of sds entries and free them
    while (ntfs->sds) {
        nsds = ntfs->sds->next;
        free(ntfs->sds->data);
        free(ntfs->sds);
        ntfs->sds = nsds;
    }

    // Iterate of sid entries and free them
    while (ntfs->sid) {
        nsid = ntfs->sid->next;
        free(ntfs->sid->data);
        free(ntfs->sid->sid_str);
        free(ntfs->sid);
        ntfs->sid = nsid;
    }
}

#endif



/**********************************************************************
 *
 *  Exported Walk Functions
 *
 **********************************************************************/


TSK_FS_BLOCK_FLAG_ENUM
ntfs_block_getflags(TSK_FS_INFO * a_fs, TSK_DADDR_T a_addr)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) a_fs;
    int retval;
    int flags = 0;

    /* identify if the cluster is allocated or not */
    retval = is_clustalloc(ntfs, a_addr);
    if (retval == 1)
        flags = TSK_FS_BLOCK_FLAG_ALLOC;
    else if (retval == 0)
        flags = TSK_FS_BLOCK_FLAG_UNALLOC;

    return flags;
}



/*
 * flags: TSK_FS_BLOCK_FLAG_ALLOC and FS_FLAG_UNALLOC
 *
 * @@@ We should probably consider some data META, but it is tough with
 * the NTFS design ...
 */
uint8_t
ntfs_block_walk(TSK_FS_INFO * fs,
    TSK_DADDR_T a_start_blk, TSK_DADDR_T a_end_blk,
    TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags, TSK_FS_BLOCK_WALK_CB a_action,
    void *a_ptr)
{
    char *myname = "ntfs_block_walk";
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    TSK_DADDR_T addr;
    TSK_FS_BLOCK *fs_block;

    // clean up any error messages that are lying around
    tsk_error_reset();

    /*
     * Sanity checks.
     */
    if (a_start_blk < fs->first_block || a_start_blk > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: start block: %" PRIuDADDR "", myname, a_start_blk);
        return 1;
    }
    else if (a_end_blk < fs->first_block || a_end_blk > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: last block: %" PRIuDADDR "", myname, a_end_blk);
        return 1;
    }

    /* Sanity check on a_flags -- make sure at least one ALLOC is set */
    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_ALLOC |
            TSK_FS_BLOCK_WALK_FLAG_UNALLOC);
    }
    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_META) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_CONT | TSK_FS_BLOCK_WALK_FLAG_META);
    }


    if ((fs_block = tsk_fs_block_alloc(fs)) == NULL) {
        return 1;
    }

    /* Cycle through the blocks */
    for (addr = a_start_blk; addr <= a_end_blk; addr++) {
        int retval;
        int myflags;

        /* identify if the cluster is allocated or not */
        retval = is_clustalloc(ntfs, addr);
        if (retval == -1) {
            tsk_fs_block_free(fs_block);
            return 1;
        }

        else if (retval == 1) {
            myflags = TSK_FS_BLOCK_FLAG_ALLOC;
        }
        else {
            myflags = TSK_FS_BLOCK_FLAG_UNALLOC;
        }

        // test if we should call the callback with this one
        if ((myflags & TSK_FS_BLOCK_FLAG_ALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_UNALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC)))
            continue;

        if (tsk_fs_block_get(fs, fs_block, addr) == NULL) {
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "ntfs_block_walk: Error reading block at %"
                PRIuDADDR, addr);
            tsk_fs_block_free(fs_block);
            return 1;
        }

        retval = a_action(fs_block, a_ptr);
        if (retval == TSK_WALK_STOP) {
            break;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_block_free(fs_block);
            return 1;
        }
    }

    tsk_fs_block_free(fs_block);
    return 0;
}



/*
 * inode_walk
 *
 * Flags: TSK_FS_META_FLAG_ALLOC, TSK_FS_META_FLAG_UNALLOC, 
 * TSK_FS_META_FLAG_USED, TSK_FS_META_FLAG_UNUSED, TSK_FS_META_FLAG_ORPHAN
 *
 * Note that with ORPHAN, entries will be found that can also be
 * found by searching based on parent directories (if parent directory is
 * known)
 */
uint8_t
ntfs_inode_walk(TSK_FS_INFO * fs, TSK_INUM_T start_inum,
    TSK_INUM_T end_inum, TSK_FS_META_FLAG_ENUM flags,
    TSK_FS_META_WALK_CB a_action, void *ptr)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    int myflags;
    TSK_INUM_T mftnum;
    TSK_FS_FILE *fs_file;
    TSK_INUM_T end_inum_tmp;

    /*
     * Sanity checks.
     */
    if (start_inum < fs->first_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "inode_walk: Starting inode number is too small (%"
            PRIuINUM ")", start_inum);
        return 1;
    }
    if (start_inum > fs->last_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "inode_walk: Starting inode number is too large (%"
            PRIuINUM ")", start_inum);
        return 1;
    }
    if (end_inum < fs->first_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "inode_walk: Ending inode number is too small (%"
            PRIuINUM ")", end_inum);
        return 1;
    }
    if (end_inum > fs->last_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Ending inode number is too large (%" PRIuINUM ")", end_inum);
        return 1;
    }


    /* If ORPHAN is wanted, then make sure that the flags are correct */
    if (flags & TSK_FS_META_FLAG_ORPHAN) {
        flags |= TSK_FS_META_FLAG_UNALLOC;
        flags &= ~TSK_FS_META_FLAG_ALLOC;
        flags |= TSK_FS_META_FLAG_USED;
        flags &= ~TSK_FS_META_FLAG_UNUSED;
    }

    else {
        if (((flags & TSK_FS_META_FLAG_ALLOC) == 0) &&
            ((flags & TSK_FS_META_FLAG_UNALLOC) == 0)) {
            flags |= (TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNALLOC);
        }

        /* If neither of the USED or UNUSED flags are set, then set them
         * both
         */
        if (((flags & TSK_FS_META_FLAG_USED) == 0) &&
            ((flags & TSK_FS_META_FLAG_UNUSED) == 0)) {
            flags |= (TSK_FS_META_FLAG_USED | TSK_FS_META_FLAG_UNUSED);
        }
    }


    /* If we are looking for orphan files and have not yet filled
     * in the list of unalloc inodes that are pointed to, then fill
     * in the list
     * */
    if ((flags & TSK_FS_META_FLAG_ORPHAN)
        && (fs->list_inum_named == NULL)) {
        if (tsk_fs_dir_load_inum_named(fs) != TSK_OK) {
            strncat(tsk_errstr2,
                " - ntfs_inode_walk: identifying inodes allocated by file names",
                TSK_ERRSTR_L);
            return 1;
        }
    }

    if ((fs_file = tsk_fs_file_alloc(fs)) == NULL)
        return 1;

    if ((fs_file->meta = tsk_fs_meta_alloc(NTFS_FILE_CONTENT_LEN)) == NULL)
        return 1;

    // we need to handle fs->last_inum specially because it is for the 
    // virtual ORPHANS directory.  Handle it outside of the loop.
    if (end_inum == TSK_FS_ORPHANDIR_INUM(fs))
        end_inum_tmp = end_inum - 1;
    else
        end_inum_tmp = end_inum;


    for (mftnum = start_inum; mftnum <= end_inum_tmp; mftnum++) {
        int retval;
        TSK_RETVAL_ENUM retval2;

        /* read MFT entry in to NTFS_INFO */
        if ((retval2 = ntfs_dinode_load(ntfs, mftnum)) != TSK_OK) {
            // if the entry is corrupt, then skip to the next one
            if (retval2 == TSK_COR) {
                if (tsk_verbose)
                    tsk_error_print(stderr);
                tsk_error_reset();
                continue;
            }
            tsk_fs_file_close(fs_file);
            return 1;
        }

        /* we only want to look at base file records 
         * (extended are because the base could not fit into one)
         */
        if (tsk_getu48(fs->endian, ntfs->mft->base_ref) != NTFS_MFT_BASE)
            continue;

        /* NOTE: We could add a sanity check here with the MFT bitmap
         * to validate of the INUSE flag and bitmap are in agreement
         */
        /* check flags */
        myflags =
            ((tsk_getu16(fs->endian, ntfs->mft->flags) &
                NTFS_MFT_INUSE) ? TSK_FS_META_FLAG_ALLOC :
            TSK_FS_META_FLAG_UNALLOC);

        /* We consider USED entries to be ones with attributes */
        if (tsk_getu16(fs->endian, ntfs->mft->attr_off) == 0)
            myflags |= TSK_FS_META_FLAG_UNUSED;
        else
            myflags |= TSK_FS_META_FLAG_USED;

        if ((flags & myflags) != myflags)
            continue;

        /* If we want only orphans, then check if this
         * inode is in the seen list
         * */
        if ((myflags & TSK_FS_META_FLAG_UNALLOC) &&
            (flags & TSK_FS_META_FLAG_ORPHAN) &&
            (tsk_list_find(fs->list_inum_named, mftnum))) {
            continue;
        }


        /* copy into generic format */
        if ((retval = ntfs_dinode_copy(ntfs, fs_file)) != TSK_OK) {
            // continue on if there were only corruption problems
            if (retval == TSK_COR) {
                if (tsk_verbose)
                    tsk_error_print(stderr);
                tsk_error_reset();
                continue;
            }
            tsk_fs_file_close(fs_file);
            return 1;
        }

        /* call action */
        retval = a_action(fs_file, ptr);
        if (retval == TSK_WALK_STOP) {
            tsk_fs_file_close(fs_file);
            return 0;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
    }

    // handle the virtual orphans folder if they asked for it
    if ((end_inum == TSK_FS_ORPHANDIR_INUM(fs))
        && (flags & TSK_FS_META_FLAG_ALLOC)
        && (flags & TSK_FS_META_FLAG_USED)) {
        int retval;

        if (tsk_fs_dir_make_orphan_dir_meta(fs, fs_file->meta)) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
        /* call action */
        retval = a_action(fs_file, ptr);
        if (retval == TSK_WALK_STOP) {
            tsk_fs_file_close(fs_file);
            return 0;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
    }

    tsk_fs_file_close(fs_file);
    return 0;
}



static uint8_t
ntfs_fscheck(TSK_FS_INFO * fs, FILE * hFile)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "fscheck not implemented for NTFS yet");
    return 1;
}


/**
 * Print details about the file system to a file handle. 
 *
 * @param fs File system to print details on
 * @param hFile File handle to print text to
 * 
 * @returns 1 on error and 0 on success
 */
static uint8_t
ntfs_fsstat(TSK_FS_INFO * fs, FILE * hFile)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;
    TSK_FS_FILE *fs_file;
    const TSK_FS_ATTR *fs_attr;
    char asc[512];
    ntfs_attrdef *attrdeftmp;

    tsk_fprintf(hFile, "FILE SYSTEM INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "File System Type: NTFS\n");
    tsk_fprintf(hFile,
        "Volume Serial Number: %.16" PRIX64
        "\n", tsk_getu64(fs->endian, ntfs->fs->serial));
    tsk_fprintf(hFile, "OEM Name: %c%c%c%c%c%c%c%c\n",
        ntfs->fs->oemname[0],
        ntfs->fs->oemname[1],
        ntfs->fs->oemname[2],
        ntfs->fs->oemname[3],
        ntfs->fs->oemname[4],
        ntfs->fs->oemname[5], ntfs->fs->oemname[6], ntfs->fs->oemname[7]);
    /*
     * Volume 
     */
    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, NTFS_MFT_VOL)) == NULL) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_NUM;
        strncat(tsk_errstr2, " - fsstat: Error finding Volume MFT Entry",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        return 1;
    }

    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_VNAME);
    if (!fs_attr) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_COR;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Volume Name attribute not found in $Volume");
        return 1;
    }

    if ((fs_attr->flags & TSK_FS_ATTR_RES)
        && (fs_attr->size)) {

        UTF16 *name16 = (UTF16 *) fs_attr->rd.buf;
        UTF8 *name8 = (UTF8 *) asc;
        int retVal;
        retVal =
            tsk_UTF16toUTF8(fs->endian, (const UTF16 **) &name16,
            (UTF16 *) ((uintptr_t) name16 +
                (int) fs_attr->
                size), &name8,
            (UTF8 *) ((uintptr_t) name8 + sizeof(asc)),
            TSKlenientConversion);
        if (retVal != TSKconversionOK) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fsstat: Error converting NTFS Volume label to UTF8: %d",
                    retVal);
            *name8 = '\0';
        }

        /* Make sure it is NULL Terminated */
        else if ((uintptr_t) name8 > (uintptr_t) asc + sizeof(asc))
            asc[sizeof(asc)] = '\0';
        else
            *name8 = '\0';
        tsk_fprintf(hFile, "Volume Name: %s\n", asc);
    }

    tsk_fs_file_close(fs_file);
    fs_file = NULL;
    fs_attr = NULL;
    if (ntfs->ver == NTFS_VINFO_NT)
        tsk_fprintf(hFile, "Version: Windows NT\n");
    else if (ntfs->ver == NTFS_VINFO_2K)
        tsk_fprintf(hFile, "Version: Windows 2000\n");
    else if (ntfs->ver == NTFS_VINFO_XP)
        tsk_fprintf(hFile, "Version: Windows XP\n");
    tsk_fprintf(hFile, "\nMETADATA INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile,
        "First Cluster of MFT: %" PRIu64 "\n",
        tsk_getu64(fs->endian, ntfs->fs->mft_clust));
    tsk_fprintf(hFile,
        "First Cluster of MFT Mirror: %"
        PRIu64 "\n", tsk_getu64(fs->endian, ntfs->fs->mftm_clust));
    tsk_fprintf(hFile,
        "Size of MFT Entries: %" PRIu16 " bytes\n", ntfs->mft_rsize_b);
    tsk_fprintf(hFile,
        "Size of Index Records: %" PRIu16 " bytes\n", ntfs->idx_rsize_b);
    tsk_fprintf(hFile,
        "Range: %" PRIuINUM " - %" PRIuINUM
        "\n", fs->first_inum, fs->last_inum);
    tsk_fprintf(hFile, "Root Directory: %" PRIuINUM "\n", fs->root_inum);
    tsk_fprintf(hFile, "\nCONTENT INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Sector Size: %" PRIu16 "\n", ntfs->ssize_b);
    tsk_fprintf(hFile, "Cluster Size: %" PRIu16 "\n", ntfs->csize_b);
    tsk_fprintf(hFile,
        "Total Cluster Range: %" PRIuDADDR
        " - %" PRIuDADDR "\n", fs->first_block, fs->last_block);

    if (fs->last_block != fs->last_block_act)
        tsk_fprintf(hFile,
            "Total Range in Image: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fs->first_block, fs->last_block_act);

    tsk_fprintf(hFile,
        "Total Sector Range: 0 - %" PRIu64
        "\n", tsk_getu64(fs->endian, ntfs->fs->vol_size_s) - 1);
    /* 
     * Attrdef Info 
     */
    tsk_fprintf(hFile, "\n$AttrDef Attribute Values:\n");
    if (!ntfs->attrdef) {
        if (ntfs_load_attrdef(ntfs)) {
            tsk_fprintf(hFile, "Error loading attribute definitions\n");
            goto attrdef_egress;
        }
    }

    attrdeftmp = ntfs->attrdef;
    while ((((uintptr_t) attrdeftmp - (uintptr_t) ntfs->attrdef +
                sizeof(ntfs_attrdef)) < ntfs->attrdef_len) &&
        (tsk_getu32(fs->endian, attrdeftmp->type))) {
        UTF16 *name16 = (UTF16 *) attrdeftmp->label;
        UTF8 *name8 = (UTF8 *) asc;
        int retVal;
        retVal =
            tsk_UTF16toUTF8(fs->endian, (const UTF16 **) &name16,
            (UTF16 *) ((uintptr_t) name16 +
                sizeof(attrdeftmp->
                    label)),
            &name8,
            (UTF8 *) ((uintptr_t) name8 + sizeof(asc)),
            TSKlenientConversion);
        if (retVal != TSKconversionOK) {
            if (tsk_verbose)
                tsk_fprintf(stderr,
                    "fsstat: Error converting NTFS attribute def label to UTF8: %d",
                    retVal);
            *name8 = '\0';
        }

        /* Make sure it is NULL Terminated */
        else if ((uintptr_t) name8 > (uintptr_t) asc + sizeof(asc))
            asc[sizeof(asc)] = '\0';
        else
            *name8 = '\0';
        tsk_fprintf(hFile, "%s (%" PRIu32 ")   ",
            asc, tsk_getu32(fs->endian, attrdeftmp->type));
        if ((tsk_getu64(fs->endian, attrdeftmp->minsize) == 0) &&
            (tsk_getu64(fs->endian,
                    attrdeftmp->maxsize) == 0xffffffffffffffffULL)) {

            tsk_fprintf(hFile, "Size: No Limit");
        }
        else {
            tsk_fprintf(hFile, "Size: %" PRIu64 "-%" PRIu64,
                tsk_getu64(fs->endian, attrdeftmp->minsize),
                tsk_getu64(fs->endian, attrdeftmp->maxsize));
        }

        tsk_fprintf(hFile, "   Flags: %s%s%s\n",
            (tsk_getu32(fs->endian, attrdeftmp->flags) &
                NTFS_ATTRDEF_FLAGS_RES ? "Resident" :
                ""), (tsk_getu32(fs->endian,
                    attrdeftmp->
                    flags) &
                NTFS_ATTRDEF_FLAGS_NONRES ?
                "Non-resident" : ""),
            (tsk_getu32(fs->endian, attrdeftmp->flags) &
                NTFS_ATTRDEF_FLAGS_IDX ? ",Index" : ""));
        attrdeftmp++;
    }

  attrdef_egress:

    return 0;
}


/************************* istat *******************************/

#define NTFS_PRINT_WIDTH   8
typedef struct {
    FILE *hFile;
    int idx;
} NTFS_PRINT_ADDR;
static TSK_WALK_RET_ENUM
print_addr_act(TSK_FS_FILE * fs_file, TSK_OFF_T a_off, TSK_DADDR_T addr,
    char *buf, size_t size, TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr)
{
    NTFS_PRINT_ADDR *print = (NTFS_PRINT_ADDR *) ptr;
    tsk_fprintf(print->hFile, "%" PRIuDADDR " ", addr);
    if (++(print->idx) == NTFS_PRINT_WIDTH) {
        tsk_fprintf(print->hFile, "\n");
        print->idx = 0;
    }

    return TSK_WALK_CONT;
}

/**
 * Print details on a specific file to a file handle. 
 *
 * @param fs File system file is located in
 * @param hFile File name to print text to
 * @param inum Address of file in file system
 * @param numblock The number of blocks in file to force print (can go beyond file size)
 * @param sec_skew Clock skew in seconds to also print times in
 * 
 * @returns 1 on error and 0 on success
 */
static uint8_t
ntfs_istat(TSK_FS_INFO * fs, FILE * hFile,
    TSK_INUM_T inum, TSK_DADDR_T numblock, int32_t sec_skew)
{
    TSK_FS_FILE *fs_file;
    const TSK_FS_ATTR *fs_attr;
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, inum)) == NULL) {
        strncat(tsk_errstr2, " - istat",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        return 1;
    }

    tsk_fprintf(hFile, "MFT Entry Header Values:\n");
    tsk_fprintf(hFile,
        "Entry: %" PRIuINUM
        "        Sequence: %" PRIu32 "\n", inum, fs_file->meta->seq);
    if (tsk_getu48(fs->endian, ntfs->mft->base_ref) != 0) {
        tsk_fprintf(hFile,
            "Base File Record: %" PRIu64 "\n",
            (uint64_t) tsk_getu48(fs->endian, ntfs->mft->base_ref));
    }

    tsk_fprintf(hFile,
        "$LogFile Sequence Number: %" PRIu64
        "\n", tsk_getu64(fs->endian, ntfs->mft->lsn));
    tsk_fprintf(hFile, "%sAllocated %s\n",
        (fs_file->meta->
            flags & TSK_FS_META_FLAG_ALLOC) ? "" :
        "Not ",
        (fs_file->meta->
            type == TSK_FS_META_TYPE_DIR) ? "Directory" : "File");
    tsk_fprintf(hFile, "Links: %u\n", fs_file->meta->nlink);

    /* STANDARD_INFORMATION info */
    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_SI);
    if (fs_attr) {
        ntfs_attr_si *si = (ntfs_attr_si *) fs_attr->rd.buf;
        int a = 0;
        tsk_fprintf(hFile, "\n$STANDARD_INFORMATION Attribute Values:\n");
        tsk_fprintf(hFile, "Flags: ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_RO)
            tsk_fprintf(hFile, "%sRead Only", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_HID)
            tsk_fprintf(hFile, "%sHidden", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_SYS)
            tsk_fprintf(hFile, "%sSystem", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_ARCH)
            tsk_fprintf(hFile, "%sArchive", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_DEV)
            tsk_fprintf(hFile, "%sDevice", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_NORM)
            tsk_fprintf(hFile, "%sNormal", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_TEMP)
            tsk_fprintf(hFile, "%sTemporary", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_SPAR)
            tsk_fprintf(hFile, "%sSparse", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_REP)
            tsk_fprintf(hFile, "%sReparse Point", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_COMP)
            tsk_fprintf(hFile, "%sCompressed", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_OFF)
            tsk_fprintf(hFile, "%sOffline", a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_NOIDX)
            tsk_fprintf(hFile, "%sNot Content Indexed",
                a++ == 0 ? "" : ", ");
        if (tsk_getu32(fs->endian, si->dos) & NTFS_SI_ENC)
            tsk_fprintf(hFile, "%sEncrypted", a++ == 0 ? "" : ", ");
        tsk_fprintf(hFile, "\n");
        tsk_fprintf(hFile, "Owner ID: %" PRIu32 "\n",
            tsk_getu32(fs->endian, si->own_id));

#if TSK_USE_SID
        tsk_fprintf(hFile, "Security ID: %" PRIu32 "  (%s)\n",
            tsk_getu32(fs->endian, si->sec_id), ntfs_get_sid_as_string(fs,
                tsk_getu32(fs->endian, si->sec_id)));
#endif


        if (tsk_getu32(fs->endian, si->maxver) != 0) {
            tsk_fprintf(hFile,
                "Version %" PRIu32 " of %" PRIu32
                "\n", tsk_getu32(fs->endian, si->ver),
                tsk_getu32(fs->endian, si->maxver));
        }

        if (tsk_getu64(fs->endian, si->quota) != 0) {
            tsk_fprintf(hFile, "Quota Charged: %" PRIu64 "\n",
                tsk_getu64(fs->endian, si->quota));
        }

        if (tsk_getu64(fs->endian, si->usn) != 0) {
            tsk_fprintf(hFile,
                "Last User Journal Update Sequence Number: %"
                PRIu64 "\n", tsk_getu64(fs->endian, si->usn));
        }


        /* Times - take it from fs_file->meta instead of redoing the work */

        if (sec_skew != 0) {
            tsk_fprintf(hFile, "\nAdjusted times:\n");
            fs_file->meta->mtime -= sec_skew;
            fs_file->meta->atime -= sec_skew;
            fs_file->meta->ctime -= sec_skew;
            fs_file->meta->crtime -= sec_skew;
            tsk_fprintf(hFile, "Created:\t%s",
                ctime(&fs_file->meta->crtime));
            tsk_fprintf(hFile, "File Modified:\t%s",
                ctime(&fs_file->meta->mtime));
            tsk_fprintf(hFile, "MFT Modified:\t%s",
                ctime(&fs_file->meta->ctime));
            tsk_fprintf(hFile, "Accessed:\t%s",
                ctime(&fs_file->meta->atime));
            fs_file->meta->mtime += sec_skew;
            fs_file->meta->atime += sec_skew;
            fs_file->meta->ctime += sec_skew;
            fs_file->meta->crtime += sec_skew;
            tsk_fprintf(hFile, "\nOriginal times:\n");
        }

        tsk_fprintf(hFile, "Created:\t%s", ctime(&fs_file->meta->crtime));
        tsk_fprintf(hFile, "File Modified:\t%s",
            ctime(&fs_file->meta->mtime));
        tsk_fprintf(hFile, "MFT Modified:\t%s",
            ctime(&fs_file->meta->ctime));
        tsk_fprintf(hFile, "Accessed:\t%s", ctime(&fs_file->meta->atime));
    }

    /* $FILE_NAME Information */
    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_FNAME);
    if (fs_attr) {

        ntfs_attr_fname *fname = (ntfs_attr_fname *) fs_attr->rd.buf;
        time_t cr_time, m_time, c_time, a_time;
        uint64_t flags;
        int a = 0;
        tsk_fprintf(hFile, "\n$FILE_NAME Attribute Values:\n");
        flags = tsk_getu64(fs->endian, fname->flags);
        tsk_fprintf(hFile, "Flags: ");
        if (flags & NTFS_FNAME_FLAGS_DIR)
            tsk_fprintf(hFile, "%sDirectory", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_DEV)
            tsk_fprintf(hFile, "%sDevice", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_NORM)
            tsk_fprintf(hFile, "%sNormal", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_RO)
            tsk_fprintf(hFile, "%sRead Only", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_HID)
            tsk_fprintf(hFile, "%sHidden", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_SYS)
            tsk_fprintf(hFile, "%sSystem", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_ARCH)
            tsk_fprintf(hFile, "%sArchive", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_TEMP)
            tsk_fprintf(hFile, "%sTemp", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_SPAR)
            tsk_fprintf(hFile, "%sSparse", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_REP)
            tsk_fprintf(hFile, "%sReparse Point", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_COMP)
            tsk_fprintf(hFile, "%sCompressed", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_ENC)
            tsk_fprintf(hFile, "%sEncrypted", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_OFF)
            tsk_fprintf(hFile, "%sOffline", a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_NOIDX)
            tsk_fprintf(hFile, "%sNot Content Indexed",
                a++ == 0 ? "" : ", ");
        if (flags & NTFS_FNAME_FLAGS_IDXVIEW)
            tsk_fprintf(hFile, "%sIndex View", a++ == 0 ? "" : ", ");
        tsk_fprintf(hFile, "\n");
        /* We could look this up in the attribute, but we already did
         * the work */
        if (fs_file->meta->name2) {
            TSK_FS_META_NAME_LIST *fs_name = fs_file->meta->name2;
            tsk_fprintf(hFile, "Name: ");
            while (fs_name) {
                tsk_fprintf(hFile, "%s", fs_name->name);
                fs_name = fs_name->next;
                if (fs_name)
                    tsk_fprintf(hFile, ", ");
                else
                    tsk_fprintf(hFile, "\n");
            }
        }

        tsk_fprintf(hFile,
            "Parent MFT Entry: %" PRIu64
            " \tSequence: %" PRIu16 "\n",
            (uint64_t) tsk_getu48(fs->endian, fname->par_ref),
            tsk_getu16(fs->endian, fname->par_seq));
        tsk_fprintf(hFile,
            "Allocated Size: %" PRIu64
            "   \tActual Size: %" PRIu64 "\n",
            tsk_getu64(fs->endian, fname->alloc_fsize),
            tsk_getu64(fs->endian, fname->real_fsize));
        /* 
         * Times 
         */
        cr_time = nt2unixtime(tsk_getu64(fs->endian, fname->crtime));
        /* altered - modified */
        m_time = nt2unixtime(tsk_getu64(fs->endian, fname->mtime));
        /* MFT modified */
        c_time = nt2unixtime(tsk_getu64(fs->endian, fname->ctime));
        /* Access */
        a_time = nt2unixtime(tsk_getu64(fs->endian, fname->atime));
        if (sec_skew != 0) {
            tsk_fprintf(hFile, "\nAdjusted times:\n");
            cr_time -= sec_skew;
            m_time -= sec_skew;
            a_time -= sec_skew;
            c_time -= sec_skew;
            tsk_fprintf(hFile, "Created:\t%s", ctime(&cr_time));
            tsk_fprintf(hFile, "File Modified:\t%s", ctime(&m_time));
            tsk_fprintf(hFile, "MFT Modified:\t%s", ctime(&c_time));
            tsk_fprintf(hFile, "Accessed:\t%s", ctime(&a_time));
            cr_time += sec_skew;
            m_time += sec_skew;
            a_time += sec_skew;
            c_time += sec_skew;
            tsk_fprintf(hFile, "\nOriginal times:\n");
        }

        tsk_fprintf(hFile, "Created:\t%s", ctime(&cr_time));
        tsk_fprintf(hFile, "File Modified:\t%s", ctime(&m_time));
        tsk_fprintf(hFile, "MFT Modified:\t%s", ctime(&c_time));
        tsk_fprintf(hFile, "Accessed:\t%s", ctime(&a_time));
    }


    /* $OBJECT_ID Information */
    fs_attr = tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_OBJID);
    if (fs_attr) {
        ntfs_attr_objid *objid = (ntfs_attr_objid *) fs_attr->rd.buf;
        uint64_t id1, id2;
        tsk_fprintf(hFile, "\n$OBJECT_ID Attribute Values:\n");
        id1 = tsk_getu64(fs->endian, objid->objid1);
        id2 = tsk_getu64(fs->endian, objid->objid2);
        tsk_fprintf(hFile,
            "Object Id: %.8" PRIx32 "-%.4" PRIx16
            "-%.4" PRIx16 "-%.4" PRIx16 "-%.12"
            PRIx64 "\n",
            (uint32_t) (id2 >> 32) & 0xffffffff,
            (uint16_t) (id2 >> 16) & 0xffff,
            (uint16_t) (id2 & 0xffff),
            (uint16_t) (id1 >> 48) & 0xffff, (uint64_t) (id1 & (uint64_t)
                0x0000ffffffffffffULL));
        /* The rest of the  fields do not always exist.  Check the attr size */
        if (fs_attr->size > 16) {
            id1 = tsk_getu64(fs->endian, objid->orig_volid1);
            id2 = tsk_getu64(fs->endian, objid->orig_volid2);
            tsk_fprintf(hFile,
                "Birth Volume Id: %.8" PRIx32 "-%.4"
                PRIx16 "-%.4" PRIx16 "-%.4" PRIx16
                "-%.12" PRIx64 "\n",
                (uint32_t) (id2 >> 32) & 0xffffffff,
                (uint16_t) (id2 >> 16) & 0xffff,
                (uint16_t) (id2 & 0xffff),
                (uint16_t) (id1 >> 48) & 0xffff,
                (uint64_t) (id1 & (uint64_t)
                    0x0000ffffffffffffULL));
        }

        if (fs_attr->size > 32) {
            id1 = tsk_getu64(fs->endian, objid->orig_objid1);
            id2 = tsk_getu64(fs->endian, objid->orig_objid2);
            tsk_fprintf(hFile,
                "Birth Object Id: %.8" PRIx32 "-%.4"
                PRIx16 "-%.4" PRIx16 "-%.4" PRIx16
                "-%.12" PRIx64 "\n",
                (uint32_t) (id2 >> 32) & 0xffffffff,
                (uint16_t) (id2 >> 16) & 0xffff,
                (uint16_t) (id2 & 0xffff),
                (uint16_t) (id1 >> 48) & 0xffff,
                (uint64_t) (id1 & (uint64_t)
                    0x0000ffffffffffffULL));
        }

        if (fs_attr->size > 48) {
            id1 = tsk_getu64(fs->endian, objid->orig_domid1);
            id2 = tsk_getu64(fs->endian, objid->orig_domid2);
            tsk_fprintf(hFile,
                "Birth Domain Id: %.8" PRIx32 "-%.4"
                PRIx16 "-%.4" PRIx16 "-%.4" PRIx16
                "-%.12" PRIx64 "\n",
                (uint32_t) (id2 >> 32) & 0xffffffff,
                (uint16_t) (id2 >> 16) & 0xffff,
                (uint16_t) (id2 & 0xffff),
                (uint16_t) (id1 >> 48) & 0xffff,
                (uint64_t) (id1 & (uint64_t)
                    0x0000ffffffffffffULL));
        }
    }

    /* Attribute List Information */
    fs_attr =
        tsk_fs_attrlist_get(fs_file->meta->attr, NTFS_ATYPE_ATTRLIST);
    if (fs_attr) {
        char *buf;
        ntfs_attrlist *list;
        uintptr_t endaddr;
        TSK_FS_LOAD_FILE load_file;

        tsk_fprintf(hFile, "\n$ATTRIBUTE_LIST Attribute Values:\n");

        /* Get a copy of the attribute list stream  */
        load_file.total = load_file.left = (size_t) fs_attr->size;
        load_file.cur = load_file.base = buf =
            tsk_malloc((size_t) fs_attr->size);
        if (buf == NULL) {
            return 1;
        }

        endaddr = (uintptr_t) buf + (uintptr_t) fs_attr->size;
        if (tsk_fs_attr_walk(fs_attr,
                0, tsk_fs_load_file_action, (void *) &load_file)) {
            tsk_fprintf(hFile, "error reading attribute list buffer\n");
            tsk_error_reset();
            goto egress;
        }

        /* this value should be zero, if not then we didn't read all of the
         * buffer
         */
        if (load_file.left > 0) {
            tsk_fprintf(hFile, "error reading attribute list buffer\n");
            goto egress;
        }

        /* Process the list & print the details */
        for (list = (ntfs_attrlist *) buf;
            (list) && ((uintptr_t) list < endaddr)
            && (tsk_getu16(fs->endian, list->len) > 0);
            list =
            (ntfs_attrlist *) ((uintptr_t) list + tsk_getu16(fs->endian,
                    list->len))) {
            tsk_fprintf(hFile,
                "Type: %" PRIu32 "-%" PRIu16 " \tMFT Entry: %" PRIu64
                " \tVCN: %" PRIu64 "\n", tsk_getu32(fs->endian,
                    list->type), tsk_getu16(fs->endian, list->id),
                (uint64_t) tsk_getu48(fs->endian, list->file_ref),
                tsk_getu64(fs->endian, list->start_vcn));
        }
      egress:
        free(buf);
    }

    /* Print all of the attributes */
    tsk_fprintf(hFile, "\nAttributes: \n");
    if (fs_file->meta->attr) {
        int cnt, i;

        // cycle through the attributes
        cnt = tsk_fs_file_attr_getsize(fs_file);
        for (i = 0; i < cnt; i++) {
            char type[512];

            const TSK_FS_ATTR *fs_attr =
                tsk_fs_file_attr_get_idx(fs_file, i);
            if (!fs_attr)
                continue;

            if (ntfs_attrname_lookup(fs, fs_attr->type, type, 512)) {
                tsk_fprintf(hFile, "error looking attribute name\n");
                break;
            }
            tsk_fprintf(hFile,
                "Type: %s (%" PRIu32 "-%" PRIu16
                ")   Name: %s   %sResident%s%s%s   size: %"
                PRIuOFF "\n", type, fs_attr->type,
                fs_attr->id, fs_attr->name,
                (fs_attr->
                    flags & TSK_FS_ATTR_NONRES) ? "Non-" :
                "",
                (fs_attr->
                    flags & TSK_FS_ATTR_ENC) ? ", Encrypted"
                : "",
                (fs_attr->
                    flags & TSK_FS_ATTR_COMP) ?
                ", Compressed" : "",
                (fs_attr->
                    flags & TSK_FS_ATTR_SPARSE) ? ", Sparse" : "",
                fs_attr->size);

            /* print the layout if it is non-resident and not "special" */
            if (fs_attr->flags & TSK_FS_ATTR_NONRES) {
                NTFS_PRINT_ADDR print_addr;
                print_addr.idx = 0;
                print_addr.hFile = hFile;
                if (tsk_fs_file_walk_type(fs_file, fs_attr->type,
                        fs_attr->id,
                        (TSK_FS_FILE_WALK_FLAG_AONLY |
                            TSK_FS_FILE_WALK_FLAG_SLACK),
                        print_addr_act, (void *) &print_addr)) {
                    tsk_fprintf(hFile, "\nError walking file\n");
                    tsk_error_print(hFile);
                    tsk_error_reset();
                }
                if (print_addr.idx != 0)
                    tsk_fprintf(hFile, "\n");
            }
        }
    }

    tsk_fs_file_close(fs_file);
    return 0;
}



/* JOURNAL CODE - MOVE TO NEW FILE AT SOME POINT */

uint8_t
ntfs_jopen(TSK_FS_INFO * fs, TSK_INUM_T inum)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "NTFS Journal is not yet supported\n");
    return 1;
}

uint8_t
ntfs_jentry_walk(TSK_FS_INFO * fs, int flags,
    TSK_FS_JENTRY_WALK_CB a_action, void *ptr)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "NTFS Journal is not yet supported\n");
    return 1;
}


uint8_t
ntfs_jblk_walk(TSK_FS_INFO * fs, TSK_DADDR_T start,
    TSK_DADDR_T end, int flags, TSK_FS_JBLK_WALK_CB a_action, void *ptr)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "NTFS Journal is not yet supported\n");
    return 1;
}


static TSK_FS_ATTR_TYPE_ENUM
ntfs_get_default_attr_type(const TSK_FS_FILE * a_file)
{
    if ((a_file == NULL) || (a_file->meta == NULL))
        return TSK_FS_ATTR_TYPE_DEFAULT;

    /* Use DATA for files and IDXROOT for dirs */
    if (a_file->meta->type == TSK_FS_META_TYPE_DIR)
        return TSK_FS_ATTR_TYPE_NTFS_IDXROOT;
    else
        return TSK_FS_ATTR_TYPE_NTFS_DATA;

}


static void
ntfs_close(TSK_FS_INFO * fs)
{
    NTFS_INFO *ntfs = (NTFS_INFO *) fs;

    fs->tag = 0;
    free((char *) ntfs->mft);
    free((char *) ntfs->fs);
    tsk_fs_attr_run_free(ntfs->bmap);
    free(ntfs->bmap_buf);
    tsk_fs_file_close(ntfs->mft_file);
#if TSK_USE_SID
    ntfs_secure_data_free(ntfs);
#endif

    if (fs->list_inum_named) {
        tsk_list_free(fs->list_inum_named);
        fs->list_inum_named = NULL;
    }

    free(fs);
}


/**
 * Open part of a disk image as an NTFS file system. 
 *
 * @param img_info Disk image to analyze
 * @param offset Byte offset where NTFS file system starts
 * @param ftype Specific type of NTFS file system
 * @param test NOT USED
 * @returns NULL on error or if data is not an NTFS file system
 */
TSK_FS_INFO *
ntfs_open(TSK_IMG_INFO * img_info, TSK_OFF_T offset,
    TSK_FS_TYPE_ENUM ftype, uint8_t test)
{
    char *myname = "ntfs_open";
    NTFS_INFO *ntfs;
    TSK_FS_INFO *fs;
    unsigned int len;
    ssize_t cnt;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if (TSK_FS_TYPE_ISNTFS(ftype) == 0) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "Invalid FS type in ntfs_open");
        return NULL;
    }

    if ((ntfs = (NTFS_INFO *) tsk_malloc(sizeof(*ntfs))) == NULL) {
        return NULL;
    }
    fs = &(ntfs->fs_info);

    fs->ftype = TSK_FS_TYPE_NTFS;
    fs->duname = "Cluster";
    fs->flags = TSK_FS_INFO_FLAG_HAVE_SEQ;
    fs->tag = TSK_FS_INFO_TAG;

    fs->img_info = img_info;
    fs->offset = offset;

    ntfs->loading_the_MFT = 0;
    ntfs->bmap = NULL;
    ntfs->bmap_buf = NULL;

    /* Read the boot sector */
    len = roundup(sizeof(ntfs_sb), NTFS_DEV_BSIZE);
    ntfs->fs = (ntfs_sb *) tsk_malloc(len);
    if (ntfs->fs == NULL) {
        fs->tag = 0;
        free(ntfs);
        return NULL;
    }

    cnt = tsk_fs_read(fs, (TSK_OFF_T) 0, (char *) ntfs->fs, len);
    if (cnt != len) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "%s: Error reading boot sector.", myname);
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }

    /* Check the magic value */
    if (tsk_fs_guessu16(fs, ntfs->fs->magic, NTFS_FS_MAGIC)) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (magic)");
        return NULL;
    }


    /*
     * block calculations : although there are no blocks in ntfs,
     * we are using a cluster as a "block"
     */

    ntfs->ssize_b = tsk_getu16(fs->endian, ntfs->fs->ssize);
    if ((ntfs->ssize_b == 0) || (ntfs->ssize_b % 512)) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (invalid sector size)");
        return NULL;
    }

    if ((ntfs->fs->csize != 0x01) &&
        (ntfs->fs->csize != 0x02) &&
        (ntfs->fs->csize != 0x04) &&
        (ntfs->fs->csize != 0x08) &&
        (ntfs->fs->csize != 0x10) &&
        (ntfs->fs->csize != 0x20) && (ntfs->fs->csize != 0x40)
        && (ntfs->fs->csize != 0x80)) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (invalid cluster size)");
        return NULL;
    }

    ntfs->csize_b = ntfs->fs->csize * ntfs->ssize_b;
    fs->first_block = 0;
    /* This field is defined as 64-bits but according to the
     * NTFS drivers in Linux, windows only uses 32-bits
     */
    fs->block_count =
        (TSK_DADDR_T) tsk_getu32(fs->endian,
        ntfs->fs->vol_size_s) / ntfs->fs->csize;
    if (fs->block_count == 0) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (volume size is 0)");
        return NULL;
    }

    fs->last_block = fs->last_block_act = fs->block_count - 1;
    fs->block_size = ntfs->csize_b;
    fs->dev_bsize = NTFS_DEV_BSIZE;

    // determine the last block we have in this image
    if ((TSK_DADDR_T) ((img_info->size - offset) / fs->block_size) <
        fs->block_count)
        fs->last_block_act =
            (img_info->size - offset) / fs->block_size - 1;

    if (ntfs->fs->mft_rsize_c > 0)
        ntfs->mft_rsize_b = ntfs->fs->mft_rsize_c * ntfs->csize_b;
    else
        /* if the mft_rsize_c is not > 0, then it is -log2(rsize_b) */
        ntfs->mft_rsize_b = 1 << -ntfs->fs->mft_rsize_c;

    if ((ntfs->mft_rsize_b == 0) || (ntfs->mft_rsize_b % 512)) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (invalid MFT entry size)");
        return NULL;
    }

    if (ntfs->fs->idx_rsize_c > 0)
        ntfs->idx_rsize_b = ntfs->fs->idx_rsize_c * ntfs->csize_b;
    else
        /* if the idx_rsize_c is not > 0, then it is -log2(rsize_b) */
        ntfs->idx_rsize_b = 1 << -ntfs->fs->idx_rsize_c;

    if ((ntfs->idx_rsize_b == 0) || (ntfs->idx_rsize_b % 512)) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (invalid idx record size)");
        return NULL;
    }

    ntfs->root_mft_addr =
        tsk_getu64(fs->endian, ntfs->fs->mft_clust) * ntfs->csize_b;
    if (tsk_getu64(fs->endian, ntfs->fs->mft_clust) > fs->last_block) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not a NTFS file system (invalid starting MFT clust)");
        return NULL;
    }

    /*
     * Set the function pointers (before we start calling internal functions)
     */
    fs->inode_walk = ntfs_inode_walk;
    fs->block_walk = ntfs_block_walk;
    fs->block_getflags = ntfs_block_getflags;

    fs->get_default_attr_type = ntfs_get_default_attr_type;
    fs->load_attrs = ntfs_load_attrs;

    fs->file_add_meta = ntfs_inode_lookup;
    fs->dir_open_meta = ntfs_dir_open_meta;
    fs->fsstat = ntfs_fsstat;
    fs->fscheck = ntfs_fscheck;
    fs->istat = ntfs_istat;
    fs->close = ntfs_close;

    fs->jblk_walk = ntfs_jblk_walk;
    fs->jentry_walk = ntfs_jentry_walk;
    fs->jopen = ntfs_jopen;
    fs->journ_inum = 0;



    /*
     * inode
     */

    /* allocate the buffer to hold mft entries */
    ntfs->mft = (ntfs_mft *) tsk_malloc(ntfs->mft_rsize_b);
    if (ntfs->mft == NULL) {
        fs->tag = 0;
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }

    ntfs->mnum = 0;
    fs->root_inum = NTFS_ROOTINO;
    fs->first_inum = NTFS_FIRSTINO;
    fs->last_inum = NTFS_LAST_DEFAULT_INO;
    ntfs->mft_data = NULL;

    /* load the data run for the MFT table into ntfs->mft */
    ntfs->loading_the_MFT = 1;
    if ((ntfs->mft_file =
            tsk_fs_file_open_meta(fs, NULL, NTFS_MFT_MFT)) == NULL) {
        fs->tag = 0;
        free(ntfs->mft);
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }

    /* cache the data attribute 
     *
     * This will likely be done already by proc_attrseq, but this
     * should be quick
     */
    ntfs->mft_data =
        tsk_fs_attrlist_get(ntfs->mft_file->meta->attr, NTFS_ATYPE_DATA);
    if (!ntfs->mft_data) {
        fs->tag = 0;
        tsk_fs_file_close(ntfs->mft_file);
        free(ntfs->mft);
        free(ntfs->fs);
        free(ntfs);
        strncat(tsk_errstr2, " - Data Attribute not found in $MFT",
            TSK_ERRSTR_L - strlen(tsk_errstr2));
        return NULL;
    }

    /* Get the inode count based on the table size */
    fs->inum_count = ntfs->mft_data->size / ntfs->mft_rsize_b + 1;      // we are adding 1 in this calc to account for Orphans directory
    fs->last_inum = fs->inum_count - 1;

    /* reset the flag that we are no longer loading $MFT */
    ntfs->loading_the_MFT = 0;

    /* Volume ID */
    for (fs->fs_id_used = 0; fs->fs_id_used < 8; fs->fs_id_used++) {
        fs->fs_id[fs->fs_id_used] = ntfs->fs->serial[fs->fs_id_used];
    }

    /* load the version of the file system */
    if (ntfs_load_ver(ntfs)) {
        fs->tag = 0;
        tsk_fs_file_close(ntfs->mft_file);
        free(ntfs->mft);
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }

    /* load the data block bitmap data run into ntfs_info */
    if (ntfs_load_bmap(ntfs)) {
        fs->tag = 0;
        tsk_fs_file_close(ntfs->mft_file);
        free(ntfs->mft);
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }

    /* load the SID data into ntfs_info ($Secure - $SDS, $SDH, $SII */
#if TSK_USE_SID
    if (ntfs_load_secure(ntfs)) {
        fs->tag = 0;
        tsk_fs_file_close(ntfs->mft_file);
        free(ntfs->mft);
        free(ntfs->fs);
        free(ntfs);
        return NULL;
    }
#endif


    // initialize the caches
    ntfs->attrdef = NULL;
    fs->list_inum_named = NULL;


    if (tsk_verbose) {
        tsk_fprintf(stderr,
            "ssize: %" PRIu16
            " csize: %d serial: %" PRIx64 "\n",
            tsk_getu16(fs->endian, ntfs->fs->ssize),
            ntfs->fs->csize, tsk_getu64(fs->endian, ntfs->fs->serial));
        tsk_fprintf(stderr,
            "mft_rsize: %d idx_rsize: %d vol: %d mft: %"
            PRIu64 " mft_mir: %" PRIu64 "\n",
            ntfs->mft_rsize_b, ntfs->idx_rsize_b,
            (int) fs->block_count, tsk_getu64(fs->endian,
                ntfs->fs->mft_clust), tsk_getu64(fs->endian,
                ntfs->fs->mftm_clust));
    }

    return fs;
}