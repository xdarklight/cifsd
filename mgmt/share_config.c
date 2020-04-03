// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 */

#include <linux/cred.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/parser.h>
#include <linux/namei.h>
#include <linux/sched.h>

#include "share_config.h"
#include "user_config.h"
#include "user_session.h"
#include "../buffer_pool.h"
#include "../transport_ipc.h"
#include "../ksmbd_server.h" /* FIXME */

#define SHARE_INVALID_UID	((__u16)-1)
#define SHARE_INVALID_GID	((__u16)-1)

#define SHARE_HASH_BITS		3
static DEFINE_HASHTABLE(shares_table, SHARE_HASH_BITS);
static DECLARE_RWSEM(shares_table_lock);

struct ksmbd_veto_pattern {
	char			*pattern;
	struct list_head	list;
};

static unsigned int share_name_hash(char *name)
{
	return jhash(name, strlen(name), 0);
}

static void kill_share(struct ksmbd_share_config *share)
{
	while (!list_empty(&share->veto_list)) {
		struct ksmbd_veto_pattern *p;

		p = list_entry(share->veto_list.next,
			       struct ksmbd_veto_pattern,
			       list);
		list_del(&p->list);
		kfree(p->pattern);
		kfree(p);
	}

	if (share->path)
		path_put(&share->vfs_path);
	kfree(share->name);
	kfree(share->path);
	kfree(share);
}

static void deferred_share_free(struct work_struct *work)
{
	struct ksmbd_share_config *share = container_of(work,
					       struct ksmbd_share_config,
					       free_work);

	kill_share(share);
}

void __ksmbd_share_config_put(struct ksmbd_share_config *share)
{
	down_write(&shares_table_lock);
	hash_del(&share->hlist);
	up_write(&shares_table_lock);

	schedule_work(&share->free_work);
}

static struct ksmbd_share_config *
__get_share_config(struct ksmbd_share_config *share)
{
	if (!atomic_inc_not_zero(&share->refcount))
		return NULL;
	return share;
}

static struct ksmbd_share_config *__share_lookup(char *name)
{
	struct ksmbd_share_config *share;
	unsigned int key = share_name_hash(name);

	hash_for_each_possible(shares_table, share, hlist, key) {
		if (!strcmp(name, share->name))
			return share;
	}
	return NULL;
}

static int parse_veto_list(struct ksmbd_share_config *share,
			   char *veto_list,
			   int veto_list_sz)
{
	int sz = 0;

	if (!veto_list_sz)
		return 0;

	while (veto_list_sz > 0) {
		struct ksmbd_veto_pattern *p;

		p = ksmbd_alloc(sizeof(struct ksmbd_veto_pattern));
		if (!p)
			return -ENOMEM;

		sz = strlen(veto_list);
		if (!sz)
			break;

		p->pattern = kstrdup(veto_list, GFP_KERNEL);
		if (!p->pattern) {
			ksmbd_free(p);
			return -ENOMEM;
		}

		list_add(&p->list, &share->veto_list);

		veto_list += sz + 1;
		veto_list_sz -= (sz + 1);
	}

	return 0;
}

static struct ksmbd_share_config *share_config_request(char *name)
{
	struct ksmbd_share_config_response *resp;
	struct ksmbd_share_config *share = NULL;
	struct ksmbd_share_config *lookup;
	int ret;

	resp = ksmbd_ipc_share_config_request(name);
	if (!resp)
		return NULL;

	if (resp->flags == KSMBD_SHARE_FLAG_INVALID)
		goto out;

	share = ksmbd_alloc(sizeof(struct ksmbd_share_config));
	if (!share)
		goto out;

	share->flags = resp->flags;
	atomic_set(&share->refcount, 1);
	INIT_WORK(&share->free_work, deferred_share_free);
	INIT_LIST_HEAD(&share->veto_list);
	share->name = kstrdup(name, GFP_KERNEL);

	if (!test_share_config_flag(share, KSMBD_SHARE_FLAG_PIPE)) {
		share->path = kstrdup(KSMBD_SHARE_CONFIG_PATH(resp),
				      GFP_KERNEL);
		if (share->path)
			share->path_sz = strlen(share->path);
		share->create_mask = resp->create_mask;
		share->directory_mask = resp->directory_mask;
		share->force_create_mode = resp->force_create_mode;
		share->force_directory_mode = resp->force_directory_mode;
		share->force_uid = resp->force_uid;
		share->force_gid = resp->force_gid;
		ret = parse_veto_list(share,
				      KSMBD_SHARE_CONFIG_VETO_LIST(resp),
				      resp->veto_list_sz);
		if (!ret && share->path) {
			ret = kern_path(share->path, 0, &share->vfs_path);
			if (ret) {
				ksmbd_debug("failed to access '%s'\n",
					share->path);
				/* Avoid put_path() */
				kfree(share->path);
				share->path = NULL;
			}
		}
		if (ret || !share->name) {
			kill_share(share);
			share = NULL;
			goto out;
		}
	}

	down_write(&shares_table_lock);
	lookup = __share_lookup(name);
	if (lookup)
		lookup = __get_share_config(lookup);
	if (!lookup) {
		hash_add(shares_table, &share->hlist, share_name_hash(name));
	} else {
		kill_share(share);
		share = lookup;
	}
	up_write(&shares_table_lock);

out:
	ksmbd_free(resp);
	return share;
}

static void strtolower(char *share_name)
{
	while (*share_name) {
		*share_name = tolower(*share_name);
		share_name++;
	}
}

struct ksmbd_share_config *ksmbd_share_config_get(char *name)
{
	struct ksmbd_share_config *share;

	strtolower(name);

	down_read(&shares_table_lock);
	share = __share_lookup(name);
	if (share)
		share = __get_share_config(share);
	up_read(&shares_table_lock);

	if (share)
		return share;
	return share_config_request(name);
}

bool ksmbd_share_veto_filename(struct ksmbd_share_config *share,
			       const char *filename)
{
	struct ksmbd_veto_pattern *p;

	list_for_each_entry(p, &share->veto_list, list) {
		if (match_wildcard(p->pattern, filename))
			return true;
	}
	return false;
}

void ksmbd_share_configs_cleanup(void)
{
	struct ksmbd_share_config *share;
	struct hlist_node *tmp;
	int i;

	down_write(&shares_table_lock);
	hash_for_each_safe(shares_table, i, tmp, share, hlist) {
		hash_del(&share->hlist);
		kill_share(share);
	}
	up_write(&shares_table_lock);
}

const struct cred *ksmbd_override_fsids(struct ksmbd_session *sess,
		struct ksmbd_share_config *share)
{
	struct cred *cred;
	unsigned int uid = user_uid(sess->user);
	unsigned int gid = user_gid(sess->user);

	if (share->force_uid != SHARE_INVALID_UID)
		uid = share->force_uid;
	if (share->force_gid != SHARE_INVALID_GID)
		gid = share->force_gid;

	cred = prepare_kernel_cred(NULL);
	if (!cred)
		return ERR_PTR(-ENOMEM);

	cred->fsuid = make_kuid(current_user_ns(), uid);
	cred->fsgid = make_kgid(current_user_ns(), gid);
	if (!uid_eq(cred->fsuid, GLOBAL_ROOT_UID))
		cred->cap_effective = cap_drop_fs_set(cred->cap_effective);

	return override_creds(cred);
}

void ksmbd_revert_fsids(const struct cred *old_cred)
{
	if (!IS_ERR_OR_NULL(old_cred)) {
		const struct cred *cred;

		cred = current->cred;
		revert_creds(old_cred);
		put_cred(cred);
	}
}
