/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "dht-common.h"
#include "unittest/unittest.h"

#define layout_base_size (sizeof(dht_layout_t))

#define layout_entry_size (sizeof((dht_layout_t *)NULL)->list[0])

#define layout_size(cnt) (layout_base_size + (cnt * layout_entry_size))

dht_layout_t *
dht_layout_new(xlator_t *this, int cnt)
{
    dht_layout_t *layout = NULL;
    dht_conf_t *conf = NULL;

    REQUIRE(NULL != this);
    REQUIRE(cnt >= 0);

    conf = this->private;

    layout = GF_CALLOC(1, layout_size(cnt), gf_dht_mt_dht_layout_t);
    if (!layout) {
        goto out;
    }

    layout->type = DHT_HASH_TYPE_DM;
    layout->cnt = cnt;

    if (conf) {
        layout->spread_cnt = conf->dir_spread_cnt;
        layout->gen = conf->gen;
    }

    GF_ATOMIC_INIT(layout->ref, 1);

    ENSURE(NULL != layout);
    ENSURE(layout->type == DHT_HASH_TYPE_DM);
    ENSURE(layout->cnt == cnt);
    ENSURE(GF_ATOMIC_GET(layout->ref) == 1);
out:
    return layout;
}

dht_layout_t *
dht_layout_get(xlator_t *this, inode_t *inode)
{
    dht_layout_t *layout = NULL;

    dht_inode_ctx_layout_get(inode, this, &layout);

    return layout;
}

int
dht_layout_set(xlator_t *this, inode_t *inode, dht_layout_t *layout)
{
    dht_conf_t *conf = NULL;
    int ret = -1;

    conf = this->private;
    if (!conf || !layout)
        goto out;

    ret = dht_inode_ctx_layout_set(inode, this, layout);

out:
    return ret;
}

void
dht_layout_unref(dht_layout_t *layout)
{
    int ref = 0;

    if (!layout || layout->preset)
        return;

    ref = GF_ATOMIC_DEC(layout->ref);

    if (!ref)
        GF_FREE(layout);
}

dht_layout_t *
dht_layout_ref(dht_layout_t *layout)
{
    if (layout->preset)
        return layout;

    GF_ATOMIC_INC(layout->ref);

    return layout;
}

xlator_t *
dht_layout_search(xlator_t *this, dht_layout_t *layout, const char *name)
{
    uint32_t hash = 0;
    xlator_t *subvol = NULL;
    int i = 0;
    int ret = 0;

    ret = dht_hash_compute(this, layout->type, name, &hash);
    if (ret != 0) {
        gf_smsg(this->name, GF_LOG_WARNING, 0, DHT_MSG_COMPUTE_HASH_FAILED,
                "type=%d", layout->type, "name=%s", name, NULL);
        goto out;
    }

    for (i = 0; i < layout->cnt; i++) {
        if (layout->list[i].start <= hash && layout->list[i].stop >= hash) {
            subvol = layout->list[i].xlator;
            break;
        }
    }

    if (!subvol) {
        gf_smsg(this->name, GF_LOG_WARNING, 0, DHT_MSG_HASHED_SUBVOL_GET_FAILED,
                "hash-value=0x%x", hash, NULL);
    }

out:
    return subvol;
}

dht_layout_t *
dht_layout_for_subvol(xlator_t *this, xlator_t *subvol)
{
    dht_conf_t *conf = NULL;
    dht_layout_t *layout = NULL;
    int i = 0;

    conf = this->private;
    if (!conf)
        goto out;

    for (i = 0; i < conf->subvolume_cnt; i++) {
        if (conf->subvolumes[i] == subvol) {
            layout = conf->file_layouts[i];
            break;
        }
    }

out:
    return layout;
}

int
dht_layouts_init(xlator_t *this, dht_conf_t *conf)
{
    dht_layout_t *layout = NULL;
    int i = 0;
    int ret = -1;

    if (!conf)
        goto out;

    conf->file_layouts = GF_CALLOC(conf->subvolume_cnt, sizeof(dht_layout_t *),
                                   gf_dht_mt_dht_layout_t);
    if (!conf->file_layouts) {
        goto out;
    }

    for (i = 0; i < conf->subvolume_cnt; i++) {
        layout = dht_layout_new(this, 1);

        if (!layout) {
            goto out;
        }

        layout->preset = 1;

        layout->list[0].xlator = conf->subvolumes[i];

        conf->file_layouts[i] = layout;
    }

    ret = 0;
out:
    return ret;
}

int
dht_disk_layout_extract(xlator_t *this, dht_layout_t *layout, int pos,
                        int32_t **disk_layout_p)
{
    int ret = -1;
    int32_t *disk_layout = NULL;

    disk_layout = GF_CALLOC(5, sizeof(int), gf_dht_mt_int32_t);
    if (!disk_layout) {
        goto out;
    }

    disk_layout[0] = htobe32(layout->list[pos].commit_hash);
    disk_layout[1] = htobe32(layout->type);
    disk_layout[2] = htobe32(layout->list[pos].start);
    disk_layout[3] = htobe32(layout->list[pos].stop);

    if (disk_layout_p)
        *disk_layout_p = disk_layout;
    else
        GF_FREE(disk_layout);

    ret = 0;

out:
    return ret;
}

int
dht_disk_layout_extract_for_subvol(xlator_t *this, dht_layout_t *layout,
                                   xlator_t *subvol, int32_t **disk_layout_p)
{
    int i = 0;

    for (i = 0; i < layout->cnt; i++) {
        if (layout->list[i].xlator == subvol)
            break;
    }

    if (i == layout->cnt)
        return -1;

    return dht_disk_layout_extract(this, layout, i, disk_layout_p);
}

static int
dht_disk_layout_merge(xlator_t *this, dht_layout_t *layout, int pos,
                      void *disk_layout_raw, int disk_layout_len)
{
    int type = 0;
    int start_off = 0;
    int stop_off = 0;
    int commit_hash = 0;
    int disk_layout[4];

    if (!disk_layout_raw) {
        gf_smsg(this->name, GF_LOG_CRITICAL, 0, DHT_MSG_LAYOUT_MERGE_FAILED,
                NULL);
        return -1;
    }

    GF_ASSERT(disk_layout_len == sizeof(disk_layout));

    memcpy(disk_layout, disk_layout_raw, disk_layout_len);

    type = be32toh(disk_layout[1]);
    switch (type) {
        case DHT_HASH_TYPE_DM_USER:
            gf_msg_debug(this->name, 0, "found user-set layout");
            layout->type = type;
            /* Fall through. */
        case DHT_HASH_TYPE_DM:
            break;
        default:
            gf_smsg(this->name, GF_LOG_CRITICAL, 0, DHT_MSG_INVALID_DISK_LAYOUT,
                    "layout=%d", disk_layout[1], NULL);
            return -1;
    }

    commit_hash = be32toh(disk_layout[0]);
    start_off = be32toh(disk_layout[2]);
    stop_off = be32toh(disk_layout[3]);

    layout->list[pos].commit_hash = commit_hash;
    layout->list[pos].start = start_off;
    layout->list[pos].stop = stop_off;

    gf_msg_trace(this->name, 0,
                 "merged to layout: 0x%x - 0x%x (hash 0x%x, type %d) from %s",
                 start_off, stop_off, commit_hash, type,
                 layout->list[pos].xlator->name);

    return 0;
}

int
dht_layout_merge(xlator_t *this, dht_layout_t *layout, xlator_t *subvol,
                 int op_ret, int op_errno, dict_t *xattr)
{
    int i = 0;
    int ret = -1;
    int err = -1;
    void *disk_layout_raw = NULL;
    int disk_layout_len = 0;
    dht_conf_t *conf = this->private;

    if (op_ret != 0) {
        err = op_errno;
    }

    if (!layout)
        goto out;

    for (i = 0; i < layout->cnt; i++) {
        if (layout->list[i].xlator == NULL) {
            layout->list[i].err = err;
            layout->list[i].xlator = subvol;
            break;
        }
    }

    if (op_ret != 0) {
        ret = 0;
        goto out;
    }

    if (xattr) {
        /* during lookup and not mkdir */
        ret = dict_get_ptr_and_len(xattr, conf->xattr_name, &disk_layout_raw,
                                   &disk_layout_len);
    }

    if (ret != 0) {
        layout->list[i].err = 0;
        gf_msg_trace(this->name, 0, "Missing disk layout on %s. err = %d",
                     subvol->name, err);
        ret = 0;
        goto out;
    }

    ret = dht_disk_layout_merge(this, layout, i, disk_layout_raw,
                                disk_layout_len);
    if (ret != 0) {
        gf_smsg(this->name, GF_LOG_WARNING, 0, DHT_MSG_LAYOUT_MERGE_FAILED,
                "subvolume=%s", subvol->name, NULL);
        goto out;
    }

    if (layout->commit_hash == 0) {
        layout->commit_hash = layout->list[i].commit_hash;
    } else if (layout->commit_hash != layout->list[i].commit_hash) {
        layout->commit_hash = DHT_LAYOUT_HASH_INVALID;
    }

    layout->list[i].err = 0;

out:
    return ret;
}

void
dht_layout_range_swap(dht_layout_t *layout, int i, int j)
{
    uint32_t start_swap = 0;
    uint32_t stop_swap = 0;

    start_swap = layout->list[i].start;
    stop_swap = layout->list[i].stop;

    layout->list[i].start = layout->list[j].start;
    layout->list[i].stop = layout->list[j].stop;

    layout->list[j].start = start_swap;
    layout->list[j].stop = stop_swap;
}

gf_boolean_t
dht_is_subvol_in_layout(dht_layout_t *layout, xlator_t *xlator)
{
    int i = 0;

    for (i = 0; i < layout->cnt; i++) {
        /* Check if xlator is already part of layout, and layout is
         * non-zero. */
        if (!strcmp(layout->list[i].xlator->name, xlator->name)) {
            if (layout->list[i].start != layout->list[i].stop)
                return _gf_true;
            break;
        }
    }
    return _gf_false;
}

static int
dht_layout_entry_cmp(const void *p, const void *q)
{
    const dht_layout_entry_t *x = p, *y = q;
    int64_t diff = 0;

    if (!y->start && !y->stop)
        diff = (int64_t)x->stop - (int64_t)y->stop;
    else
        diff = (int64_t)x->start - (int64_t)y->start;

    return (diff == 0 ? 0 : (diff < 0 ? -1 : 1));
}

static int
dht_layout_entry_cmp_volname(const void *p, const void *q)
{
    const dht_layout_entry_t *x = p, *y = q;

    return strcmp(x->xlator->name, y->xlator->name);
}

void
dht_layout_sort(dht_layout_t *layout)
{
    qsort(layout->list, layout->cnt, sizeof(dht_layout_entry_t),
          dht_layout_entry_cmp);
}

void
dht_layout_sort_volname(dht_layout_t *layout)
{
    qsort(layout->list, layout->cnt, sizeof(dht_layout_entry_t),
          dht_layout_entry_cmp_volname);
}

void
dht_layout_anomalies(xlator_t *this, loc_t *loc, dht_layout_t *layout,
                     uint32_t *holes_p, uint32_t *overlaps_p,
                     uint32_t *missing_p, uint32_t *down_p, uint32_t *misc_p,
                     uint32_t *no_space_p)
{
    uint32_t missing = 0;
    uint32_t down = 0;
    uint32_t misc = 0;
    uint32_t hole_cnt = 0;
    uint32_t overlap_cnt = 0;
    int i = 0;
    uint32_t prev_stop = 0;
    uint32_t last_stop = 0;
    char is_virgin = 1;
    uint32_t no_space = 0;

    /* This function scans through the layout spread of a directory to
       check if there are any anomalies. Prior to calling this function
       the layout entries should be sorted in the ascending order.

       If the layout entry has err != 0
            then increment the corresponding anomaly.
       else
            if (start of the current layout entry > stop + 1 of previous
               non erroneous layout entry)
                    then it indicates a hole in the layout
            if (start of the current layout entry < stop + 1 of previous
                non erroneous layout entry)
                     then it indicates an overlap in the layout
    */
    last_stop = layout->list[0].start - 1;
    prev_stop = last_stop;

    for (i = 0; i < layout->cnt; i++) {
        switch (layout->list[i].err) {
            case -1:
            case ENOENT:
            case ESTALE:
                missing++;
                continue;
            case ENOTCONN:
                down++;
                continue;
            case ENOSPC:
                no_space++;
                continue;
            case 0:
                /* if err == 0 and start == stop, then it is a non misc++;
                 * participating subvolume(spread-cnt). Then, do not
                 * check for anomalies. If start != stop, then treat it
                 * as misc err */
                if (layout->list[i].start == layout->list[i].stop) {
                    continue;
                }
                break;
            default:
                misc++;
                continue;
        }

        is_virgin = 0;

        if ((prev_stop + 1) < layout->list[i].start) {
            hole_cnt++;
        }

        if ((prev_stop + 1) > layout->list[i].start) {
            overlap_cnt++;
        }
        prev_stop = layout->list[i].stop;
    }

    if ((last_stop - prev_stop) || is_virgin)
        hole_cnt++;

    if (holes_p)
        *holes_p = hole_cnt;

    if (overlaps_p)
        *overlaps_p = overlap_cnt;

    if (missing_p)
        *missing_p = missing;

    if (down_p)
        *down_p = down;

    if (misc_p)
        *misc_p = misc;

    if (no_space_p)
        *no_space_p = no_space;
}

int
dht_layout_missing_dirs(dht_layout_t *layout)
{
    int i = 0, missing = 0;

    if (layout == NULL)
        goto out;

    for (i = 0; i < layout->cnt; i++) {
        if ((layout->list[i].err == ENOENT) ||
            ((layout->list[i].err == -1) && (layout->list[i].start == 0) &&
             (layout->list[i].stop == 0))) {
            missing++;
        }
    }

out:
    return missing;
}

int
dht_layout_normalize(xlator_t *this, loc_t *loc, dht_layout_t *layout)
{
    int ret = 0;
    uint32_t holes = 0;
    uint32_t overlaps = 0;
    uint32_t missing = 0;
    uint32_t down = 0;
    uint32_t misc = 0, missing_dirs = 0;
    char gfid[GF_UUID_BUF_SIZE] = {0};

    dht_layout_sort(layout);

    gf_uuid_unparse(loc->gfid, gfid);

    dht_layout_anomalies(this, loc, layout, &holes, &overlaps, &missing, &down,
                         &misc, NULL);

    if (holes || overlaps) {
        if (missing == layout->cnt) {
            gf_msg_debug(this->name, 0,
                         "Directory %s looked up first time"
                         " gfid = %s",
                         loc->path, gfid);
        } else {
            gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_ANOMALIES_INFO,
                    "path=%s", loc->path, "gfid=%s", gfid, "holes=%d", holes,
                    "overlaps=%d", overlaps, NULL);
        }
        ret = -1;
    }

    if (ret >= 0) {
        missing_dirs = dht_layout_missing_dirs(layout);
        /* TODO During DHT selfheal rewrite (almost) find a better place
         * to detect this - probably in dht_layout_anomalies()
         */
        if (missing_dirs > 0)
            ret += missing_dirs;
    }

    return ret;
}

int
dht_dir_has_layout(dict_t *xattr, char *name)
{
    void *disk_layout_raw = NULL;

    return dict_get_ptr(xattr, name, &disk_layout_raw);
}

int
dht_layout_dir_mismatch(xlator_t *this, dht_layout_t *layout, xlator_t *subvol,
                        loc_t *loc, dict_t *xattr)
{
    int idx = 0;
    int pos = -1;
    int ret = 0;
    int err = 0;
    int dict_ret = 0;
    int32_t disk_layout[4];
    void *disk_layout_raw = NULL;
    uint32_t start_off = -1;
    uint32_t stop_off = -1;
    uint32_t commit_hash = -1;
    dht_conf_t *conf = this->private;
    char gfid[GF_UUID_BUF_SIZE] = {0};

    if (loc && loc->inode)
        gf_uuid_unparse(loc->inode->gfid, gfid);

    for (idx = 0; idx < layout->cnt; idx++) {
        if (layout->list[idx].xlator == subvol) {
            pos = idx;
            break;
        }
    }

    if (pos == -1) {
        if (loc) {
            gf_msg_debug(this->name, 0, "%s - no layout info for subvolume %s",
                         loc ? loc->path : "path not found", subvol->name);
        }
        ret = 1;
        goto out;
    }

    err = layout->list[pos].err;

    if (!xattr) {
        if (err == 0) {
            if (loc) {
                gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_XATTR_DICT_NULL,
                        "path=%s", loc->path, NULL);
            } else {
                gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_XATTR_DICT_NULL,
                        "path not found", NULL);
            }
            ret = -1;
        }
        goto out;
    }

    dict_ret = dict_get_ptr(xattr, conf->xattr_name, &disk_layout_raw);

    if (dict_ret < 0) {
        if (err == 0 && layout->list[pos].stop) {
            if (loc) {
                gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_DISK_LAYOUT_MISSING,
                        "path=%s", loc->path, "gfid=%s", gfid, NULL);
            } else {
                gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_DISK_LAYOUT_MISSING,
                        "path not found"
                        "gfid=%s",
                        gfid, NULL);
            }
            ret = -1;
        }
        goto out;
    }

    memcpy(disk_layout, disk_layout_raw, sizeof(disk_layout));

    start_off = be32toh(disk_layout[2]);
    stop_off = be32toh(disk_layout[3]);
    commit_hash = be32toh(disk_layout[0]);

    if ((layout->list[pos].start != start_off) ||
        (layout->list[pos].stop != stop_off) ||
        (layout->list[pos].commit_hash != commit_hash)) {
        gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_LAYOUT_INFO, "subvol=%s",
                layout->list[pos].xlator->name, "inode-layout:start=0x%x",
                layout->list[pos].start, "inode-layout:stop=0x%x",
                layout->list[pos].stop, "layout-commit-hash=0x%x; ",
                layout->list[pos].commit_hash, "disk-layout:start-off=0x%x",
                start_off, "disk-layout:top-off=0x%x", stop_off,
                "commit-hash=0x%x", commit_hash, NULL);
        ret = 1;
    } else {
        ret = 0;
    }
out:
    return ret;
}

int
dht_layout_preset(xlator_t *this, xlator_t *subvol, inode_t *inode)
{
    dht_layout_t *layout = NULL;
    int ret = -1;
    dht_conf_t *conf = NULL;

    conf = this->private;
    if (!conf)
        goto out;

    layout = dht_layout_for_subvol(this, subvol);
    if (!layout) {
        gf_smsg(this->name, GF_LOG_INFO, 0, DHT_MSG_SUBVOL_NO_LAYOUT_INFO,
                "subvolume=%s", subvol ? subvol->name : "<nil>", NULL);
        ret = -1;
        goto out;
    }

    gf_msg_debug(this->name, 0, "file = %s, subvol = %s",
                 uuid_utoa(inode->gfid), subvol ? subvol->name : "<nil>");

    ret = dht_inode_ctx_layout_set(inode, this, layout);

out:
    return ret;
}

int
dht_layout_index_for_subvol(dht_layout_t *layout, xlator_t *subvol)
{
    int i = 0, ret = -1;

    for (i = 0; i < layout->cnt; i++) {
        if (layout->list[i].xlator == subvol) {
            ret = i;
            break;
        }
    }

    return ret;
}
