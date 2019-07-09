/**
 * @file shm_mod.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief main SHM routines modifying module information
 *
 * @copyright
 * Copyright (c) 2019 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include <libyang/libyang.h>

/**
 * @brief READ/WRITE lock a main SHM module.
 *
 * @param[in] mod_name Module name.
 * @param[in] shm_lock Main SHM module lock.
 * @param[in] timeout_ms Timeout in ms.
 * @param[in] wr Whether to WRITE or READ lock the module.
 * @param[in] sid Sysrepo session ID.
 */
static sr_error_info_t *
sr_shmmod_lock(const char *mod_name, struct sr_mod_lock_s *shm_lock, int timeout_ms, int wr, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    struct timespec timeout_ts;
    int ret;

    assert(timeout_ms > 0);

    sr_time_get(&timeout_ts, timeout_ms);

    /* MUTEX LOCK */
    ret = pthread_mutex_timedlock(&shm_lock->lock.mutex, &timeout_ts);
    if (ret) {
        SR_ERRINFO_LOCK(&err_info, __func__, ret);
        return err_info;
    }

    if (wr) {
        /* write lock */
        ret = 0;
        while (!ret && (shm_lock->lock.readers || ((shm_lock->write_locked || shm_lock->ds_locked) && (shm_lock->sid.sr != sid.sr)))) {
            /* COND WAIT */
            ret = pthread_cond_timedwait(&shm_lock->lock.cond, &shm_lock->lock.mutex, &timeout_ts);
        }

        if (ret) {
            /* MUTEX UNLOCK */
            pthread_mutex_unlock(&shm_lock->lock.mutex);

            if ((ret == ETIMEDOUT) && (shm_lock->write_locked || shm_lock->ds_locked)) {
                /* timeout */
                sr_errinfo_new(&err_info, SR_ERR_LOCKED, NULL, "Module \"%s\" is %s by session %u (NC SID %u).",
                        mod_name, shm_lock->ds_locked ? "locked" : "being used", shm_lock->sid.sr, shm_lock->sid.nc);
            } else {
                /* other error */
                SR_ERRINFO_COND(&err_info, __func__, ret);
            }
            return err_info;
        }
    } else {
        /* read lock */
        ++shm_lock->lock.readers;

        /* MUTEX UNLOCK */
        pthread_mutex_unlock(&shm_lock->lock.mutex);
    }

    return NULL;
}

/**
 * @brief Comparator function for qsort of mod info modules.
 *
 * @param[in] ptr1 First value pointer.
 * @param[in] ptr2 Second value pointer.
 * @return Less than, equal to, or greater than 0 if the first value is found
 * to be less than, equal to, or greater to the second value.
 */
static int
sr_modinfo_qsort_cmp(const void *ptr1, const void *ptr2)
{
    struct sr_mod_info_mod_s *mod1, *mod2;

    mod1 = (struct sr_mod_info_mod_s *)ptr1;
    mod2 = (struct sr_mod_info_mod_s *)ptr2;

    if (mod1->shm_mod > mod2->shm_mod) {
        return 1;
    }
    if (mod1->shm_mod < mod2->shm_mod) {
        return -1;
    }
    return 0;
}

sr_error_info_t *
sr_shmmod_collect_edit(sr_conn_ctx_t *conn, const struct lyd_node *edit, sr_datastore_t ds, struct sr_mod_info_s *mod_info)
{
    sr_mod_t *shm_mod;
    const struct lys_module *mod;
    const struct lyd_node *root;
    sr_error_info_t *err_info = NULL;

    mod_info->ds = ds;
    mod_info->conn = conn;

    /* add all the modules from the edit into our array */
    mod = NULL;
    LY_TREE_FOR(edit, root) {
        if (lyd_node_module(root) == mod) {
            continue;
        }

        /* remember last mod, good chance it will also be the module of some next data nodes */
        mod = lyd_node_module(root);

        /* find the module in SHM and add it with any dependencies */
        shm_mod = sr_shmmain_find_module(&conn->main_shm, conn->ext_shm.addr, mod->name, 0);
        SR_CHECK_INT_RET(!shm_mod, err_info);
        if ((err_info = sr_modinfo_add_mod(shm_mod, mod, MOD_INFO_REQ, MOD_INFO_DEP | MOD_INFO_INV_DEP, mod_info))) {
            return err_info;
        }
    }

    /* sort the modules based on their offsets in the SHM so that we have a uniform order for locking */
    qsort(mod_info->mods, mod_info->mod_count, sizeof *mod_info->mods, sr_modinfo_qsort_cmp);

    return NULL;
}

sr_error_info_t *
sr_shmmod_collect_xpath(sr_conn_ctx_t *conn, const char *xpath, sr_datastore_t ds, struct sr_mod_info_s *mod_info)
{
    sr_mod_t *shm_mod;
    char *module_name;
    const struct lys_module *ly_mod;
    const struct lys_node *ctx_node;
    struct ly_set *set = NULL;
    sr_error_info_t *err_info = NULL;
    uint32_t i;

    mod_info->ds = ds;
    mod_info->conn = conn;

    /* get the module */
    module_name = sr_get_first_ns(xpath);
    if (!module_name) {
        /* there is no module name, use sysrepo module */
        module_name = strdup(SR_YANG_MOD);
        SR_CHECK_MEM_RET(!module_name, err_info);
    }

    ly_mod = ly_ctx_get_module(conn->ly_ctx, module_name, NULL, 1);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "Module \"%s\" was not found in sysrepo.", module_name);
        free(module_name);
        return err_info;
    }
    free(module_name);

    /* take any valid node */
    ctx_node = lys_getnext(NULL, NULL, ly_mod, 0);
    if (!ctx_node) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "No data in module \"%s\".", ly_mod->name);
        return err_info;
    }

    set = lys_xpath_atomize(ctx_node, LYXP_NODE_ELEM, xpath, 0);
    if (!set) {
        sr_errinfo_new_ly(&err_info, conn->ly_ctx);
        SR_ERRINFO_INT(&err_info);
        return err_info;
    }

    /* add all the other modules */
    ly_mod = NULL;
    for (i = 0; i < set->number; ++i) {
        /* skip uninteresting nodes */
        if ((set->set.s[i]->nodetype & (LYS_RPC | LYS_NOTIF))
                || ((set->set.s[i]->flags & LYS_CONFIG_R) && IS_WRITABLE_DS(ds))) {
            continue;
        }

        if (lys_node_module(set->set.s[i]) == ly_mod) {
            /* skip already-added modules */
            continue;
        }
        ly_mod = lys_node_module(set->set.s[i]);

        if (!ly_mod->implemented || !strcmp(ly_mod->name, SR_YANG_MOD) || !strcmp(ly_mod->name, "ietf-netconf")) {
            /* skip import-only modules, the internal sysrepo module, and ietf-netconf (as it has no data, only in libyang) */
            continue;
        }

        /* find the module in SHM and add it with any dependencies */
        shm_mod = sr_shmmain_find_module(&mod_info->conn->main_shm, mod_info->conn->ext_shm.addr, ly_mod->name, 0);
        SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);
        if ((err_info = sr_modinfo_add_mod(shm_mod, ly_mod, MOD_INFO_REQ, MOD_INFO_DEP | MOD_INFO_INV_DEP, mod_info))) {
            goto cleanup;
        }
    }

    /* sort the modules based on their offsets in the SHM so that we have a uniform order for locking */
    qsort(mod_info->mods, mod_info->mod_count, sizeof *mod_info->mods, sr_modinfo_qsort_cmp);

    /* success */

cleanup:
    ly_set_free(set);
    return err_info;
}

sr_error_info_t *
sr_shmmod_collect_modules(sr_conn_ctx_t *conn, const struct lys_module *ly_mod, sr_datastore_t ds, int mod_req_deps,
        struct sr_mod_info_s *mod_info)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;

    mod_info->ds = ds;
    mod_info->conn = conn;

    if (ly_mod) {
        /* only one module */
        shm_mod = sr_shmmain_find_module(&conn->main_shm, conn->ext_shm.addr, ly_mod->name, 0);
        SR_CHECK_INT_RET(!shm_mod, err_info);

        if ((err_info = sr_modinfo_add_mod(shm_mod, ly_mod, MOD_INFO_REQ, mod_req_deps, mod_info))) {
            return err_info;
        }

        return NULL;
    }

    /* all modules */
    SR_SHM_MOD_FOR(conn->main_shm.addr, conn->main_shm.size, shm_mod) {
        ly_mod = ly_ctx_get_module(conn->ly_ctx, conn->ext_shm.addr + shm_mod->name, NULL, 1);
        SR_CHECK_INT_RET(!ly_mod, err_info);

        /* do not collect dependencies, all the modules are added anyway */
        if ((err_info = sr_modinfo_add_mod(shm_mod, ly_mod, MOD_INFO_REQ, 0, mod_info))) {
            return err_info;
        }
    }

    /* we do not need to sort the modules, they were added in the correct order */

    return NULL;
}

sr_error_info_t *
sr_shmmod_collect_op(sr_conn_ctx_t *conn, const char *op_path, const struct lyd_node *op, int output,
        sr_mod_data_dep_t **shm_deps, uint16_t *shm_dep_count, struct sr_mod_info_s *mod_info)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod, *dep_mod;
    sr_mod_op_dep_t *shm_op_deps;
    const struct lys_module *ly_mod;
    uint16_t i;

    mod_info->ds = SR_DS_OPERATIONAL;
    mod_info->conn = conn;

    /* find the module in SHM */
    shm_mod = sr_shmmain_find_module(&conn->main_shm, conn->ext_shm.addr, lyd_node_module(op)->name, 0);
    SR_CHECK_INT_RET(!shm_mod, err_info);

    /* if this is a nested action/notification, we will also need this module's data */
    if (!output && lys_parent(op->schema)) {
        if ((err_info = sr_modinfo_add_mod(shm_mod, lyd_node_module(op), MOD_INFO_REQ, 0, mod_info))) {
            return err_info;
        }
    }

    /* find this operation dependencies */
    shm_op_deps = (sr_mod_op_dep_t *)(conn->ext_shm.addr + shm_mod->op_deps);
    for (i = 0; i < shm_mod->op_dep_count; ++i) {
        if (!strcmp(op_path, conn->ext_shm.addr + shm_op_deps[i].xpath)) {
            break;
        }
    }
    SR_CHECK_INT_RET(i == shm_mod->op_dep_count, err_info);

    /* collect dependencies */
    *shm_deps = (sr_mod_data_dep_t *)(conn->ext_shm.addr + (output ? shm_op_deps[i].out_deps : shm_op_deps[i].in_deps));
    *shm_dep_count = (output ? shm_op_deps[i].out_dep_count : shm_op_deps[i].in_dep_count);
    for (i = 0; i < *shm_dep_count; ++i) {
        if ((*shm_deps)[i].type == SR_DEP_INSTID) {
            /* we will handle those just before validation */
            continue;
        }

        /* find the dependency */
        dep_mod = sr_shmmain_find_module(&conn->main_shm, conn->ext_shm.addr, NULL, (*shm_deps)[i].module);
        SR_CHECK_INT_RET(!dep_mod, err_info);

        /* find ly module */
        ly_mod = ly_ctx_get_module(conn->ly_ctx, conn->ext_shm.addr + dep_mod->name, NULL, 1);
        SR_CHECK_INT_RET(!ly_mod, err_info);

        /* add dependency */
        if ((err_info = sr_modinfo_add_mod(dep_mod, ly_mod, MOD_INFO_DEP, MOD_INFO_DEP, mod_info))) {
            return err_info;
        }
    }

    /* sort the modules based on their offsets in the SHM so that we have a uniform order for locking */
    qsort(mod_info->mods, mod_info->mod_count, sizeof *mod_info->mods, sr_modinfo_qsort_cmp);

    return NULL;
}

sr_error_info_t *
sr_shmmod_modinfo_rdlock(struct sr_mod_info_s *mod_info, int upgradable, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    int mod_wr;
    uint32_t i;
    sr_datastore_t ds;
    struct sr_mod_info_mod_s *mod;
    struct sr_mod_lock_s *shm_lock;

    switch (mod_info->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
    case SR_DS_CANDIDATE:
        ds = mod_info->ds;
        break;
    case SR_DS_OPERATIONAL:
    case SR_DS_STATE:
        /* will use running DS */
        ds = SR_DS_RUNNING;
        break;
    }

    for (i = 0; i < mod_info->mod_count; ++i) {
        mod = &mod_info->mods[i];
        shm_lock = &mod->shm_mod->data_lock_info[ds];

        /* WRITE-lock data-required modules, READ-lock dependency modules */
        mod_wr = upgradable && (mod->state & MOD_INFO_REQ) ? 1 : 0;

        /* MOD READ/WRITE LOCK */
        if ((err_info = sr_shmmod_lock(mod->ly_mod->name, shm_lock, SR_MOD_LOCK_TIMEOUT * 1000, mod_wr, sid))) {
            return err_info;
        }

        if (mod_wr) {
            /* set flag, store SID, and downgrade lock to the required read lock for now */
            assert(!shm_lock->write_locked);
            shm_lock->write_locked = 1;
            shm_lock->sid = sid;

            /* MOD WRITE UNLOCK */
            sr_rwunlock(&shm_lock->lock, 1, __func__);

            /* MOD READ LOCK */
            if ((err_info = sr_shmmod_lock(mod->ly_mod->name, shm_lock, SR_MOD_LOCK_TIMEOUT * 1000, 0, sid))) {
                return err_info;
            }
        }

        /* set the flag for unlocking (it is always READ locked now) */
        mod->state |= MOD_INFO_RLOCK;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmod_modinfo_rdlock_upgrade(struct sr_mod_info_s *mod_info, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i;
    sr_datastore_t ds;
    struct sr_mod_info_mod_s *mod;
    struct sr_mod_lock_s *shm_lock;

    switch (mod_info->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
    case SR_DS_CANDIDATE:
        ds = mod_info->ds;
        break;
    case SR_DS_OPERATIONAL:
    case SR_DS_STATE:
        /* will use running DS */
        ds = SR_DS_RUNNING;
        break;
    }

    for (i = 0; i < mod_info->mod_count; ++i) {
        mod = &mod_info->mods[i];
        shm_lock = &mod->shm_mod->data_lock_info[ds];

        /* upgrade only required modules */
        if ((mod->state & MOD_INFO_REQ) && (mod->state & MOD_INFO_RLOCK)) {
            assert(shm_lock->write_locked);
            assert(!memcmp(&shm_lock->sid, &sid, sizeof sid));

            /* MOD READ UNLOCK */
            sr_rwunlock(&shm_lock->lock, 0, __func__);

            /* remove flag for correct error recovery */
            mod->state &= ~MOD_INFO_RLOCK;

            /* MOD WRITE LOCK */
            if ((err_info = sr_shmmod_lock(mod->ly_mod->name, shm_lock, SR_MOD_LOCK_TIMEOUT * 1000, 1, sid))) {
                return err_info;
            }
            mod->state |= MOD_INFO_WLOCK;
        }
    }

    return NULL;
}

void
sr_shmmod_modinfo_unlock(struct sr_mod_info_s *mod_info, int upgradable)
{
    uint32_t i;
    sr_datastore_t ds;
    struct sr_mod_info_mod_s *mod;
    struct sr_mod_lock_s *shm_lock;

    switch (mod_info->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
    case SR_DS_CANDIDATE:
        ds = mod_info->ds;
        break;
    case SR_DS_OPERATIONAL:
    case SR_DS_STATE:
        /* will use running DS */
        ds = SR_DS_RUNNING;
        break;
    }

    for (i = 0; i < mod_info->mod_count; ++i) {
        mod = &mod_info->mods[i];
        shm_lock = &mod->shm_mod->data_lock_info[ds];

        if ((mod->state & MOD_INFO_REQ) && (mod->state & (MOD_INFO_RLOCK | MOD_INFO_WLOCK)) && upgradable) {
            /* this module's lock was upgraded (WRITE-locked), correctly clean everything */
            assert(shm_lock->write_locked);
            shm_lock->write_locked = 0;
            if (!shm_lock->ds_locked) {
                memset(&shm_lock->sid, 0, sizeof shm_lock->sid);
            }
        }

        if (mod->state & MOD_INFO_WLOCK) {
            /* MOD WRITE UNLOCK */
            sr_rwunlock(&shm_lock->lock, 1, __func__);
        } else if (mod->state & MOD_INFO_RLOCK) {
            /* MOD READ UNLOCK */
            sr_rwunlock(&shm_lock->lock, 0, __func__);
        }
    }
}

void
sr_shmmod_release_locks(sr_conn_ctx_t *conn, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    struct sr_mod_lock_s *shm_lock;
    uint32_t i;

    SR_SHM_MOD_FOR(conn->main_shm.addr, conn->main_shm.size, shm_mod) {
        for (i = 0; i < SR_WRITABLE_DS_COUNT; ++i) {
            shm_lock = &shm_mod->data_lock_info[i];
            if (shm_lock->sid.sr == sid.sr) {
                if (shm_lock->write_locked) {
                    /* this should never happen, write lock is held during some API calls */
                    sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "Session %u (NC SID %u) was working with module \"%s\"!",
                            sid.sr, sid.nc, conn->ext_shm.addr + shm_mod->name);
                    sr_errinfo_free(&err_info);
                    shm_lock->write_locked = 0;
                }
                if (!shm_lock->ds_locked) {
                    /* why our SID matched then? */
                    SR_ERRINFO_INT(&err_info);
                    sr_errinfo_free(&err_info);
                    continue;
                }

                /* unlock */
                shm_lock->ds_locked = 0;
                memset(&shm_lock->sid, 0, sizeof shm_lock->sid);
                shm_lock->ds_ts = 0;
            }
        }
    }
}

sr_error_info_t *
sr_shmmod_conf_subscription_add(sr_shm_t *shm_ext, sr_mod_t *shm_mod, const char *xpath, sr_datastore_t ds,
        uint32_t priority, int sub_opts, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t xpath_off, conf_subs_off;
    sr_mod_conf_sub_t *shm_sub;
    uint32_t new_ext_size;

    assert((ds == SR_DS_RUNNING) || (ds == SR_DS_STARTUP));

    /* moving all existing subscriptions (if any) and adding a new one */
    conf_subs_off = shm_ext->size;
    xpath_off = conf_subs_off + (shm_mod->conf_sub[ds].sub_count + 1) * sizeof *shm_sub;
    new_ext_size = xpath_off + (xpath ? strlen(xpath) + 1 : 0);

    /* remap main ext SHM */
    if ((err_info = sr_shm_remap(shm_ext, new_ext_size))) {
        return err_info;
    }

    /* add wasted memory */
    *((size_t *)shm_ext->addr) += shm_mod->conf_sub[ds].sub_count * sizeof *shm_sub;

    /* move subscriptions */
    memcpy(shm_ext->addr + conf_subs_off, shm_ext->addr + shm_mod->conf_sub[ds].subs,
            shm_mod->conf_sub[ds].sub_count * sizeof *shm_sub);
    shm_mod->conf_sub[ds].subs = conf_subs_off;

    /* fill new subscription */
    shm_sub = (sr_mod_conf_sub_t *)(shm_ext->addr + shm_mod->conf_sub[ds].subs);
    shm_sub += shm_mod->conf_sub[ds].sub_count;
    ++shm_mod->conf_sub[ds].sub_count;

    if (xpath) {
        strcpy(shm_ext->addr + xpath_off, xpath);
        shm_sub->xpath = xpath_off;
    } else {
        shm_sub->xpath = 0;
    }
    shm_sub->priority = priority;
    shm_sub->opts = sub_opts;
    shm_sub->evpipe_num = evpipe_num;

    return NULL;
}

sr_error_info_t *
sr_shmmod_conf_subscription_del(char *ext_shm_addr, sr_mod_t *shm_mod, const char *xpath, sr_datastore_t ds,
        uint32_t priority, int sub_opts, uint32_t evpipe_num, int all_evpipe, int *last_removed)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_conf_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* find the subscription(s) */
    shm_sub = (sr_mod_conf_sub_t *)(ext_shm_addr + shm_mod->conf_sub[ds].subs);
    for (i = 0; i < shm_mod->conf_sub[ds].sub_count; ++i) {
continue_loop:
        if (all_evpipe) {
            if (shm_sub[i].evpipe_num == evpipe_num) {
                break;
            }
        } else if ((!xpath && !shm_sub[i].xpath)
                    || (xpath && shm_sub[i].xpath && !strcmp(ext_shm_addr + shm_sub[i].xpath, xpath))) {
            if ((shm_sub[i].priority == priority) && (shm_sub[i].opts == sub_opts) && (shm_sub[i].evpipe_num == evpipe_num)) {
                break;
            }
        }
    }
    if (all_evpipe && (i == shm_mod->conf_sub[ds].sub_count)) {
        /* all matching subscriptions removed */
        return NULL;
    }
    SR_CHECK_INT_RET(i == shm_mod->conf_sub[ds].sub_count, err_info);

    /* add wasted memory */
    *((size_t *)ext_shm_addr) += sizeof *shm_sub + (shm_sub[i].xpath ? strlen(ext_shm_addr + shm_sub[i].xpath) + 1 : 0);

    --shm_mod->conf_sub[ds].sub_count;
    if (!shm_mod->conf_sub[ds].sub_count) {
        /* the only subscription removed */
        shm_mod->conf_sub[ds].subs = 0;
        if (last_removed) {
            *last_removed = 1;
        }
    } else if (i < shm_mod->conf_sub[ds].sub_count) {
        /* replace the deleted subscription with the last one */
        memcpy(&shm_sub[i], &shm_sub[shm_mod->conf_sub[ds].sub_count], sizeof *shm_sub);
    }

    if (all_evpipe) {
        goto continue_loop;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmod_oper_subscription_add(sr_shm_t *shm_ext, sr_mod_t *shm_mod, const char *xpath, sr_mod_oper_sub_type_t sub_type,
        uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t xpath_off, oper_subs_off;
    sr_mod_oper_sub_t *shm_sub;
    size_t new_ext_size, new_len, cur_len;
    uint16_t i;

    assert(xpath && sub_type);

    /* check that this exact subscription does not exist yet while finding its position */
    new_len = sr_xpath_len_no_predicates(xpath);
    shm_sub = (sr_mod_oper_sub_t *)(shm_ext->addr + shm_mod->oper_subs);
    for (i = 0; i < shm_mod->oper_sub_count; ++i) {
        cur_len = sr_xpath_len_no_predicates(shm_ext->addr + shm_sub[i].xpath);
        if (cur_len > new_len) {
            /* we can insert it at i-th position */
            break;
        }

        if ((cur_len == new_len) && !strcmp(shm_ext->addr + shm_sub[i].xpath, xpath)) {
            sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL,
                    "Data provider subscription for \"%s\" on \"%s\" already exists.", shm_ext->addr + shm_mod->name, xpath);
            return err_info;
        }
    }

    /* get new offsets and SHM size */
    oper_subs_off = shm_ext->size;
    xpath_off = oper_subs_off + (shm_mod->oper_sub_count + 1) * sizeof *shm_sub;
    new_ext_size = xpath_off + (xpath ? strlen(xpath) + 1 : 0);

    /* remap main ext SHM */
    if ((err_info = sr_shm_remap(shm_ext, new_ext_size))) {
        return err_info;
    }

    /* add wasted memory */
    *((size_t *)shm_ext->addr) += shm_mod->oper_sub_count * sizeof *shm_sub;

    /* move preceding and succeeding subscriptions leaving place for the new one */
    if (i) {
        memcpy(shm_ext->addr + oper_subs_off, shm_ext->addr + shm_mod->oper_subs,
                i * sizeof *shm_sub);
    }
    if (i < shm_mod->oper_sub_count) {
        memcpy(shm_ext->addr + oper_subs_off + (i + 1) * sizeof *shm_sub,
                shm_ext->addr + shm_mod->oper_subs + i * sizeof *shm_sub, (shm_mod->oper_sub_count - i) * sizeof *shm_sub);
    }
    shm_mod->oper_subs = oper_subs_off;

    /* fill new subscription */
    shm_sub = (sr_mod_oper_sub_t *)(shm_ext->addr + shm_mod->oper_subs);
    shm_sub += i;
    if (xpath) {
        strcpy(shm_ext->addr + xpath_off, xpath);
        shm_sub->xpath = xpath_off;
    } else {
        shm_sub->xpath = 0;
    }
    shm_sub->sub_type = sub_type;
    shm_sub->evpipe_num = evpipe_num;

    ++shm_mod->oper_sub_count;

    return NULL;
}

sr_error_info_t *
sr_shmmod_oper_subscription_del(char *ext_shm_addr, sr_mod_t *shm_mod, const char *xpath, uint32_t evpipe_num,
        int all_evpipe)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_oper_sub_t *shm_sub;
    uint16_t i;

    /* find the subscription */
    shm_sub = (sr_mod_oper_sub_t *)(ext_shm_addr + shm_mod->oper_subs);
    for (i = 0; i < shm_mod->oper_sub_count; ++i) {
continue_loop:
        if (all_evpipe) {
            if (shm_sub[i].evpipe_num == evpipe_num) {
                break;
            }
        } else if (shm_sub[i].xpath && !strcmp(ext_shm_addr + shm_sub[i].xpath, xpath)) {
            break;
        }
    }
    if (all_evpipe && (i == shm_mod->oper_sub_count)) {
        return NULL;
    }
    SR_CHECK_INT_RET(i == shm_mod->oper_sub_count, err_info);

    /* add wasted memory */
    *((size_t *)ext_shm_addr) += sizeof *shm_sub + strlen(ext_shm_addr + shm_sub[i].xpath) + 1;

    --shm_mod->oper_sub_count;
    if (!shm_mod->oper_sub_count) {
        /* the only subscription removed */
        shm_mod->oper_subs = 0;
    } else {
        /* move all following subscriptions */
        if (i < shm_mod->oper_sub_count) {
            memmove(&shm_sub[i], &shm_sub[i + 1], (shm_mod->oper_sub_count - i) * sizeof *shm_sub);
        }
    }

    if (all_evpipe) {
        goto continue_loop;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmod_notif_subscription_add(sr_shm_t *shm_ext, sr_mod_t *shm_mod, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t notif_subs_off;
    sr_mod_notif_sub_t *shm_sub;
    size_t new_ext_size;

    /* moving all existing subscriptions (if any) and adding a new one */
    notif_subs_off = shm_ext->size;
    new_ext_size = notif_subs_off + (shm_mod->notif_sub_count + 1) * sizeof *shm_sub;

    /* remap main ext SHM */
    if ((err_info = sr_shm_remap(shm_ext, new_ext_size))) {
        return err_info;
    }

    /* add wasted memory */
    *((size_t *)shm_ext->addr) += shm_mod->notif_sub_count * sizeof *shm_sub;

    /* move subscriptions */
    memcpy(shm_ext->addr + notif_subs_off, shm_ext->addr + shm_mod->notif_subs,
            shm_mod->notif_sub_count * sizeof *shm_sub);
    shm_mod->notif_subs = notif_subs_off;

    /* fill new subscription */
    shm_sub = (sr_mod_notif_sub_t *)(shm_ext->addr + shm_mod->notif_subs);
    shm_sub += shm_mod->notif_sub_count;
    shm_sub->evpipe_num = evpipe_num;

    ++shm_mod->notif_sub_count;

    return NULL;
}

sr_error_info_t *
sr_shmmod_notif_subscription_del(char *ext_shm_addr, sr_mod_t *shm_mod, uint32_t evpipe_num, int all_evpipe,
        int *last_removed)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_notif_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* find the subscription */
    shm_sub = (sr_mod_notif_sub_t *)(ext_shm_addr + shm_mod->notif_subs);
    for (i = 0; i < shm_mod->notif_sub_count; ++i) {
continue_loop:
        if (shm_sub[i].evpipe_num == evpipe_num) {
            break;
        }
    }
    if (all_evpipe && (i == shm_mod->notif_sub_count)) {
        return NULL;
    }
    SR_CHECK_INT_RET(i == shm_mod->notif_sub_count, err_info);

    /* add wasted memory */
    *((size_t *)ext_shm_addr) += sizeof *shm_sub;

    --shm_mod->notif_sub_count;
    if (!shm_mod->notif_sub_count) {
        /* the only subscription removed */
        shm_mod->notif_subs = 0;
        if (last_removed) {
            *last_removed = 1;
        }
    } else if (i < shm_mod->notif_sub_count) {
        /* replace the deleted subscription with the last one */
        memcpy(&shm_sub[i], &shm_sub[shm_mod->notif_sub_count], sizeof *shm_sub);
    }

    if (all_evpipe) {
        goto continue_loop;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmod_add_inv_dep(sr_conn_ctx_t *conn, const char *mod_name, off_t inv_dep_mod_name, off_t *shm_end)
{
    sr_error_info_t *err_info = NULL;
    off_t *shm_inv_deps, inv_deps_off;
    sr_mod_t *shm_mod;
    size_t new_ext_size;
    uint32_t i;

    /* find the module */
    shm_mod = sr_shmmain_find_module(&conn->main_shm, conn->ext_shm.addr, mod_name, 0);
    SR_CHECK_INT_RET(!shm_mod, err_info);

    /* check for duplicities */
    shm_inv_deps = (off_t *)(conn->ext_shm.addr + shm_mod->inv_data_deps);
    for (i = 0; i < shm_mod->inv_data_dep_count; ++i) {
        if (shm_inv_deps[i] == inv_dep_mod_name) {
            break;
        }
    }
    if (i < shm_mod->inv_data_dep_count) {
        /* inverse dependency already exists */
        return NULL;
    }

    /* moving all existing inv data deps (if any) and adding a new one */
    inv_deps_off = *shm_end;
    new_ext_size = inv_deps_off + (shm_mod->inv_data_dep_count + 1) * sizeof(off_t);

    /* remap main ext SHM */
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size))) {
        return err_info;
    }

    /* add wasted memory */
    *((size_t *)conn->ext_shm.addr) += shm_mod->inv_data_dep_count * sizeof(off_t);

    /* move existing inverse data deps */
    memcpy(conn->ext_shm.addr + inv_deps_off, conn->ext_shm.addr + shm_mod->inv_data_deps,
           shm_mod->inv_data_dep_count * sizeof(off_t));
    shm_mod->inv_data_deps = inv_deps_off;

    /* fill new inverse data dep */
    shm_inv_deps = (off_t *)(conn->ext_shm.addr + shm_mod->inv_data_deps);
    shm_inv_deps[i] = inv_dep_mod_name;

    ++shm_mod->inv_data_dep_count;
    *shm_end += shm_mod->inv_data_dep_count * sizeof(off_t);
    return NULL;
}
