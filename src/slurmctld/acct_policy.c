/*****************************************************************************\
 *  acct_policy.c - Enforce accounting policy
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/slurm_accounting_storage.h"

#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/acct_policy.h"
#include "src/common/node_select.h"
#include "src/common/slurm_priority.h"

#define _DEBUG 0

enum {
	ACCT_POLICY_ADD_SUBMIT,
	ACCT_POLICY_REM_SUBMIT,
	ACCT_POLICY_JOB_BEGIN,
	ACCT_POLICY_JOB_FINI
};

static void _set_qos_order(struct job_record *job_ptr,
			   slurmdb_qos_rec_t **qos_ptr_1,
			   slurmdb_qos_rec_t **qos_ptr_2)
{
	xassert(job_ptr);
	xassert(qos_ptr_1);
	xassert(qos_ptr_2);

	/* Initialize incoming pointers */
	*qos_ptr_1 = *qos_ptr_2 = NULL;

	if (job_ptr->qos_ptr) {
		if (job_ptr->part_ptr && job_ptr->part_ptr->qos_ptr) {
			/* If the job's QOS has the flag to over ride the
			 * partition then use that otherwise use the
			 * partition's QOS as the king.
			 */
			if (((slurmdb_qos_rec_t *)job_ptr->qos_ptr)->flags
			    & QOS_FLAG_PART_QOS) {
				*qos_ptr_1 = job_ptr->qos_ptr;
				*qos_ptr_2 = job_ptr->part_ptr->qos_ptr;
			} else {
				*qos_ptr_1 = job_ptr->part_ptr->qos_ptr;
				*qos_ptr_2 = job_ptr->qos_ptr;
			}

			/* No reason to look at the same QOS twice, actually
			 * we never want to do that ;). */
			if (*qos_ptr_1 == *qos_ptr_2)
				*qos_ptr_2 = NULL;
		} else
			*qos_ptr_1 = job_ptr->qos_ptr;
	} else if (job_ptr->part_ptr && job_ptr->part_ptr->qos_ptr)
		*qos_ptr_1 = job_ptr->part_ptr->qos_ptr;

	return;
}

static slurmdb_used_limits_t *_get_used_limits_for_user(
	List user_limit_list, uint32_t user_id)
{
	slurmdb_used_limits_t *used_limits = NULL;
	ListIterator itr = NULL;

	if (!user_limit_list)
		return NULL;

	itr = list_iterator_create(user_limit_list);
	while ((used_limits = list_next(itr))) {
		if (used_limits->uid == user_id)
			break;
	}
	list_iterator_destroy(itr);

	return used_limits;
}

static bool _valid_job_assoc(struct job_record *job_ptr)
{
	slurmdb_assoc_rec_t assoc_rec, *assoc_ptr;

	assoc_ptr = (slurmdb_assoc_rec_t *)job_ptr->assoc_ptr;
	if ((assoc_ptr == NULL) ||
	    (assoc_ptr->id  != job_ptr->assoc_id) ||
	    (assoc_ptr->uid != job_ptr->user_id)) {
		error("Invalid assoc_ptr for jobid=%u", job_ptr->job_id);
		memset(&assoc_rec, 0, sizeof(slurmdb_assoc_rec_t));

		assoc_rec.acct      = job_ptr->account;
		if (job_ptr->part_ptr)
			assoc_rec.partition = job_ptr->part_ptr->name;
		assoc_rec.uid       = job_ptr->user_id;

		if (assoc_mgr_fill_in_assoc(acct_db_conn, &assoc_rec,
					    accounting_enforce,
					    (slurmdb_assoc_rec_t **)
					    &job_ptr->assoc_ptr, false)) {
			info("_validate_job_assoc: invalid account or "
			     "partition for uid=%u jobid=%u",
			     job_ptr->user_id, job_ptr->job_id);
			return false;
		}
		job_ptr->assoc_id = assoc_rec.id;
	}
	return true;
}

static void _qos_adjust_limit_usage(int type, struct job_record *job_ptr,
				    slurmdb_qos_rec_t *qos_ptr,
				    uint32_t node_cnt,
				    uint64_t used_cpu_run_secs,
				    uint32_t job_memory)
{
	slurmdb_used_limits_t *used_limits = NULL;

	if (!qos_ptr)
		return;

	if (!qos_ptr->usage->user_limit_list)
		qos_ptr->usage->user_limit_list =
			list_create(slurmdb_destroy_used_limits);
	used_limits = _get_used_limits_for_user(qos_ptr->usage->user_limit_list,
						job_ptr->user_id);

	if (!used_limits) {
		used_limits = xmalloc(sizeof(slurmdb_used_limits_t));
		used_limits->uid = job_ptr->user_id;
		list_append(qos_ptr->usage->user_limit_list,
			    used_limits);
	}

	switch(type) {
	case ACCT_POLICY_ADD_SUBMIT:
		qos_ptr->usage->grp_used_submit_jobs++;
		used_limits->submit_jobs++;
		break;
	case ACCT_POLICY_REM_SUBMIT:
		if (qos_ptr->usage->grp_used_submit_jobs)
			qos_ptr->usage->grp_used_submit_jobs--;
		else
			debug2("acct_policy_remove_job_submit: "
			       "grp_submit_jobs underflow for qos %s",
			       qos_ptr->name);

		if (used_limits->submit_jobs)
			used_limits->submit_jobs--;
		else
			debug2("acct_policy_remove_job_submit: "
			       "used_submit_jobs underflow for "
			       "qos %s user %d",
			       qos_ptr->name, used_limits->uid);
		break;
	case ACCT_POLICY_JOB_BEGIN:
		qos_ptr->usage->grp_used_jobs++;
		qos_ptr->usage->grp_used_cpus += job_ptr->total_cpus;
		qos_ptr->usage->grp_used_mem += job_memory;
		qos_ptr->usage->grp_used_nodes += node_cnt;
		qos_ptr->usage->grp_used_cpu_run_secs +=
			used_cpu_run_secs;
		used_limits->jobs++;
		used_limits->cpus += job_ptr->total_cpus;
		used_limits->nodes += node_cnt;
		break;
	case ACCT_POLICY_JOB_FINI:
		qos_ptr->usage->grp_used_jobs--;
		if ((int32_t)qos_ptr->usage->grp_used_jobs < 0) {
			qos_ptr->usage->grp_used_jobs = 0;
			debug2("acct_policy_job_fini: used_jobs "
			       "underflow for qos %s", qos_ptr->name);
		}

		qos_ptr->usage->grp_used_cpus -= job_ptr->total_cpus;
		if ((int32_t)qos_ptr->usage->grp_used_cpus < 0) {
			qos_ptr->usage->grp_used_cpus = 0;
			debug2("acct_policy_job_fini: grp_used_cpus "
			       "underflow for qos %s", qos_ptr->name);
		}

		qos_ptr->usage->grp_used_mem -= job_memory;
		if ((int32_t)qos_ptr->usage->grp_used_mem < 0) {
			qos_ptr->usage->grp_used_mem = 0;
			debug2("acct_policy_job_fini: grp_used_mem "
			       "underflow for qos %s", qos_ptr->name);
		}

		qos_ptr->usage->grp_used_nodes -= node_cnt;
		if ((int32_t)qos_ptr->usage->grp_used_nodes < 0) {
			qos_ptr->usage->grp_used_nodes = 0;
			debug2("acct_policy_job_fini: grp_used_nodes "
			       "underflow for qos %s", qos_ptr->name);
		}

		used_limits->cpus -= job_ptr->total_cpus;
		if ((int32_t)used_limits->cpus < 0) {
			used_limits->cpus = 0;
			debug2("acct_policy_job_fini: "
			       "used_limits->cpus "
			       "underflow for qos %s user %d",
			       qos_ptr->name, used_limits->uid);
		}

		used_limits->jobs--;
		if ((int32_t)used_limits->jobs < 0) {
			used_limits->jobs = 0;
			debug2("acct_policy_job_fini: used_jobs "
			       "underflow for qos %s user %d",
			       qos_ptr->name, used_limits->uid);
		}

		used_limits->nodes -= node_cnt;
		if ((int32_t)used_limits->nodes < 0) {
			used_limits->nodes = 0;
			debug2("acct_policy_job_fini: "
			       "used_limits->nodes"
			       "underflow for qos %s user %d",
			       qos_ptr->name, used_limits->uid);
		}

		break;
	default:
		error("acct_policy: qos unknown type %d", type);
		break;
	}

}

static void _adjust_limit_usage(int type, struct job_record *job_ptr)
{
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_assoc_rec_t *assoc_ptr = NULL;
	assoc_mgr_lock_t locks = { WRITE_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };
	uint64_t used_cpu_run_secs = 0;
	uint32_t job_memory = 0;
	uint32_t node_cnt;

	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS)
	    || !_valid_job_assoc(job_ptr))
		return;
#ifdef HAVE_BG
	xassert(job_ptr->select_jobinfo);
	select_g_select_jobinfo_get(job_ptr->select_jobinfo,
				    SELECT_JOBDATA_NODE_CNT, &node_cnt);
	if (node_cnt == NO_VAL) {
		/* This should never happen */
		node_cnt = job_ptr->node_cnt;
		error("node_cnt not available at %s:%d\n", __FILE__, __LINE__);
	}
#else
	node_cnt = job_ptr->node_cnt;
#endif

	if (type == ACCT_POLICY_JOB_FINI)
		priority_g_job_end(job_ptr);
	else if (type == ACCT_POLICY_JOB_BEGIN)
		used_cpu_run_secs = (uint64_t)job_ptr->total_cpus
			* (uint64_t)job_ptr->time_limit * 60;

	if (job_ptr->details && job_ptr->details->pn_min_memory) {
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			job_memory = (job_ptr->details->pn_min_memory
				      & (~MEM_PER_CPU))
				* job_ptr->total_cpus;
			debug2("_adjust_limit_usage: job %u: MPC: "
			       "job_memory set to %u", job_ptr->job_id,
			       job_memory);
		} else {
			job_memory = (job_ptr->details->pn_min_memory)
				* node_cnt;
			debug2("_adjust_limit_usage: job %u: MPN: "
			       "job_memory set to %u", job_ptr->job_id,
			       job_memory);
		}
	}

	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	_qos_adjust_limit_usage(type, job_ptr, qos_ptr_1,
				node_cnt, used_cpu_run_secs, job_memory);
	_qos_adjust_limit_usage(type, job_ptr, qos_ptr_2,
				node_cnt, used_cpu_run_secs, job_memory);

	assoc_ptr = (slurmdb_assoc_rec_t *)job_ptr->assoc_ptr;
	while (assoc_ptr) {
		switch(type) {
		case ACCT_POLICY_ADD_SUBMIT:
			assoc_ptr->usage->used_submit_jobs++;
			break;
		case ACCT_POLICY_REM_SUBMIT:
			if (assoc_ptr->usage->used_submit_jobs)
				assoc_ptr->usage->used_submit_jobs--;
			else
				debug2("acct_policy_remove_job_submit: "
				       "used_submit_jobs underflow for "
				       "account %s",
				       assoc_ptr->acct);
			break;
		case ACCT_POLICY_JOB_BEGIN:
			assoc_ptr->usage->used_jobs++;
			assoc_ptr->usage->grp_used_cpus += job_ptr->total_cpus;
			assoc_ptr->usage->grp_used_mem += job_memory;
			assoc_ptr->usage->grp_used_nodes += node_cnt;
			assoc_ptr->usage->grp_used_cpu_run_secs +=
				used_cpu_run_secs;
			debug4("acct_policy_job_begin: after adding job %i, "
			       "assoc %s grp_used_cpu_run_secs is %"PRIu64"",
			       job_ptr->job_id, assoc_ptr->acct,
			       assoc_ptr->usage->grp_used_cpu_run_secs);
			break;
		case ACCT_POLICY_JOB_FINI:
			if (assoc_ptr->usage->used_jobs)
				assoc_ptr->usage->used_jobs--;
			else
				debug2("acct_policy_job_fini: used_jobs "
				       "underflow for account %s",
				       assoc_ptr->acct);

			assoc_ptr->usage->grp_used_cpus -= job_ptr->total_cpus;
			if ((int32_t)assoc_ptr->usage->grp_used_cpus < 0) {
				assoc_ptr->usage->grp_used_cpus = 0;
				debug2("acct_policy_job_fini: grp_used_cpus "
				       "underflow for account %s",
				       assoc_ptr->acct);
			}

			assoc_ptr->usage->grp_used_mem -= job_memory;
			if ((int32_t)assoc_ptr->usage->grp_used_mem < 0) {
				assoc_ptr->usage->grp_used_mem = 0;
				debug2("acct_policy_job_fini: grp_used_mem "
				       "underflow for account %s",
				       assoc_ptr->acct);
			}

			assoc_ptr->usage->grp_used_nodes -= node_cnt;
			if ((int32_t)assoc_ptr->usage->grp_used_nodes < 0) {
				assoc_ptr->usage->grp_used_nodes = 0;
				debug2("acct_policy_job_fini: grp_used_nodes "
				       "underflow for account %s",
				       assoc_ptr->acct);
			}

			break;
		default:
			error("acct_policy: association unknown type %d", type);
			break;
		}
		/* now handle all the group limits of the parents */
		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
	}
	assoc_mgr_unlock(&locks);
}

static void _qos_alter_job(struct job_record *job_ptr,
			   slurmdb_qos_rec_t *qos_ptr,
			   uint64_t used_cpu_run_secs,
			   uint64_t new_used_cpu_run_secs)
{
	if (!qos_ptr || !job_ptr)
		return;

	qos_ptr->usage->grp_used_cpu_run_secs -=
		used_cpu_run_secs;
	qos_ptr->usage->grp_used_cpu_run_secs +=
		new_used_cpu_run_secs;
	debug2("altering %u QOS %s got %"PRIu64" "
	       "just removed %"PRIu64" and added %"PRIu64"",
	       job_ptr->job_id,
	       qos_ptr->name,
	       qos_ptr->usage->grp_used_cpu_run_secs,
	       used_cpu_run_secs,
	       new_used_cpu_run_secs);
}

static int _qos_policy_validate(job_desc_msg_t *job_desc,
				struct part_record *part_ptr,
				slurmdb_qos_rec_t *qos_ptr,
				slurmdb_qos_rec_t *qos_out_ptr,
				uint32_t *reason,
				acct_policy_limit_set_t *acct_policy_limit_set,
				bool update_call,
				char *user_name,
				uint32_t job_memory,
				int job_cnt,
				bool strict_checking)
{
	uint32_t qos_max_cpus_limit = INFINITE;
	uint32_t qos_max_nodes_limit = INFINITE;
	uint32_t qos_time_limit = INFINITE;
	uint32_t qos_out_max_cpus_limit = INFINITE;
	uint32_t qos_out_max_nodes_limit = INFINITE;
	int rc = true;

	if (!qos_ptr || !qos_out_ptr)
		return rc;

	/* for validation we don't need to look at
	 * qos_ptr->grp_cpu_mins.
	 */
	qos_max_cpus_limit =
		MIN(qos_ptr->grp_cpus, qos_ptr->max_cpus_pu);
	qos_out_max_cpus_limit =
		MIN(qos_out_ptr->grp_cpus, qos_out_ptr->max_cpus_pu);

	if ((acct_policy_limit_set->max_tres[TRES_ARRAY_CPU] == ADMIN_SET_LIMIT)
	    || (qos_out_max_cpus_limit != INFINITE)
	    || (qos_max_cpus_limit == INFINITE)
	    || (update_call &&
		(job_desc->tres_req_cnt[TRES_ARRAY_CPU] == (uint64_t)NO_VAL))) {
		/* no need to check/set */

	} else if (strict_checking &&
		   (job_desc->tres_req_cnt[TRES_ARRAY_CPU]
		    != (uint64_t)NO_VAL)) {

		if (qos_out_ptr->max_cpus_pu == INFINITE)
			qos_out_ptr->max_cpus_pu = qos_ptr->max_cpus_pu;
		if (qos_out_ptr->grp_cpus == INFINITE)
			qos_out_ptr->grp_cpus = qos_ptr->grp_cpus;

		if (job_desc->tres_req_cnt[TRES_ARRAY_CPU] >
		    qos_ptr->max_cpus_pu) {
			if (reason)
				*reason = WAIT_QOS_MAX_CPU_PER_USER;

			debug2("job submit for user %s(%u): "
			       "min cpu request %"PRIu64" exceeds "
			       "per-user max cpu limit %u for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       job_desc->tres_req_cnt[TRES_ARRAY_CPU],
			       qos_ptr->max_cpus_pu,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		} else if (job_desc->tres_req_cnt[TRES_ARRAY_CPU] >
			   qos_ptr->grp_cpus) {
			if (reason)
				*reason = WAIT_QOS_GRP_CPU;

			debug2("job submit for user %s(%u): "
			       "min cpu request %"PRIu64" exceeds "
			       "group max cpu limit %u for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       job_desc->tres_req_cnt[TRES_ARRAY_CPU],
			       qos_ptr->grp_cpus,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* for validation we don't need to look at
	 * qos_ptr->grp_jobs.
	 */
	if (!acct_policy_limit_set->max_tres[TRES_ARRAY_MEM] && strict_checking
	    && (qos_out_ptr->grp_mem == INFINITE)
	    && (qos_ptr->grp_mem != INFINITE)) {

		qos_out_ptr->grp_mem = qos_ptr->grp_mem;

		if (job_memory > qos_ptr->grp_mem) {
			if (reason)
				*reason = WAIT_QOS_GRP_MEMORY;
			debug2("job submit for user %s(%u): "
			       "min memory request %u exceeds "
			       "group max memory limit %u for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       job_memory,
			       qos_ptr->grp_mem,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	qos_max_nodes_limit =
		MIN(qos_ptr->grp_nodes, qos_ptr->max_nodes_pu);
	qos_out_max_nodes_limit =
		MIN(qos_out_ptr->grp_nodes, qos_out_ptr->max_nodes_pu);

	if ((acct_policy_limit_set->max_nodes == ADMIN_SET_LIMIT)
	    || (qos_out_max_nodes_limit != INFINITE)
	    || (qos_max_nodes_limit == INFINITE)
	    || (update_call && (job_desc->max_nodes == NO_VAL))) {
		/* no need to check/set */
	} else if (strict_checking && (job_desc->min_nodes != NO_VAL)) {

		if (qos_out_ptr->max_nodes_pu == INFINITE)
			qos_out_ptr->max_nodes_pu = qos_ptr->max_nodes_pu;
		if (qos_out_ptr->grp_nodes == INFINITE)
			qos_out_ptr->grp_nodes = qos_ptr->grp_nodes;

		if (job_desc->min_nodes > qos_ptr->max_nodes_pu) {
			/* MaxNodesPerUser */
			if (reason)
				*reason = WAIT_QOS_MAX_NODE_PER_USER;
			debug2("job submit for user %s(%u): "
			       "min node request %u exceeds "
			       "per-user max node limit %u for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       job_desc->min_nodes,
			       qos_ptr->max_nodes_pu,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		} else if (job_desc->min_nodes > qos_ptr->grp_nodes) {
			if (reason)
				*reason = WAIT_QOS_GRP_NODES;
			debug2("job submit for user %s(%u): "
			       "min node request %u exceeds "
			       "group max node limit %u for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       job_desc->min_nodes,
			       qos_ptr->grp_nodes,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	if ((qos_out_ptr->grp_submit_jobs == INFINITE) &&
	    (qos_ptr->grp_submit_jobs != INFINITE)) {

		qos_out_ptr->grp_submit_jobs = qos_ptr->grp_submit_jobs;

		if ((qos_ptr->usage->grp_used_submit_jobs + job_cnt)
		    > qos_ptr->grp_submit_jobs) {
			if (reason)
				*reason = WAIT_QOS_GRP_SUB_JOB;
			debug2("job submit for user %s(%u): "
			       "group max submit job limit exceeded %u "
			       "for qos '%s'",
			       user_name,
			       job_desc->user_id,
			       qos_ptr->grp_submit_jobs,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}


	/* for validation we don't need to look at
	 * qos_ptr->grp_wall. It is checked while the job is running.
	 */


	/* we do need to check qos_ptr->max_cpu_mins_pj.
	 * if you can end up in PENDING QOSJobLimit, you need
	 * to validate it if DenyOnLimit is set
	 */
	if (((job_desc->tres_req_cnt[TRES_ARRAY_CPU]  != NO_VAL) ||
	     (job_desc->min_nodes != NO_VAL)) &&
	    (qos_out_ptr->max_cpu_mins_pj == INFINITE) &&
	    (qos_ptr->max_cpu_mins_pj != INFINITE)) {
		uint32_t cpu_cnt = job_desc->min_nodes;

		qos_out_ptr->max_cpu_mins_pj = qos_ptr->max_cpu_mins_pj;

		if ((job_desc->min_nodes == NO_VAL) ||
		    (job_desc->tres_req_cnt[TRES_ARRAY_CPU] > job_desc->min_nodes))
			cpu_cnt = job_desc->tres_req_cnt[TRES_ARRAY_CPU];
		qos_time_limit = qos_ptr->max_cpu_mins_pj / cpu_cnt;
	}

	if ((acct_policy_limit_set->max_tres[TRES_ARRAY_CPU] == ADMIN_SET_LIMIT)
	    || (qos_out_ptr->max_cpus_pj |= INFINITE)
	    || (qos_ptr->max_cpus_pj == INFINITE)
	    || (update_call &&
		(job_desc->tres_req_cnt[TRES_ARRAY_CPU] == (uint64_t)NO_VAL))) {
		/* no need to check/set */
	} else if (strict_checking &&
		   (job_desc->tres_req_cnt[TRES_ARRAY_CPU] != (uint64_t)NO_VAL)) {

		qos_out_ptr->max_cpus_pj = qos_ptr->max_cpus_pj;

		if (job_desc->tres_req_cnt[TRES_ARRAY_CPU] > qos_ptr->max_cpus_pj) {
			if (reason)
				*reason = WAIT_QOS_MAX_CPUS_PER_JOB;
			debug2("job submit for user %s(%u): "
			       "min cpu limit %"PRIu64" exceeds "
			       "qos max %u",
			       user_name,
			       job_desc->user_id,
			       job_desc->tres_req_cnt[TRES_ARRAY_CPU],
			       qos_ptr->max_cpus_pj);
			rc = false;
			goto end_it;
		}
	}

	/* for validation we don't need to look at
	 * qos_ptr->max_jobs.
	 */

	if ((acct_policy_limit_set->max_nodes == ADMIN_SET_LIMIT)
	    || (qos_out_ptr->max_nodes_pj != INFINITE)
	    || (qos_ptr->max_nodes_pj == INFINITE)
	    || (update_call && (job_desc->max_nodes == NO_VAL))) {
		/* no need to check/set */
	} else if (strict_checking && (job_desc->min_nodes != NO_VAL)) {

		qos_out_ptr->max_nodes_pj = qos_ptr->max_nodes_pj;

		if (job_desc->min_nodes > qos_ptr->max_nodes_pj) {
			if (reason)
				*reason = WAIT_QOS_MAX_NODE_PER_JOB;
			debug2("job submit for user %s(%u): "
			       "min node limit %u exceeds "
			       "qos max %u",
			       user_name,
			       job_desc->user_id,
			       job_desc->min_nodes,
			       qos_ptr->max_nodes_pj);
			rc = false;
			goto end_it;
		}
	}

	if ((qos_out_ptr->max_submit_jobs_pu == INFINITE) &&
	    (qos_ptr->max_submit_jobs_pu != INFINITE)) {
		slurmdb_used_limits_t *used_limits = _get_used_limits_for_user(
			qos_ptr->usage->user_limit_list, job_desc->user_id);

		qos_out_ptr->max_submit_jobs_pu = qos_ptr->max_submit_jobs_pu;

		if ((!used_limits &&
		     qos_ptr->max_submit_jobs_pu == 0) ||
		    (used_limits &&
		     ((used_limits->submit_jobs + job_cnt) >
		      qos_ptr->max_submit_jobs_pu))) {
			if (reason)
				*reason = WAIT_QOS_MAX_SUB_JOB;
			debug2("job submit for user %s(%u): "
			       "qos max submit job limit exceeded %u",
			       user_name,
			       job_desc->user_id,
			       qos_ptr->max_submit_jobs_pu);
			rc = false;
			goto end_it;
		}
	}

	if ((acct_policy_limit_set->time == ADMIN_SET_LIMIT)
	    || (qos_out_ptr->max_wall_pj != INFINITE)
	    || (qos_ptr->max_wall_pj == INFINITE)
	    || (update_call && (job_desc->time_limit == NO_VAL))) {
		/* no need to check/set */
	} else {

		qos_out_ptr->max_wall_pj = qos_ptr->max_wall_pj;

		if (qos_time_limit > qos_ptr->max_wall_pj)
			qos_time_limit = qos_ptr->max_wall_pj;
	}

	if (qos_time_limit != INFINITE) {
		if (job_desc->time_limit == NO_VAL) {
			if (part_ptr->max_time == INFINITE)
				job_desc->time_limit = qos_time_limit;
			else {
				job_desc->time_limit =
					MIN(qos_time_limit,
					    part_ptr->max_time);
			}
			acct_policy_limit_set->time = 1;
		} else if (acct_policy_limit_set->time &&
			   job_desc->time_limit > qos_time_limit) {
			job_desc->time_limit = qos_time_limit;
		} else if (strict_checking
			   && job_desc->time_limit > qos_time_limit) {
			if (reason)
				*reason = WAIT_QOS_MAX_WALL_PER_JOB;
			debug2("job submit for user %s(%u): "
			       "time limit %u exceeds qos max %u",
			       user_name,
			       job_desc->user_id,
			       job_desc->time_limit, qos_time_limit);
			rc = false;
			goto end_it;
		}
	}

	if (strict_checking && (qos_out_ptr->min_cpus_pj == INFINITE)
	    && (qos_ptr->min_cpus_pj != INFINITE)) {

		qos_out_ptr->min_cpus_pj = qos_ptr->min_cpus_pj;

		if (job_desc->tres_req_cnt[TRES_ARRAY_CPU] <
		    qos_ptr->min_cpus_pj) {
			if (reason)
				*reason = WAIT_QOS_MIN_CPUS;
			debug2("job submit for user %s(%u): "
			       "min cpus %"PRIu64" below "
			       "qos min %u",
			       user_name,
			       job_desc->user_id,
			       job_desc->tres_req_cnt[TRES_ARRAY_CPU],
			       qos_ptr->min_cpus_pj);
			rc = false;
			goto end_it;
		}
	}

end_it:
	return rc;
}

static int _qos_job_runnable_pre_select(struct job_record *job_ptr,
					 slurmdb_qos_rec_t *qos_ptr,
					 slurmdb_qos_rec_t *qos_out_ptr)
{
	uint32_t wall_mins;
	uint32_t time_limit;
	int rc = true;
	slurmdb_used_limits_t *used_limits = NULL;
	bool free_used_limits = false;

	if (!qos_ptr || !qos_out_ptr)
		return rc;

	wall_mins = qos_ptr->usage->grp_used_wall / 60;

	/*
	 * Try to get the used limits for the user or initialise a local
	 * nullified one if not available.
	 */
	used_limits = _get_used_limits_for_user(
		qos_ptr->usage->user_limit_list,
		job_ptr->user_id);

	if (!used_limits) {
		used_limits = xmalloc(sizeof(slurmdb_used_limits_t));
		used_limits->uid = job_ptr->user_id;
		free_used_limits = true;
	}

	/* we don't need to check grp_cpu_mins here */

	/* we don't need to check grp_cpus here */

	/* we don't need to check grp_mem here */
	if ((qos_out_ptr->grp_jobs == INFINITE) &&
	    (qos_ptr->grp_jobs != INFINITE)) {

		qos_out_ptr->grp_jobs = qos_ptr->grp_jobs;

		if (qos_ptr->usage->grp_used_jobs >= qos_ptr->grp_jobs) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_JOB;
			debug("job %u being held, "
			       "the job is at or exceeds "
			       "group max jobs limit %u with %u for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_jobs,
			       qos_ptr->usage->grp_used_jobs, qos_ptr->name);

			rc = false;
			goto end_it;
		}
	}

	/* we don't need to check grp_cpu_run_mins here */

	/* we don't need to check grp_nodes here */

	/* we don't need to check submit_jobs here */

	if ((qos_out_ptr->grp_wall == INFINITE)
	    && (qos_ptr->grp_wall != INFINITE)) {

		qos_out_ptr->grp_wall = qos_ptr->grp_wall;

		if (wall_mins >= qos_ptr->grp_wall) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_WALL;
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "group wall limit %u "
			       "with %u for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_wall,
			       wall_mins, qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* we don't need to check max_cpu_mins_pj here */

	/* we don't need to check max_cpus_pj here */

	/* we don't need to check min_cpus_pj here */

	/* we don't need to check max_cpus_pu here */

	if ((qos_out_ptr->max_jobs_pu == INFINITE)
	    && (qos_ptr->max_jobs_pu != INFINITE)) {

		qos_out_ptr->max_jobs_pu = qos_ptr->max_jobs_pu;

		if (used_limits->jobs >= qos_ptr->max_jobs_pu) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_JOB_PER_USER;
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "max jobs per-user limit "
			       "%u with %u for QOS %s",
			       job_ptr->job_id,
			       qos_ptr->max_jobs_pu,
			       used_limits->jobs, qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* we don't need to check max_nodes_pj here */

	/* we don't need to check max_nodes_pu here */

	/* we don't need to check submit_jobs_pu here */

	/* if the qos limits have changed since job
	 * submission and job can not run, then kill it */
	if ((job_ptr->limit_set.time != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->max_wall_pj == INFINITE)
	    && (qos_ptr->max_wall_pj != INFINITE)) {

		qos_out_ptr->max_wall_pj = qos_ptr->max_wall_pj;

		time_limit = qos_ptr->max_wall_pj;
		if ((job_ptr->time_limit != NO_VAL) &&
		    (job_ptr->time_limit > time_limit)) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_WALL_PER_JOB;
			debug2("job %u being held, "
			       "time limit %u exceeds qos "
			       "max wall pj %u",
			       job_ptr->job_id,
			       job_ptr->time_limit,
			       time_limit);
			rc = false;
			goto end_it;
		}
	}
end_it:

	if (free_used_limits)
		xfree(used_limits);

	return rc;
}

static int _qos_job_runnable_post_select(struct job_record *job_ptr,
					 slurmdb_qos_rec_t *qos_ptr,
					 slurmdb_qos_rec_t *qos_out_ptr,
					 uint32_t node_cnt, uint32_t cpu_cnt,
					 uint32_t job_memory,
					 uint64_t job_cpu_time_limit,
					 bool admin_set_memory_limit)
{
	uint64_t usage_mins;
	uint64_t cpu_time_limit;
	uint64_t cpu_run_mins;
	slurmdb_used_limits_t *used_limits = NULL;
	bool free_used_limits = false;
	bool safe_limits = false;
	int rc = true;

	if (!qos_ptr || !qos_out_ptr)
		return rc;

	/* check to see if we should be using safe limits, if so we
	 * will only start a job if there are sufficient remaining
	 * cpu-minutes for it to run to completion */
	if (accounting_enforce & ACCOUNTING_ENFORCE_SAFE)
		safe_limits = true;

	usage_mins = (uint64_t)(qos_ptr->usage->usage_raw / 60.0);
	cpu_run_mins = qos_ptr->usage->grp_used_cpu_run_secs / 60;

	/*
	 * Try to get the used limits for the user or initialise a local
	 * nullified one if not available.
	 */
	used_limits = _get_used_limits_for_user(
		qos_ptr->usage->user_limit_list,
		job_ptr->user_id);
	if (!used_limits) {
		used_limits = xmalloc(sizeof(slurmdb_used_limits_t));
		used_limits->uid = job_ptr->user_id;
		free_used_limits = true;
	}

	/* If the QOS has a GrpCPUMins limit set we may hold the job */
	if ((qos_out_ptr->grp_cpu_mins == (uint64_t)INFINITE)
	    && (qos_ptr->grp_cpu_mins != (uint64_t)INFINITE)) {

		qos_out_ptr->grp_cpu_mins = qos_ptr->grp_cpu_mins;

		if (usage_mins >= qos_ptr->grp_cpu_mins) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_CPU_MIN;
			debug2("Job %u being held, "
			       "the job is at or exceeds QOS %s's "
			       "group max cpu minutes of %"PRIu64" "
			       "with %"PRIu64"",
			       job_ptr->job_id,
			       qos_ptr->name,
			       qos_ptr->grp_cpu_mins,
			       usage_mins);
			rc = false;
			goto end_it;
		} else if (safe_limits
			   && ((job_cpu_time_limit + cpu_run_mins) >
			       (qos_ptr->grp_cpu_mins - usage_mins))) {
			/*
			 * If we're using safe limits start
			 * the job only if there are
			 * sufficient cpu-mins left such that
			 * it will run to completion without
			 * being killed
			 */
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_CPU_MIN;
			debug2("Job %u being held, "
			       "the job is at or exceeds QOS %s's "
			       "group max cpu minutes of %"PRIu64" "
			       "of which %"PRIu64" are still available "
			       "but request is for %"PRIu64" "
			       "(%"PRIu64" already used) cpu "
			       "minutes (%u cpus)",
			       job_ptr->job_id,
			       qos_ptr->name,
			       qos_ptr->grp_cpu_mins,
			       qos_ptr->grp_cpu_mins - usage_mins,
			       job_cpu_time_limit + cpu_run_mins,
			       cpu_run_mins,
			       cpu_cnt);

			rc = false;
			goto end_it;
		}
	}

	/* If the JOB's cpu limit wasn't administratively set and the
	 * QOS has a GrpCPU limit, cancel the job if its minimum
	 * cpu requirement has exceeded the limit for all CPUs
	 * usable by the QOS
	 */
	if ((job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->grp_cpus == INFINITE)
	    && (qos_ptr->grp_cpus != INFINITE)) {

		qos_out_ptr->grp_cpus = qos_ptr->grp_cpus;

		if (cpu_cnt > qos_ptr->grp_cpus) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_CPU;
			debug2("job %u is being held, "
			       "min cpu request %u exceeds "
			       "group max cpu limit %u for "
			       "qos '%s'",
			       job_ptr->job_id,
			       cpu_cnt,
			       qos_ptr->grp_cpus,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}

		if ((qos_ptr->usage->grp_used_cpus +
		     cpu_cnt) > qos_ptr->grp_cpus) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_GRP_CPU;
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "group max cpu limit %u "
			       "with already used %u + requested %u "
			       "for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_cpus,
			       qos_ptr->usage->grp_used_cpus,
			       cpu_cnt,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	if (!admin_set_memory_limit
	    && (qos_out_ptr->grp_mem == INFINITE)
	    && (qos_ptr->grp_mem != INFINITE)) {

		qos_out_ptr->grp_mem = qos_ptr->grp_mem;

		if (job_memory > qos_ptr->grp_mem) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_MEMORY;
			info("job %u is being held, "
			     "memory request %u exceeds "
			     "group max memory limit %u for "
			     "qos '%s'",
			     job_ptr->job_id,
			     job_memory,
			     qos_ptr->grp_mem,
			     qos_ptr->name);
			rc = false;
			goto end_it;
		}

		if ((qos_ptr->usage->grp_used_mem +
		     job_memory) > qos_ptr->grp_mem) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_GRP_MEMORY;
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "group memory limit %u "
			       "with already used %u + requested %u "
			       "for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_mem,
			       qos_ptr->usage->grp_used_mem,
			       job_memory,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* we don't need to check grp_jobs here */

	if ((qos_out_ptr->grp_cpu_run_mins == INFINITE)
	    && (qos_ptr->grp_cpu_run_mins != INFINITE)) {

		qos_out_ptr->grp_cpu_run_mins = qos_ptr->grp_cpu_run_mins;

		if (cpu_run_mins + job_cpu_time_limit >
		    qos_ptr->grp_cpu_run_mins) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_GRP_CPU_RUN_MIN;
			debug2("job %u being held, "
			       "qos %s is at or exceeds "
			       "group max running cpu minutes "
			       "limit %"PRIu64" with already "
			       "used %"PRIu64" + requested %"PRIu64" "
			       "for qos '%s'",
			       job_ptr->job_id, qos_ptr->name,
			       qos_ptr->grp_cpu_run_mins,
			       cpu_run_mins,
			       job_cpu_time_limit,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	if ((job_ptr->limit_set.min_nodes != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->grp_nodes == INFINITE)
	    && (qos_ptr->grp_nodes != INFINITE)) {

		qos_out_ptr->grp_nodes = qos_ptr->grp_nodes;

		if (node_cnt > qos_ptr->grp_nodes) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_GRP_NODES;
			debug2("job %u is being held, "
			       "min node request %u exceeds "
			       "group max node limit %u for "
			       "qos '%s'",
			       job_ptr->job_id,
			       node_cnt,
			       qos_ptr->grp_nodes,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}

		if ((qos_ptr->usage->grp_used_nodes +
		     node_cnt) >
		    qos_ptr->grp_nodes) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_GRP_NODES;
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "group max node limit %u "
			       "with already used %u + requested %u "
			       "for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_nodes,
			       qos_ptr->usage->grp_used_nodes,
			       node_cnt,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* we don't need to check submit_jobs here */

	/* we don't need to check grp_wall here */

	if ((qos_out_ptr->max_cpu_mins_pj == INFINITE)
	    && (qos_ptr->max_cpu_mins_pj != INFINITE)) {

		qos_out_ptr->max_cpu_mins_pj = qos_ptr->max_cpu_mins_pj;

		cpu_time_limit = qos_ptr->max_cpu_mins_pj;
		if ((job_ptr->time_limit != NO_VAL) &&
		    (job_cpu_time_limit > cpu_time_limit)) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_CPU_MINS_PER_JOB;
			debug2("job %u being held, "
			       "cpu time limit %"PRIu64" exceeds "
			       "qos %s max per-job %"PRIu64"",
			       job_ptr->job_id,
			       job_cpu_time_limit, qos_ptr->name,
			       cpu_time_limit);
			rc = false;
			goto end_it;
		}
	}

	if ((job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->max_cpus_pj == INFINITE)
	    && (qos_ptr->max_cpus_pj != INFINITE)) {

		qos_out_ptr->max_cpus_pj = qos_ptr->max_cpus_pj;

		if (cpu_cnt > qos_ptr->max_cpus_pj) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_CPUS_PER_JOB;
			debug2("job %u being held, "
			       "min cpu limit %u exceeds "
			       "qos %s per-job max %u",
			       job_ptr->job_id,
			       cpu_cnt, qos_ptr->name,
			       qos_ptr->max_cpus_pj);
			rc = false;
			goto end_it;
		}
	}

	if ((job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->min_cpus_pj == INFINITE)
	    && (qos_ptr->min_cpus_pj != INFINITE)) {

		qos_out_ptr->min_cpus_pj = qos_ptr->min_cpus_pj;

		if (cpu_cnt && cpu_cnt < qos_ptr->min_cpus_pj) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_MIN_CPUS;
			debug2("%s job %u being held, "
			       "min cpu limit %u below "
			       "qos %s per-job min %u",
			       __func__, job_ptr->job_id,
			       cpu_cnt, qos_ptr->name,
			       qos_ptr->min_cpus_pj);
			rc = false;
			goto end_it;
		}
	}

	if ((job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->max_cpus_pu == INFINITE)
	    && (qos_ptr->max_cpus_pu != INFINITE)) {

		qos_out_ptr->max_cpus_pu = qos_ptr->max_cpus_pu;

		/* Hold the job if it exceeds the per-user
		 * CPU limit for the given QOS
		 */
		if (cpu_cnt > qos_ptr->max_cpus_pu) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_CPU_PER_USER;
			debug2("job %u being held, "
			       "min cpu limit %u exceeds "
			       "qos %s per-user max %u",
			       job_ptr->job_id,
			       cpu_cnt, qos_ptr->name,
			       qos_ptr->max_cpus_pu);
			rc = false;
			goto end_it;
		}
		/* Hold the job if the user has exceeded
		 * the QOS per-user CPU limit with their
		 * current usage */
		if ((used_limits->cpus + cpu_cnt)
		    > qos_ptr->max_cpus_pu) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =
				WAIT_QOS_MAX_CPU_PER_USER;
			debug2("job %u being held, "
			       "the user is at or would exceed "
			       "max cpus per-user limit "
			       "%u with %u(+%u) for QOS %s",
			       job_ptr->job_id,
			       qos_ptr->max_cpus_pu,
			       used_limits->cpus,
			       cpu_cnt,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}

	/* We do not need to check max_jobs_pu here */

	if ((job_ptr->limit_set.min_nodes != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->max_nodes_pj == INFINITE)
	    && (qos_ptr->max_nodes_pj != INFINITE)) {

		qos_out_ptr->max_nodes_pj = qos_ptr->max_nodes_pj;

		if (node_cnt > qos_ptr->max_nodes_pj) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_MAX_NODE_PER_JOB;
			debug2("job %u being held, "
			       "min node limit %u exceeds "
			       "qos %s max %u",
			       job_ptr->job_id,
			       node_cnt, qos_ptr->name,
			       qos_ptr->max_nodes_pj);
			rc = false;
			goto end_it;
		}
	}

	if ((job_ptr->limit_set.min_nodes != ADMIN_SET_LIMIT)
	    && (qos_out_ptr->max_nodes_pu == INFINITE)
	    && (qos_ptr->max_nodes_pu != INFINITE)) {

		qos_out_ptr->max_nodes_pu = qos_ptr->max_nodes_pu;

		/* Cancel the job if it exceeds the per-user
		 * node limit for the given QOS
		 */
		if (node_cnt > qos_ptr->max_nodes_pu) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_QOS_MAX_NODE_PER_USER;
			debug2("job %u being held, "
			       "min node per-puser limit %u exceeds "
			       "qos %s max %u",
			       job_ptr->job_id,
			       node_cnt, qos_ptr->name,
			       qos_ptr->max_nodes_pu);
			rc = false;
			goto end_it;
		}

		/*
		 * Hold the job if the user has exceeded
		 * the QOS per-user CPU limit with their
		 * current usage
		 */
		if ((used_limits->nodes + node_cnt) > qos_ptr->max_nodes_pu) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason =	WAIT_QOS_MAX_NODE_PER_USER;
			debug2("job %u being held, "
			       "the user is at or would exceed "
			       "max nodes per-user "
			       "limit %u with %u(+%u) for QOS %s",
			       job_ptr->job_id,
			       qos_ptr->max_nodes_pu,
			       used_limits->nodes,
			       node_cnt,
			       qos_ptr->name);
			rc = false;
			goto end_it;
		}
	}
end_it:
	/* we don't need to check submit_jobs_pu here */

	/* we don't need to check max_wall_pj here */

	if (free_used_limits)
		xfree(used_limits);

	return rc;
}

static int _qos_job_time_out(struct job_record *job_ptr,
			     slurmdb_qos_rec_t *qos_ptr,
			     slurmdb_qos_rec_t *qos_out_ptr,
			     uint64_t job_cpu_usage_mins)
{
	uint64_t usage_mins;
	uint32_t wall_mins;
	int rc = true;
	time_t now = time(NULL);

	if (!qos_ptr || !qos_out_ptr)
		return rc;

	/* The idea here is for qos to trump what an association
	 * has set for a limit, so if an association set of
	 * wall 10 mins and the qos has 20 mins set and the
	 * job has been running for 11 minutes it continues
	 * until 20.
	 */
	usage_mins = (uint64_t)(qos_ptr->usage->usage_raw / 60.0);
	wall_mins = qos_ptr->usage->grp_used_wall / 60;

	if ((qos_out_ptr->grp_cpu_mins == (uint64_t)INFINITE)
	    && (qos_ptr->grp_cpu_mins != (uint64_t)INFINITE)) {

		qos_out_ptr->grp_cpu_mins = qos_ptr->grp_cpu_mins;

		if (usage_mins >= qos_ptr->grp_cpu_mins) {
			last_job_update = now;
			info("Job %u timed out, "
			     "the job is at or exceeds QOS %s's "
			     "group max cpu minutes of %"PRIu64" "
			     "with %"PRIu64"",
			     job_ptr->job_id,
			     qos_ptr->name,
			     qos_ptr->grp_cpu_mins,
			     usage_mins);
			job_ptr->state_reason = FAIL_TIMEOUT;
			rc = false;
			goto end_it;
		}
	}

	if ((qos_out_ptr->grp_wall == INFINITE)
	    && (qos_ptr->grp_wall != INFINITE)) {

		qos_out_ptr->grp_wall = qos_ptr->grp_wall;

		if (wall_mins >= qos_ptr->grp_wall) {
			last_job_update = now;
			info("Job %u timed out, "
			     "the job is at or exceeds QOS %s's "
			     "group wall limit of %u with %u",
			     job_ptr->job_id,
			     qos_ptr->name, qos_ptr->grp_wall,
			     wall_mins);
			job_ptr->state_reason = FAIL_TIMEOUT;
			rc = false;
			goto end_it;
		}
	}

	if ((qos_out_ptr->max_cpu_mins_pj == (uint64_t)INFINITE)
	    && (qos_ptr->max_cpu_mins_pj != (uint64_t)INFINITE)) {

		qos_out_ptr->max_cpu_mins_pj = qos_ptr->max_cpu_mins_pj;

		if (job_cpu_usage_mins >= qos_ptr->max_cpu_mins_pj) {
			last_job_update = now;
			info("Job %u timed out, "
			     "the job is at or exceeds QOS %s's "
			     "max cpu minutes of %"PRIu64" "
			     "with %"PRIu64"",
			     job_ptr->job_id,
			     qos_ptr->name,
			     qos_ptr->max_cpu_mins_pj,
			     job_cpu_usage_mins);
			job_ptr->state_reason = FAIL_TIMEOUT;
			rc = false;
			goto end_it;
		}
	}

end_it:
	return rc;
}

/*
 * _validate_tres_limits - validate the tres requested against limits
 * of an association as well as qos skipping any limit an admin set
 *
 * OUT - tres_pos - if false is returned position in array of failed limit
 * IN - job_tres_array - count of various tres in use
 * IN - assoc_tres_array - limits on the association
 * IN - qos_tres_array - limits on the qos
 * IN - acct_policy_limit_set_array - limits that have been overridden
 *                                    by an admin
 * IN strick_checking - If a limit needs to be enforced now or not.
 * IN update_call - If this is an update or a create call
 *
 * RET - True if no limit is violated, false otherwise with tres_pos
 * being set to the position of the failed limit.
 */
static bool _validate_tres_limits(int *tres_pos,
				  uint64_t *job_tres_array,
				  uint64_t *assoc_tres_array,
				  uint64_t *qos_tres_array,
				  uint16_t *admin_set_limit_tres_array,
				  bool strict_checking,
				  bool update_call)
{
	if (!strict_checking)
		return true;

	for ((*tres_pos) = 0; (*tres_pos) < g_tres_count;
	     (*tres_pos)++) {
		if ((admin_set_limit_tres_array[*tres_pos] !=
		     ADMIN_SET_LIMIT) &&
		    (qos_tres_array[*tres_pos] == (uint64_t)INFINITE) &&
		    (assoc_tres_array[*tres_pos] != (uint64_t)INFINITE) &&
		    (job_tres_array[*tres_pos] || !update_call) &&
		    (job_tres_array[*tres_pos] > assoc_tres_array[*tres_pos]))
			return false;
		/* should be the same as below */

		/* if ((admin_set_limit_tres_array[*tres_pos] == */
		/*      (uint64_t)ADMIN_SET_LIMIT) */
		/*     || (qos_tres_array[*tres_pos] != (uint64_t)INFINITE) */
		/*     || (assoc_tres_array[*tres_pos] == (uint64_t)INFINITE) */
		/*     || (update_call && */
		/* 	(job_tres_array[*tres_pos] == (uint64_t)NO_VAL))) { */
		/* 	/\* no need to check/set *\/ */
		/* } else if ((job_tres_array[*tres_pos] != (uint64_t)NO_VAL) */
		/* 	   && (job_tres_array[*tres_pos] > */
		/* 	       assoc_tres_array[*tres_pos])) { */
		/* 	return false; */
		/* } */
	}

	return true;
}

/*
 * acct_policy_add_job_submit - Note that a job has been submitted for
 *	accounting policy purposes.
 */
extern void acct_policy_add_job_submit(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_ADD_SUBMIT, job_ptr);
}

/*
 * acct_policy_remove_job_submit - Note that a job has finished (might
 *      not had started or been allocated resources) for accounting
 *      policy purposes.
 */
extern void acct_policy_remove_job_submit(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_REM_SUBMIT, job_ptr);
}

/*
 * acct_policy_job_begin - Note that a job is starting for accounting
 *	policy purposes.
 */
extern void acct_policy_job_begin(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_JOB_BEGIN, job_ptr);
}

/*
 * acct_policy_job_fini - Note that a job is completing for accounting
 *	policy purposes.
 */
extern void acct_policy_job_fini(struct job_record *job_ptr)
{
	/* if end_time_exp == NO_VAL this has already happened */
	if (job_ptr->end_time_exp != (time_t)NO_VAL)
		_adjust_limit_usage(ACCT_POLICY_JOB_FINI, job_ptr);
	else
		debug2("We have already ran the job_fini for job %u",
		       job_ptr->job_id);
}

extern void acct_policy_alter_job(struct job_record *job_ptr,
				  uint32_t new_time_limit)
{
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_assoc_rec_t *assoc_ptr = NULL;
	assoc_mgr_lock_t locks = { WRITE_LOCK, NO_LOCK, WRITE_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };
	uint64_t used_cpu_run_secs, new_used_cpu_run_secs;

	if (!IS_JOB_RUNNING(job_ptr) || (job_ptr->time_limit == new_time_limit))
		return;

	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS)
	    || !_valid_job_assoc(job_ptr))
		return;

	used_cpu_run_secs = (uint64_t)job_ptr->total_cpus
		* (uint64_t)job_ptr->time_limit * 60;
	new_used_cpu_run_secs = (uint64_t)job_ptr->total_cpus
		* (uint64_t)new_time_limit * 60;

	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	_qos_alter_job(job_ptr, qos_ptr_1,
		       used_cpu_run_secs, new_used_cpu_run_secs);
	_qos_alter_job(job_ptr, qos_ptr_2,
		       used_cpu_run_secs, new_used_cpu_run_secs);

	assoc_ptr = (slurmdb_assoc_rec_t *)job_ptr->assoc_ptr;
	while (assoc_ptr) {
		assoc_ptr->usage->grp_used_cpu_run_secs -=
			used_cpu_run_secs;
		assoc_ptr->usage->grp_used_cpu_run_secs +=
			new_used_cpu_run_secs;
		debug2("altering %u acct %s got %"PRIu64" "
		       "just removed %"PRIu64" and added %"PRIu64"",
		       job_ptr->job_id,
		       assoc_ptr->acct,
		       assoc_ptr->usage->grp_used_cpu_run_secs,
		       used_cpu_run_secs,
		       new_used_cpu_run_secs);
		/* now handle all the group limits of the parents */
		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
	}
	assoc_mgr_unlock(&locks);
}

extern bool acct_policy_validate(job_desc_msg_t *job_desc,
				 struct part_record *part_ptr,
				 slurmdb_assoc_rec_t *assoc_in,
				 slurmdb_qos_rec_t *qos_ptr,
				 uint32_t *reason,
				 acct_policy_limit_set_t *acct_policy_limit_set,
				 bool update_call)
{
	uint32_t time_limit;
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_qos_rec_t qos_rec;
	slurmdb_assoc_rec_t *assoc_ptr = assoc_in;
	int parent = 0, job_cnt = 1;
	char *user_name = NULL;
	bool rc = true;
	struct job_record job_rec;
	uint64_t qos_tres_ctld[g_tres_count];
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
				   READ_LOCK, NO_LOCK, NO_LOCK };
	bool strict_checking;

	xassert(acct_policy_limit_set);

	if (!assoc_ptr) {
		error("acct_policy_validate: no assoc_ptr given for job.");
		return false;
	}
	user_name = assoc_ptr->user;

	if (job_desc->array_bitmap)
		job_cnt = bit_set_count(job_desc->array_bitmap);

	slurmdb_init_qos_rec(&qos_rec, 0, INFINITE);

	assoc_mgr_lock(&locks);

	job_rec.qos_ptr = qos_ptr;
	job_rec.part_ptr = part_ptr;

	_set_qos_order(&job_rec, &qos_ptr_1, &qos_ptr_2);

	if (qos_ptr_1) {
		strict_checking =
			(reason || (qos_ptr_1->flags & QOS_FLAG_DENY_LIMIT));
		if (qos_ptr_2 && !strict_checking)
			strict_checking =
				qos_ptr_2->flags & QOS_FLAG_DENY_LIMIT;

		if (!(rc = _qos_policy_validate(
			      job_desc, part_ptr, qos_ptr_1, &qos_rec,
			      reason, acct_policy_limit_set, update_call,
			      user_name, job_desc->tres_req_cnt[TRES_ARRAY_MEM],
			      job_cnt, strict_checking)))
			goto end_it;
		if (!(rc = _qos_policy_validate(
			      job_desc, part_ptr, qos_ptr_2, &qos_rec,
			      reason, acct_policy_limit_set, update_call,
			      user_name, job_desc->tres_req_cnt[TRES_ARRAY_MEM],
			      job_cnt, strict_checking)))
			goto end_it;

	} else
		strict_checking = reason ? true : false;

	/* FIXME: This needs to work with qos limits, and we
	 * are fudging them now.
	 */
	memset(qos_tres_ctld, 0, sizeof(qos_tres_ctld));

	while (assoc_ptr) {
		int tres_pos = 0;

		/* for validation we don't need to look at
		 * assoc_ptr->grp_cpu_mins.
		 */

		qos_tres_ctld[TRES_ARRAY_CPU] = qos_rec.grp_cpus;
		qos_tres_ctld[TRES_ARRAY_MEM] = qos_rec.grp_mem;

		if (!_validate_tres_limits(&tres_pos, job_desc->tres_req_cnt,
					   assoc_ptr->grp_tres_ctld,
					   qos_tres_ctld,
					   acct_policy_limit_set->max_tres,
					   strict_checking, update_call)) {
			/* FIXME: This is most likely not the reason
			   we want to send back.
			*/
			if (reason)
				*reason = WAIT_ASSOC_GRP_CPU;
			debug2("job submit for user %s(%u): "
			       "min tres(%s%s%s) request %"PRIu64" exceeds "
			       "group max tres limit %"PRIu64" for account %s",
			       user_name,
			       job_desc->user_id,
			       assoc_mgr_tres_array[tres_pos]->type,
			       assoc_mgr_tres_array[tres_pos]->name ? "/" : "",
			       assoc_mgr_tres_array[tres_pos]->name ?
			       assoc_mgr_tres_array[tres_pos]->name : "",
			       job_desc->tres_req_cnt[tres_pos],
			       assoc_ptr->grp_tres_ctld[tres_pos],
			       assoc_ptr->acct);
			rc = false;
			break;
		}

		/* for validation we don't need to look at
		 * assoc_ptr->grp_jobs.
		 */

		if ((acct_policy_limit_set->max_nodes == ADMIN_SET_LIMIT)
		    || (qos_rec.grp_nodes != INFINITE)
		    || (assoc_ptr->grp_nodes == INFINITE)
		    || (update_call && (job_desc->max_nodes == NO_VAL))) {
			/* no need to check/set */
		} else if (strict_checking && (job_desc->min_nodes != NO_VAL)
			   && (job_desc->min_nodes > assoc_ptr->grp_nodes)) {
			if (reason)
				*reason = WAIT_ASSOC_GRP_NODES;
			debug2("job submit for user %s(%u): "
			       "min node request %u exceeds "
			       "group max node limit %u for account %s",
			       user_name,
			       job_desc->user_id,
			       job_desc->min_nodes,
			       assoc_ptr->grp_nodes,
			       assoc_ptr->acct);
			rc = false;
			break;
		}

		if ((qos_rec.grp_submit_jobs == INFINITE) &&
		    (assoc_ptr->grp_submit_jobs != INFINITE) &&
		    ((assoc_ptr->usage->used_submit_jobs + job_cnt)
		     > assoc_ptr->grp_submit_jobs)) {
			if (reason)
				*reason = WAIT_ASSOC_GRP_SUB_JOB;
			debug2("job submit for user %s(%u): "
			       "group max submit job limit exceeded %u "
			       "for account '%s'",
			       user_name,
			       job_desc->user_id,
			       assoc_ptr->grp_submit_jobs,
			       assoc_ptr->acct);
			rc = false;
			break;
		}


		/* for validation we don't need to look at
		 * assoc_ptr->grp_wall. It is checked while the job is running.
		 */

		/* We don't need to look at the regular limits for
		 * parents since we have pre-propogated them, so just
		 * continue with the next parent
		 */
		if (parent) {
			assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
			continue;
		}

		/* for validation we don't need to look at
		 * assoc_ptr->max_cpu_mins_pj.
		 */

		qos_tres_ctld[TRES_ARRAY_CPU] = qos_rec.max_cpus_pj;
		qos_tres_ctld[TRES_ARRAY_MEM] = (uint64_t)INFINITE;
		tres_pos = 0;
		if (!_validate_tres_limits(&tres_pos, job_desc->tres_req_cnt,
					   assoc_ptr->max_tres_ctld,
					   qos_tres_ctld,
					   acct_policy_limit_set->max_tres,
					   strict_checking, update_call)) {
			/* FIXME: This is most likely not the reason
			   we want to send back.
			*/
			if (reason)
				*reason = WAIT_ASSOC_MAX_CPUS_PER_JOB;
			debug2("job submit for user %s(%u): "
			       "min tres(%s%s%s) request %"PRIu64" exceeds "
			       "max tres limit %"PRIu64" for account %s",
			       user_name,
			       job_desc->user_id,
			       assoc_mgr_tres_array[tres_pos]->type,
			       assoc_mgr_tres_array[tres_pos]->name ? "/" : "",
			       assoc_mgr_tres_array[tres_pos]->name ?
			       assoc_mgr_tres_array[tres_pos]->name : "",
			       job_desc->tres_req_cnt[tres_pos],
			       assoc_ptr->grp_tres_ctld[tres_pos],
			       assoc_ptr->acct);
			rc = false;
			break;
		}

		/* for validation we don't need to look at
		 * assoc_ptr->max_jobs.
		 */

		if ((acct_policy_limit_set->max_nodes == ADMIN_SET_LIMIT)
		    || (qos_rec.max_nodes_pj != INFINITE)
		    || (assoc_ptr->max_nodes_pj == INFINITE)
		    || (update_call && (job_desc->max_nodes == NO_VAL))) {
			/* no need to check/set */
		} else if (strict_checking && (job_desc->min_nodes != NO_VAL)
			   && (job_desc->min_nodes > assoc_ptr->max_nodes_pj)) {
			if (reason)
				*reason = WAIT_ASSOC_MAX_NODE_PER_JOB;
			debug2("job submit for user %s(%u): "
			       "min node limit %u exceeds "
			       "account max %u",
			       user_name,
			       job_desc->user_id,
			       job_desc->min_nodes,
			       assoc_ptr->max_nodes_pj);
			rc = false;
			break;
		}

		if ((qos_rec.max_submit_jobs_pu == INFINITE) &&
		    (assoc_ptr->max_submit_jobs != INFINITE) &&
		    ((assoc_ptr->usage->used_submit_jobs + job_cnt)
		     > assoc_ptr->max_submit_jobs)) {
			if (reason)
				*reason = WAIT_ASSOC_MAX_SUB_JOB;
			debug2("job submit for user %s(%u): "
			       "account max submit job limit exceeded %u",
			       user_name,
			       job_desc->user_id,
			       assoc_ptr->max_submit_jobs);
			rc = false;
			break;
		}

		if ((acct_policy_limit_set->time == ADMIN_SET_LIMIT)
		    || (qos_rec.max_wall_pj != INFINITE)
		    || (assoc_ptr->max_wall_pj == INFINITE)
		    || (update_call && (job_desc->time_limit == NO_VAL))) {
			/* no need to check/set */
		} else {
			time_limit = assoc_ptr->max_wall_pj;
			if (job_desc->time_limit == NO_VAL) {
				if (part_ptr->max_time == INFINITE)
					job_desc->time_limit = time_limit;
				else
					job_desc->time_limit =
						MIN(time_limit,
						    part_ptr->max_time);
				acct_policy_limit_set->time = 1;
			} else if (acct_policy_limit_set->time &&
				   job_desc->time_limit > time_limit) {
				job_desc->time_limit = time_limit;
			} else if (strict_checking &&
				   (job_desc->time_limit > time_limit)) {
				if (reason)
					*reason = WAIT_ASSOC_MAX_WALL_PER_JOB;
				debug2("job submit for user %s(%u): "
				       "time limit %u exceeds account max %u",
				       user_name,
				       job_desc->user_id,
				       job_desc->time_limit, time_limit);
				rc = false;
				break;
			}
		}

		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
	}
end_it:
	assoc_mgr_unlock(&locks);

	return rc;
}

/*
 * Determine of the specified job can execute right now or is currently
 * blocked by an association or QOS limit. Does not re-validate job state.
 */
extern bool acct_policy_job_runnable_state(struct job_record *job_ptr)
{
	/* If any more limits are added this will need to be added to */
	if ((job_ptr->state_reason >= WAIT_QOS_GRP_CPU
	     && job_ptr->state_reason <= WAIT_ASSOC_MAX_SUB_JOB) ||
	    (job_ptr->state_reason == WAIT_ASSOC_JOB_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_RESOURCE_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_TIME_LIMIT) ||
	    (job_ptr->state_reason == WAIT_QOS_JOB_LIMIT) ||
	    (job_ptr->state_reason == WAIT_QOS_TIME_LIMIT)) {
		return false;
	}

	return true;
}

/*
 * acct_policy_job_runnable_pre_select - Determine of the specified
 *	job can execute right now or not depending upon accounting
 *	policy (e.g. running job limit for this association). If the
 *	association limits prevent the job from ever running (lowered
 *	limits since job submission), then cancel the job.
 */
extern bool acct_policy_job_runnable_pre_select(struct job_record *job_ptr)
{
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_qos_rec_t qos_rec;
	slurmdb_assoc_rec_t *assoc_ptr;
	uint32_t time_limit;
	bool rc = true;
	uint32_t wall_mins;
	int parent = 0; /* flag to tell us if we are looking at the
			 * parent or not
			 */
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };

	/* check to see if we are enforcing associations */
	if (!accounting_enforce)
		return true;

	if (!_valid_job_assoc(job_ptr)) {
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = FAIL_ACCOUNT;
		return false;
	}

	/* now see if we are enforcing limits */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return true;

	/* clear old state reason */
	if (!acct_policy_job_runnable_state(job_ptr)) {
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = WAIT_NO_REASON;
	}

	slurmdb_init_qos_rec(&qos_rec, 0, INFINITE);

	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	/* check the first QOS setting it's values in the qos_rec */
	if (qos_ptr_1 &&
	    !(rc = _qos_job_runnable_pre_select(job_ptr, qos_ptr_1,
						 &qos_rec)))
		goto end_it;

	/* If qos_ptr_1 didn't set the value use the 2nd QOS to set
	   the limit.
	*/
	if (qos_ptr_2 &&
	    !(rc = _qos_job_runnable_pre_select(job_ptr, qos_ptr_2,
						 &qos_rec)))
		goto end_it;

	assoc_ptr = job_ptr->assoc_ptr;
	while (assoc_ptr) {
		wall_mins = assoc_ptr->usage->grp_used_wall / 60;

#if _DEBUG
		info("acct_job_limits: %u of %u",
		     assoc_ptr->usage->used_jobs, assoc_ptr->max_jobs);
#endif
		/* we don't need to check grp_cpu_mins here */

		/* we don't need to check grp_cpus here */

		/* we don't need to check grp_mem here */

		if ((qos_rec.grp_jobs == INFINITE) &&
		    (assoc_ptr->grp_jobs != INFINITE) &&
		    (assoc_ptr->usage->used_jobs >= assoc_ptr->grp_jobs)) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_ASSOC_GRP_JOB;
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "group max jobs limit %u with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->grp_jobs,
			       assoc_ptr->usage->used_jobs, assoc_ptr->acct);

			rc = false;
			goto end_it;
		}

		/* we don't need to check grp_cpu_run_mins here */

		/* we don't need to check grp_nodes here */

		/* we don't need to check submit_jobs here */

		if ((qos_rec.grp_wall == INFINITE)
		    && (assoc_ptr->grp_wall != INFINITE)
		    && (wall_mins >= assoc_ptr->grp_wall)) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_ASSOC_GRP_WALL;
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "group wall limit %u "
			       "with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->grp_wall,
			       wall_mins, assoc_ptr->acct);
			rc = false;
			goto end_it;
		}


		/* We don't need to look at the regular limits for
		 * parents since we have pre-propogated them, so just
		 * continue with the next parent
		 */
		if (parent) {
			assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
			continue;
		}

		/* we don't need to check max_cpu_mins_pj here */

		/* we don't need to check max_cpus_pj here */

		if ((qos_rec.max_jobs_pu == INFINITE) &&
		    (assoc_ptr->max_jobs != INFINITE) &&
		    (assoc_ptr->usage->used_jobs >= assoc_ptr->max_jobs)) {
			xfree(job_ptr->state_desc);
			job_ptr->state_reason = WAIT_ASSOC_MAX_JOBS;
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "max jobs limit %u with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->max_jobs,
			       assoc_ptr->usage->used_jobs, assoc_ptr->acct);
			rc = false;
			goto end_it;
		}

		/* we don't need to check max_nodes_pj here */

		/* we don't need to check submit_jobs here */

		/* if the association limits have changed since job
		 * submission and job can not run, then kill it */
		if ((job_ptr->limit_set.time != ADMIN_SET_LIMIT)
		    && (qos_rec.max_wall_pj == INFINITE)
		    && (assoc_ptr->max_wall_pj != INFINITE)) {
			time_limit = assoc_ptr->max_wall_pj;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_ptr->time_limit > time_limit)) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason =
					WAIT_ASSOC_MAX_WALL_PER_JOB;
				debug2("job %u being held, "
				       "time limit %u exceeds account max %u",
				       job_ptr->job_id,
				       job_ptr->time_limit,
				       time_limit);
				rc = false;
				goto end_it;
			}
		}

		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
	}
end_it:
	assoc_mgr_unlock(&locks);

	return rc;
}

/*
 * acct_policy_job_runnable_post_select - After nodes have been
 *	selected for the job verify the counts don't exceed aggregated limits.
 */
extern bool acct_policy_job_runnable_post_select(
	struct job_record *job_ptr, uint32_t node_cnt,
	uint32_t cpu_cnt, uint32_t pn_min_memory)
{
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_qos_rec_t qos_rec;
	slurmdb_assoc_rec_t *assoc_ptr;
	uint64_t cpu_time_limit;
	uint64_t job_cpu_time_limit;
	uint64_t cpu_run_mins;
	bool rc = true;
	uint64_t usage_mins;
	uint32_t job_memory = 0;
	bool admin_set_memory_limit = false;
	bool safe_limits = false;
	int parent = 0; /* flag to tell us if we are looking at the
			 * parent or not
			 */
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };

	/* check to see if we are enforcing associations */
	if (!accounting_enforce)
		return true;

	/* probably don't need to check this here */
	/* if (!_valid_job_assoc(job_ptr)) { */
	/* 	job_ptr->state_reason = FAIL_ACCOUNT; */
	/* 	return false; */
	/* } */

	/* now see if we are enforcing limits */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return true;

	/* check to see if we should be using safe limits, if so we
	 * will only start a job if there are sufficient remaining
	 * cpu-minutes for it to run to completion */
	if (accounting_enforce & ACCOUNTING_ENFORCE_SAFE)
		safe_limits = true;

	/* clear old state reason */
	if (!acct_policy_job_runnable_state(job_ptr)) {
		xfree(job_ptr->state_desc);
		job_ptr->state_reason = WAIT_NO_REASON;
	}

	job_cpu_time_limit = (uint64_t)job_ptr->time_limit * (uint64_t)cpu_cnt;

	if (pn_min_memory) {
		char *memory_type = NULL;

		admin_set_memory_limit =
			(job_ptr->limit_set.max_tres[TRES_ARRAY_MEM] ==
			 ADMIN_SET_LIMIT)
			|| (job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] ==
			    ADMIN_SET_LIMIT);

		if (pn_min_memory & MEM_PER_CPU) {
			memory_type = "MPC";
			job_memory = (pn_min_memory & (~MEM_PER_CPU)) * cpu_cnt;
		} else {
			memory_type = "MPN";
			job_memory = (pn_min_memory) * node_cnt;
		}
		debug3("acct_policy_job_runnable_post_select: job %u: %s: "
		       "job_memory set to %u",
		       job_ptr->job_id, memory_type, job_memory);
	}

	slurmdb_init_qos_rec(&qos_rec, 0, INFINITE);

	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	/* check the first QOS setting it's values in the qos_rec */
	if (qos_ptr_1 &&
	    !(rc = _qos_job_runnable_post_select(job_ptr, qos_ptr_1,
						 &qos_rec, node_cnt, cpu_cnt,
						 job_memory, job_cpu_time_limit,
						 admin_set_memory_limit)))
		goto end_it;

	/* If qos_ptr_1 didn't set the value use the 2nd QOS to set
	   the limit.
	*/
	if (qos_ptr_2 &&
	    !(rc = _qos_job_runnable_post_select(job_ptr, qos_ptr_2,
						 &qos_rec, node_cnt, cpu_cnt,
						 job_memory, job_cpu_time_limit,
						 admin_set_memory_limit)))
		goto end_it;

	assoc_ptr = job_ptr->assoc_ptr;
	while (assoc_ptr) {
		usage_mins = (uint64_t)(assoc_ptr->usage->usage_raw / 60.0);
		cpu_run_mins = assoc_ptr->usage->grp_used_cpu_run_secs / 60;

#if _DEBUG
		info("acct_job_limits: %u of %u",
		     assoc_ptr->usage->used_jobs, assoc_ptr->max_jobs);
#endif
		/*
		 * If the association has a GrpCPUMins limit set (and there
		 * is no QOS with GrpCPUMins set) we may hold the job
		 */

		/* FIXME: this only works with CPUS and was only done
		 * this way to get the slurmctld to compile and work
		 * with the TRES strings.  This should probably be a
		 * new call to a function that does this for each TRES.
		 */
		uint64_t limit = slurmdb_find_tres_count_in_string(
			assoc_ptr->grp_tres_mins, TRES_CPU);
		if ((qos_rec.grp_cpu_mins == (uint64_t)INFINITE)
		    && (limit != (uint64_t)INFINITE)) {
			if (usage_mins >= limit) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_CPU_MIN;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max cpu minutes limit %"PRIu64" "
				       "with %Lf for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       limit,
				       assoc_ptr->usage->usage_raw,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			} else if (safe_limits
				   && ((job_cpu_time_limit + cpu_run_mins) >
				       (limit - usage_mins))) {
				/*
				 * If we're using safe limits start
				 * the job only if there are
				 * sufficient cpu-mins left such that
				 * it will run to completion without
				 * being killed
				 */
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_CPU_MIN;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max cpu minutes of %"PRIu64" "
				       "of which %"PRIu64" are still available "
				       "but request is for %"PRIu64" cpu "
				       "minutes (%u cpus)"
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       limit, limit - usage_mins,
				       job_cpu_time_limit + cpu_run_mins,
				       cpu_cnt,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		limit = slurmdb_find_tres_count_in_string(
			assoc_ptr->grp_tres, TRES_CPU);
		if ((job_ptr->limit_set.min_tres[TRES_ARRAY_CPU] !=
		     ADMIN_SET_LIMIT)
		    && (qos_rec.grp_cpus == INFINITE)
		    && (limit != (uint64_t)INFINITE)) {
			if (cpu_cnt > (uint32_t)limit) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_CPU;
				debug2("job %u being held, "
				       "min cpu request %u exceeds "
				       "group max cpu limit %"PRIu64" for "
				       "account %s",
				       job_ptr->job_id,
				       cpu_cnt,
				       limit,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}

			if ((assoc_ptr->usage->grp_used_cpus + cpu_cnt) >
			    limit) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_CPU;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max cpu limit %"PRIu64" "
				       "with already used %u + requested %u "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       limit,
				       assoc_ptr->usage->grp_used_cpus,
				       cpu_cnt,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		if (!admin_set_memory_limit
		    && (qos_rec.grp_mem == INFINITE)
		    && (assoc_ptr->grp_mem != INFINITE)) {
			if (job_memory > assoc_ptr->grp_mem) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_MEMORY;
				info("job %u being held, "
				     "memory request %u exceeds "
				     "group memory limit %u for "
				     "account %s",
				     job_ptr->job_id,
				     job_memory,
				     assoc_ptr->grp_mem,
				     assoc_ptr->acct);
				rc = false;
				goto end_it;
			}

			if ((assoc_ptr->usage->grp_used_mem + job_memory) >
			    assoc_ptr->grp_mem) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_MEMORY;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group memory limit %u "
				       "with already used %u + requested %u "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       assoc_ptr->grp_mem,
				       assoc_ptr->usage->grp_used_mem,
				       job_memory,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check grp_jobs here */

		limit = slurmdb_find_tres_count_in_string(
			assoc_ptr->grp_tres_run_mins, TRES_CPU);
		if ((qos_rec.grp_cpu_run_mins == INFINITE)
		    && (limit != (uint64_t)INFINITE)) {
			if (cpu_run_mins + job_cpu_time_limit >
			    (uint32_t)limit) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason =
					WAIT_ASSOC_GRP_CPU_RUN_MIN;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max running cpu minutes "
				       "limit %"PRIu64" with already "
				       "used %"PRIu64" + requested %"PRIu64" "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       limit,
				       cpu_run_mins,
				       job_cpu_time_limit,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		if ((job_ptr->limit_set.min_nodes != ADMIN_SET_LIMIT)
		    && (qos_rec.grp_nodes == INFINITE)
		    && (assoc_ptr->grp_nodes != INFINITE)) {
			if (node_cnt >
			    assoc_ptr->grp_nodes) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_NODES;
				debug2("job %u being held, "
				       "min node request %u exceeds "
				       "group max node limit %u for "
				       "account %s",
				       job_ptr->job_id,
				       node_cnt,
				       assoc_ptr->grp_nodes,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}

			if ((assoc_ptr->usage->grp_used_nodes +
			     node_cnt) >
			    assoc_ptr->grp_nodes) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason = WAIT_ASSOC_GRP_NODES;
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max node limit %u "
				       "with already used %u + requested %u "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       assoc_ptr->grp_nodes,
				       assoc_ptr->usage->grp_used_nodes,
				       node_cnt,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs here */

		/* we don't need to check grp_wall here */


		/* We don't need to look at the regular limits for
		 * parents since we have pre-propogated them, so just
		 * continue with the next parent
		 */
		if (parent) {
			assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
			continue;
		}

		limit = slurmdb_find_tres_count_in_string(
			assoc_ptr->max_tres_mins_pj, TRES_CPU);
		if ((qos_rec.max_cpu_mins_pj == INFINITE) &&
		    (limit != (uint64_t)INFINITE)) {
			cpu_time_limit = limit;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_cpu_time_limit > cpu_time_limit)) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason =
					WAIT_ASSOC_MAX_CPU_MINS_PER_JOB;
				debug2("job %u being held, "
				       "cpu time limit %"PRIu64" exceeds "
				       "assoc max per job %"PRIu64"",
				       job_ptr->job_id,
				       job_cpu_time_limit,
				       cpu_time_limit);
				rc = false;
				goto end_it;
			}
		}

		limit = slurmdb_find_tres_count_in_string(
			assoc_ptr->max_tres_pj, TRES_CPU);
		if ((qos_rec.max_cpus_pj == INFINITE) &&
		    (limit != (uint64_t)INFINITE)) {
			if (cpu_cnt > limit) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason =
					WAIT_ASSOC_MAX_CPUS_PER_JOB;
				debug2("job %u being held, "
				       "min cpu limit %u exceeds "
				       "account max %"PRIu64,
				       job_ptr->job_id,
				       cpu_cnt,
				       limit);
				rc = false;
				goto end_it;
			}
		}

		/* we do not need to check max_jobs here */

		if ((qos_rec.max_nodes_pj == INFINITE)
		    && (assoc_ptr->max_nodes_pj != INFINITE)) {
			if (node_cnt > assoc_ptr->max_nodes_pj) {
				xfree(job_ptr->state_desc);
				job_ptr->state_reason =
					WAIT_ASSOC_MAX_NODE_PER_JOB;
				debug2("job %u being held, "
				       "min node limit %u exceeds "
				       "account max %u",
				       job_ptr->job_id,
				       node_cnt,
				       assoc_ptr->max_nodes_pj);
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs here */

		/* we don't need to check max_wall_pj here */

		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
	}
end_it:
	assoc_mgr_unlock(&locks);

	return rc;
}

extern uint32_t acct_policy_get_max_nodes(struct job_record *job_ptr,
					  uint32_t *wait_reason)
{
	uint32_t max_nodes_limit = INFINITE, qos_max_p_limit = INFINITE;
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_qos_rec_t qos_rec;
	slurmdb_assoc_rec_t *assoc_ptr = job_ptr->assoc_ptr;
	bool parent = 0; /* flag to tell us if we are looking at the
			  * parent or not
			  */
	bool grp_set = 0;

	/* check to see if we are enforcing associations */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return max_nodes_limit;

	xassert(wait_reason);

	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	if (qos_ptr_1) {
		memcpy(&qos_rec, qos_ptr_1, sizeof(slurmdb_qos_rec_t));
		if (qos_ptr_2) {
			if (qos_rec.max_nodes_pj == INFINITE)
				qos_rec.max_nodes_pj = qos_ptr_2->max_nodes_pj;
			if (qos_rec.max_nodes_pu == INFINITE)
				qos_rec.max_nodes_pu = qos_ptr_2->max_nodes_pu;
			if (qos_rec.grp_nodes == INFINITE)
				qos_rec.grp_nodes = qos_ptr_2->grp_nodes;
		}

		if (qos_rec.max_nodes_pj < qos_rec.max_nodes_pu) {
			max_nodes_limit = qos_rec.max_nodes_pj;
			*wait_reason = WAIT_QOS_MAX_NODE_PER_JOB;
		} else if (qos_rec.max_nodes_pu != INFINITE) {
			max_nodes_limit = qos_rec.max_nodes_pu;
			*wait_reason = WAIT_QOS_MAX_NODE_PER_USER;
		}

		qos_max_p_limit = max_nodes_limit;

		if (qos_rec.grp_nodes < max_nodes_limit) {
			max_nodes_limit = qos_rec.grp_nodes;
			*wait_reason = WAIT_QOS_GRP_NODES;
		}
	}

	/* We have to traverse all the associations because QOS might
	   not override a particular limit.
	*/
	while (assoc_ptr) {
		if ((!qos_ptr_1 || (qos_rec.grp_nodes == INFINITE))
		    && (assoc_ptr->grp_nodes != INFINITE)
		    && (assoc_ptr->grp_nodes < max_nodes_limit)) {
			max_nodes_limit = assoc_ptr->grp_nodes;
			*wait_reason = WAIT_ASSOC_GRP_NODES;
			grp_set = 1;
		}

		if (!parent
		    && (qos_max_p_limit == INFINITE)
		    && (assoc_ptr->max_nodes_pj != INFINITE)
		    && (assoc_ptr->max_nodes_pj < max_nodes_limit)) {
			max_nodes_limit = assoc_ptr->max_nodes_pj;
			*wait_reason = WAIT_ASSOC_MAX_NODE_PER_JOB;
		}

		/* only check the first grp set */
		if (grp_set)
			break;

		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
		continue;
	}

	assoc_mgr_unlock(&locks);
	return max_nodes_limit;
}

/*
 * acct_policy_update_pending_job - Make sure the limits imposed on a job on
 *	submission are correct after an update to a qos or association.  If
 *	the association/qos limits prevent the job from running (lowered
 *	limits since job submission), then reset its reason field.
 */
extern int acct_policy_update_pending_job(struct job_record *job_ptr)
{
	job_desc_msg_t job_desc;
	acct_policy_limit_set_t acct_policy_limit_set;
	bool update_accounting = false;
	struct job_details *details_ptr;
	int rc = SLURM_SUCCESS;

	/* check to see if we are enforcing associations and the job
	 * is pending or if we are even enforcing limits. */
	if (!accounting_enforce || !IS_JOB_PENDING(job_ptr)
	    || !(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return SLURM_SUCCESS;

	details_ptr = job_ptr->details;

	if (!details_ptr) {
		error("acct_policy_update_pending_job: no details");
		return SLURM_ERROR;
	}

	/* set up the job desc to make sure things are the way we
	 * need.
	 */
	slurm_init_job_desc_msg(&job_desc);

	/* copy the limits set from the job the only one that
	 * acct_policy_validate changes is the time limit so we
	 * should be ok with the memcpy here */
	memcpy(&acct_policy_limit_set, &job_ptr->limit_set,
	       sizeof(acct_policy_limit_set_t));

	/* set the min nodes */
	job_desc.min_nodes = details_ptr->min_nodes;

	/* copy all the tres requests over */
	memcpy(&job_desc.tres_req_cnt, &job_ptr->tres_req_cnt,
	       sizeof(uint64_t) * slurmctld_tres_cnt);

	/* Only set this value if not set from a limit */
	if (job_ptr->limit_set.time == ADMIN_SET_LIMIT)
		acct_policy_limit_set.time = job_ptr->limit_set.time;
	else if ((job_ptr->time_limit != NO_VAL) && !job_ptr->limit_set.time)
		job_desc.time_limit = job_ptr->time_limit;

	if (!acct_policy_validate(&job_desc, job_ptr->part_ptr,
				  job_ptr->assoc_ptr, job_ptr->qos_ptr,
				  &job_ptr->state_reason,
				  &acct_policy_limit_set, 0)) {
		info("acct_policy_update_pending_job: exceeded "
		     "association/qos's cpu, node, memory or "
		     "time limit for job %d", job_ptr->job_id);
		return SLURM_ERROR;
	}

	/* The only variable in acct_policy_limit_set that is changed
	 * in acct_policy_validate is the time limit so only worry
	 * about that one.
	 */

	/* If it isn't an admin set limit replace it. */
	if (!acct_policy_limit_set.time && (job_ptr->limit_set.time == 1)) {
		job_ptr->time_limit = NO_VAL;
		job_ptr->limit_set.time = 0;
		update_accounting = true;
	} else if (acct_policy_limit_set.time != ADMIN_SET_LIMIT) {
		if (job_ptr->time_limit != job_desc.time_limit) {
			job_ptr->time_limit = job_desc.time_limit;
			update_accounting = true;
		}
		job_ptr->limit_set.time = acct_policy_limit_set.time;
	}

	if (update_accounting) {
		last_job_update = time(NULL);
		debug("limits changed for job %u: updating accounting",
		      job_ptr->job_id);
		/* Update job record in accounting to reflect changes */
		jobacct_storage_job_start_direct(acct_db_conn, job_ptr);
	}

	return rc;
}

/*
 * acct_policy_job_runnable - Determine of the specified job has timed
 *	out based on it's QOS or association.
 */
extern bool acct_policy_job_time_out(struct job_record *job_ptr)
{
	uint64_t job_cpu_usage_mins = 0;
	uint64_t usage_mins;
	uint32_t wall_mins;
	slurmdb_qos_rec_t *qos_ptr_1, *qos_ptr_2;
	slurmdb_qos_rec_t qos_rec;
	slurmdb_assoc_rec_t *assoc = NULL;
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK,
				   NO_LOCK, NO_LOCK, NO_LOCK };
	time_t now;

	/* Now see if we are enforcing limits.  If Safe is set then
	 * return false as well since we are being safe if the limit
	 * was changed after the job was already deemed safe to start.
	 */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS)
	    || (accounting_enforce & ACCOUNTING_ENFORCE_SAFE))
		return false;

	slurmdb_init_qos_rec(&qos_rec, 0, INFINITE);
	assoc_mgr_lock(&locks);

	_set_qos_order(job_ptr, &qos_ptr_1, &qos_ptr_2);

	assoc =	(slurmdb_assoc_rec_t *)job_ptr->assoc_ptr;

	now = time(NULL);

	/* find out how many cpu minutes this job has been
	 * running for. */
	job_cpu_usage_mins = (uint64_t)
		((((now - job_ptr->start_time)
		   - job_ptr->tot_sus_time) / 60)
		 * job_ptr->total_cpus);

	/* check the first QOS setting it's values in the qos_rec */
	if (qos_ptr_1 && !_qos_job_time_out(job_ptr, qos_ptr_1,
					    &qos_rec, job_cpu_usage_mins))
		goto job_failed;

	/* If qos_ptr_1 didn't set the value use the 2nd QOS to set
	   the limit.
	*/
	if (qos_ptr_2 && !_qos_job_time_out(job_ptr, qos_ptr_2,
					    &qos_rec, job_cpu_usage_mins))
		goto job_failed;

	/* handle any association stuff here */
	while (assoc) {
		usage_mins = (uint64_t)(assoc->usage->usage_raw / 60.0);
		wall_mins = assoc->usage->grp_used_wall / 60;

		/* FIXME: this only works with CPUS and was only done
		 * this way to get the slurmctld to compile and work
		 * with the TRES strings.  This should probably be a
		 * new call to a function that does this for each TRES.
		 */
		uint64_t limit = slurmdb_find_tres_count_in_string(
			assoc->grp_tres_mins, TRES_CPU);
		if ((qos_rec.grp_cpu_mins == INFINITE)
		    && (limit != (uint64_t)INFINITE)
		    && (usage_mins >= limit)) {
			info("Job %u timed out, "
			     "assoc %u is at or exceeds "
			     "group max cpu minutes limit %"PRIu64" "
			     "with %"PRIu64" for account %s",
			     job_ptr->job_id, assoc->id,
			     limit,
			     usage_mins,
			     assoc->acct);
			job_ptr->state_reason = FAIL_TIMEOUT;
			break;
		}

		if ((qos_rec.grp_wall == INFINITE)
		    && (assoc->grp_wall != INFINITE)
		    && (wall_mins >= assoc->grp_wall)) {
			info("Job %u timed out, "
			     "assoc %u is at or exceeds "
			     "group wall limit %u "
			     "with %u for account %s",
			     job_ptr->job_id, assoc->id,
			     assoc->grp_wall,
			     wall_mins, assoc->acct);
			job_ptr->state_reason = FAIL_TIMEOUT;
			break;
		}

		limit = slurmdb_find_tres_count_in_string(
			assoc->max_tres_mins_pj, TRES_CPU);
		if ((qos_rec.max_cpu_mins_pj == INFINITE)
		    && (limit != (uint64_t)INFINITE)
		    && (job_cpu_usage_mins >= limit)) {
			info("Job %u timed out, "
			     "assoc %u is at or exceeds "
			     "max cpu minutes limit %"PRIu64" "
			     "with %"PRIu64" for account %s",
			     job_ptr->job_id, assoc->id,
			     limit,
			     job_cpu_usage_mins,
			     assoc->acct);
			job_ptr->state_reason = FAIL_TIMEOUT;
			break;
		}

		assoc = assoc->usage->parent_assoc_ptr;
		/* these limits don't apply to the root assoc */
		if (assoc == assoc_mgr_root_assoc)
			break;
	}
job_failed:
	assoc_mgr_unlock(&locks);

	if (job_ptr->state_reason == FAIL_TIMEOUT)
		return true;

	return false;
}
