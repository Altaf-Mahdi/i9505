<<<<<<< HEAD
 /* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)     "mpd %s: " fmt, __func__

#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/cpu.h>
#include <linux/stringify.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/rq_stats.h>
#include <asm/atomic.h>
#include <asm/page.h>
#include <mach/msm_dcvs.h>
#include <mach/msm_dcvs_scm.h>
#define CREATE_TRACE_POINTS
#include <trace/events/mpdcvs_trace.h>

#define DEFAULT_RQ_AVG_POLL_MS    (1)
#define DEFAULT_RQ_AVG_DIVIDE    (25)

struct mpd_attrib {
	struct kobj_attribute	enabled;
	struct kobj_attribute	rq_avg_poll_ms;
	struct kobj_attribute	iowait_threshold_pct;

	struct kobj_attribute	rq_avg_divide;
	struct kobj_attribute	em_win_size_min_us;
	struct kobj_attribute	em_win_size_max_us;
	struct kobj_attribute	em_max_util_pct;
	struct kobj_attribute	mp_em_rounding_point_min;
	struct kobj_attribute	mp_em_rounding_point_max;
	struct kobj_attribute	online_util_pct_min;
	struct kobj_attribute	online_util_pct_max;
	struct kobj_attribute	slack_time_min_us;
	struct kobj_attribute	slack_time_max_us;
	struct kobj_attribute	hp_up_max_ms;
	struct kobj_attribute	hp_up_ms;
	struct kobj_attribute	hp_up_count;
	struct kobj_attribute	hp_dw_max_ms;
	struct kobj_attribute	hp_dw_ms;
	struct kobj_attribute	hp_dw_count;
	struct attribute_group	attrib_group;
};

struct msm_mpd_scm_data {
	enum msm_dcvs_scm_event event;
	int			nr;
};

struct mpdecision {
	uint32_t			enabled;
	atomic_t			algo_cpu_mask;
	uint32_t			rq_avg_poll_ms;
	uint32_t			iowait_threshold_pct;
	uint32_t			rq_avg_divide;
	ktime_t				next_update;
	uint32_t			slack_us;
	struct msm_mpd_algo_param	mp_param;
	struct mpd_attrib		attrib;
	struct mutex			lock;
	struct task_struct		*task;
	struct task_struct		*hptask;
	struct hrtimer			slack_timer;
	struct msm_mpd_scm_data		data;
	int				hpupdate;
	wait_queue_head_t		wait_q;
	wait_queue_head_t		wait_hpq;
};

struct hp_latency {
	int hp_up_max_ms;
	int hp_up_ms;
	int hp_up_count;
	int hp_dw_max_ms;
	int hp_dw_ms;
	int hp_dw_count;
};

static DEFINE_PER_CPU(struct hrtimer, rq_avg_poll_timer);
static DEFINE_SPINLOCK(rq_avg_lock);

enum {
	MSM_MPD_DEBUG_NOTIFIER = BIT(0),
	MSM_MPD_CORE_STATUS = BIT(1),
	MSM_MPD_SLACK_TIMER = BIT(2),
};

enum {
	HPUPDATE_WAITING = 0, /* we are waiting for cpumask update */
	HPUPDATE_SCHEDULED = 1, /* we are in the process of hotplugging */
	HPUPDATE_IN_PROGRESS = 2, /* we are in the process of hotplugging */
};

static int msm_mpd_enabled = 1;
module_param_named(enabled, msm_mpd_enabled, int, S_IRUGO | S_IWUSR | S_IWGRP);

static struct dentry *debugfs_base;
static struct mpdecision msm_mpd;

static struct hp_latency hp_latencies;

static unsigned long last_nr;
static int num_present_hundreds;
static ktime_t last_down_time;

static bool ok_to_update_tz(int nr, int last_nr)
{
	/*
	 * Exclude unnecessary TZ reports if run queue haven't changed much from
	 * the last reported value. The divison by rq_avg_divide is to
	 * filter out small changes in the run queue average which won't cause
	 * a online cpu mask change. Also if the cpu online count does not match
	 * the count requested by TZ and we are not in the process of bringing
	 * cpus online as indicated by a HPUPDATE_IN_PROGRESS in msm_mpd.hpdata
	 */
	return
	(((nr / msm_mpd.rq_avg_divide)
				!= (last_nr / msm_mpd.rq_avg_divide))
	|| ((hweight32(atomic_read(&msm_mpd.algo_cpu_mask))
				!= num_online_cpus())
		&& (msm_mpd.hpupdate != HPUPDATE_IN_PROGRESS)));
}

static enum hrtimer_restart msm_mpd_rq_avg_poll_timer(struct hrtimer *timer)
{
	int nr, nr_iowait;
	ktime_t curr_time = ktime_get();
	unsigned long flags;
	int cpu = smp_processor_id();
	enum hrtimer_restart restart = HRTIMER_RESTART;

	spin_lock_irqsave(&rq_avg_lock, flags);
	/* If running on the wrong cpu, don't restart */
	if (&per_cpu(rq_avg_poll_timer, cpu) != timer)
		restart = HRTIMER_NORESTART;

	if (ktime_to_ns(ktime_sub(curr_time, msm_mpd.next_update)) < 0)
		goto out;

	msm_mpd.next_update = ktime_add_ns(curr_time,
			(msm_mpd.rq_avg_poll_ms * NSEC_PER_MSEC));

	sched_get_nr_running_avg(&nr, &nr_iowait);

	if ((nr_iowait >= msm_mpd.iowait_threshold_pct) && (nr < last_nr))
		nr = last_nr;

	if (nr > num_present_hundreds)
		nr = num_present_hundreds;

	trace_msm_mp_runq("nr_running", nr);

	if (ok_to_update_tz(nr, last_nr)) {
		hrtimer_try_to_cancel(&msm_mpd.slack_timer);
		msm_mpd.data.nr = nr;
		msm_mpd.data.event = MSM_DCVS_SCM_RUNQ_UPDATE;
		wake_up(&msm_mpd.wait_q);
		last_nr = nr;
	}

out:
	hrtimer_set_expires(timer, msm_mpd.next_update);
	spin_unlock_irqrestore(&rq_avg_lock, flags);
	/* set next expiration */
	return restart;
}

static void bring_up_cpu(int cpu)
{
	int cpu_action_time_ms;
	int time_taken_ms;
	int ret, ret1, ret2;

	cpu_action_time_ms = ktime_to_ms(ktime_get());
	ret = cpu_up(cpu);
	if (ret) {
		pr_debug("Error %d online core %d\n", ret, cpu);
	} else {
		time_taken_ms = ktime_to_ms(ktime_get()) - cpu_action_time_ms;
		if (time_taken_ms > hp_latencies.hp_up_max_ms)
			hp_latencies.hp_up_max_ms = time_taken_ms;
		hp_latencies.hp_up_ms += time_taken_ms;
		hp_latencies.hp_up_count++;
		ret = msm_dcvs_scm_event(
				CPU_OFFSET + cpu,
				MSM_DCVS_SCM_CORE_ONLINE,
				cpufreq_get(cpu),
				(uint32_t) time_taken_ms * USEC_PER_MSEC,
				&ret1, &ret2);
		if (ret)
			pr_err("Error sending hotplug scm event err=%d\n", ret);
	}
}

static void bring_down_cpu(int cpu)
{
	int cpu_action_time_ms;
	int time_taken_ms;
	int ret, ret1, ret2;

	BUG_ON(cpu == 0);
	cpu_action_time_ms = ktime_to_ms(ktime_get());
	ret = cpu_down(cpu);
	if (ret) {
		pr_debug("Error %d offline" "core %d\n", ret, cpu);
	} else {
		time_taken_ms = ktime_to_ms(ktime_get()) - cpu_action_time_ms;
		if (time_taken_ms > hp_latencies.hp_dw_max_ms)
			hp_latencies.hp_dw_max_ms = time_taken_ms;
		hp_latencies.hp_dw_ms += time_taken_ms;
		hp_latencies.hp_dw_count++;
		ret = msm_dcvs_scm_event(
				CPU_OFFSET + cpu,
				MSM_DCVS_SCM_CORE_OFFLINE,
				(uint32_t) time_taken_ms * USEC_PER_MSEC,
				0,
				&ret1, &ret2);
		if (ret)
			pr_err("Error sending hotplug scm event err=%d\n", ret);
	}
}

static int __ref msm_mpd_update_scm(enum msm_dcvs_scm_event event, int nr)
{
	int ret = 0;
	uint32_t req_cpu_mask = 0;
	uint32_t slack_us = 0;
	uint32_t param0 = 0;

	if (event == MSM_DCVS_SCM_RUNQ_UPDATE)
		param0 = nr;

	ret = msm_dcvs_scm_event(0, event, param0, 0,
				 &req_cpu_mask, &slack_us);

	if (ret) {
		pr_err("Error (%d) sending event %d, param %d\n", ret, event,
				param0);
		return ret;
	}

	trace_msm_mp_cpusonline("cpu_online_mp", req_cpu_mask);
	trace_msm_mp_slacktime("slack_time_mp", slack_us);
	msm_mpd.slack_us = slack_us;
	atomic_set(&msm_mpd.algo_cpu_mask, req_cpu_mask);
	msm_mpd.hpupdate = HPUPDATE_SCHEDULED;
	wake_up(&msm_mpd.wait_hpq);

	/* Start MP Decision slack timer */
	if (slack_us) {
		hrtimer_cancel(&msm_mpd.slack_timer);
		ret = hrtimer_start(&msm_mpd.slack_timer,
				ktime_set(0, slack_us * NSEC_PER_USEC),
				HRTIMER_MODE_REL_PINNED);
		if (ret)
			pr_err("Failed to register slack timer (%d) %d\n",
					slack_us, ret);
	}

	return ret;
}

static enum hrtimer_restart msm_mpd_slack_timer(struct hrtimer *timer)
{
	unsigned long flags;

	trace_printk("mpd:slack_timer_fired!\n");

	spin_lock_irqsave(&rq_avg_lock, flags);
	if (msm_mpd.data.event == MSM_DCVS_SCM_RUNQ_UPDATE)
		goto out;

	msm_mpd.data.nr = 0;
	msm_mpd.data.event = MSM_DCVS_SCM_MPD_QOS_TIMER_EXPIRED;
	wake_up(&msm_mpd.wait_q);
out:
	spin_unlock_irqrestore(&rq_avg_lock, flags);
	return HRTIMER_NORESTART;
}

static int msm_mpd_idle_notifier(struct notifier_block *self,
				 unsigned long cmd, void *v)
{
	int cpu = smp_processor_id();
	unsigned long flags;

	switch (cmd) {
	case CPU_PM_EXIT:
		spin_lock_irqsave(&rq_avg_lock, flags);
		hrtimer_start(&per_cpu(rq_avg_poll_timer, cpu),
			      msm_mpd.next_update,
			      HRTIMER_MODE_ABS_PINNED);
		spin_unlock_irqrestore(&rq_avg_lock, flags);
		break;
	case CPU_PM_ENTER:
		hrtimer_cancel(&per_cpu(rq_avg_poll_timer, cpu));
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static int msm_mpd_hotplug_notifier(struct notifier_block *self,
				    unsigned long action, void *hcpu)
{
	int cpu = (int)hcpu;
	unsigned long flags;

	switch (action & (~CPU_TASKS_FROZEN)) {
	case CPU_STARTING:
		spin_lock_irqsave(&rq_avg_lock, flags);
		hrtimer_start(&per_cpu(rq_avg_poll_timer, cpu),
			      msm_mpd.next_update,
			      HRTIMER_MODE_ABS_PINNED);
		spin_unlock_irqrestore(&rq_avg_lock, flags);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block msm_mpd_idle_nb = {
	.notifier_call = msm_mpd_idle_notifier,
};

static struct notifier_block msm_mpd_hotplug_nb = {
	.notifier_call = msm_mpd_hotplug_notifier,
};

static int __cpuinit msm_mpd_do_hotplug(void *data)
{
	int *event = (int *)data;
	int cpu;

	while (1) {
		wait_event(msm_mpd.wait_hpq, *event || kthread_should_stop());
		if (kthread_should_stop())
			break;

		msm_mpd.hpupdate = HPUPDATE_IN_PROGRESS;
		/*
		 * Bring online any offline cores, then offline any online
		 * cores.  Whenever a core is off/onlined restart the procedure
		 * in case a new core is desired to be brought online in the
		 * mean time.
		 */
restart:
		for_each_possible_cpu(cpu) {
			if ((atomic_read(&msm_mpd.algo_cpu_mask) & (1 << cpu))
				&& !cpu_online(cpu)) {
				bring_up_cpu(cpu);
				if (cpu_online(cpu))
					goto restart;
			}
		}

		if (ktime_to_ns(ktime_sub(ktime_get(), last_down_time)) >
		    100 * NSEC_PER_MSEC)
			for_each_possible_cpu(cpu)
				if (!(atomic_read(&msm_mpd.algo_cpu_mask) &
				      (1 << cpu)) && cpu_online(cpu)) {
					bring_down_cpu(cpu);
					last_down_time = ktime_get();
					break;
				}
		msm_mpd.hpupdate = HPUPDATE_WAITING;
		msm_dcvs_apply_gpu_floor(0);
		msm_dcvs_update_algo_params();
	}

	return 0;
}

static int msm_mpd_do_update_scm(void *data)
{
	struct msm_mpd_scm_data *scm_data = (struct msm_mpd_scm_data *)data;
	unsigned long flags;
	enum msm_dcvs_scm_event event;
	int nr;

	while (1) {
		wait_event(msm_mpd.wait_q,
			msm_mpd.data.event == MSM_DCVS_SCM_MPD_QOS_TIMER_EXPIRED
			|| msm_mpd.data.event == MSM_DCVS_SCM_RUNQ_UPDATE
			|| kthread_should_stop());

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&rq_avg_lock, flags);
		event = scm_data->event;
		nr = scm_data->nr;
		scm_data->event = 0;
		scm_data->nr = 0;
		spin_unlock_irqrestore(&rq_avg_lock, flags);

		msm_mpd_update_scm(event, nr);
	}
	return 0;
}

static int __ref msm_mpd_set_enabled(uint32_t enable)
{
	int ret = 0;
	int ret0 = 0;
	int ret1 = 0;
	int cpu;
	static uint32_t last_enable;

	enable = (enable > 0) ? 1 : 0;
	if (last_enable == enable)
		return ret;

	if (enable) {
		ret = msm_mpd_scm_set_algo_params(&msm_mpd.mp_param);
		if (ret) {
			pr_err("Error(%d): msm_mpd_scm_set_algo_params failed\n",
				ret);
			return ret;
		}
	}

	ret = msm_dcvs_scm_event(0, MSM_DCVS_SCM_MPD_ENABLE, enable, 0,
			&ret0, &ret1);
	if (ret) {
		pr_err("Error(%d) %s MP Decision\n",
				ret, (enable ? "enabling" : "disabling"));
	} else {
		last_enable = enable;
		last_nr = 0;
	}
	if (enable) {
		msm_mpd.next_update = ktime_add_ns(ktime_get(),
				(msm_mpd.rq_avg_poll_ms * NSEC_PER_MSEC));
		msm_mpd.task = kthread_run(msm_mpd_do_update_scm,
					      &msm_mpd.data, "msm_mpdecision");
		if (IS_ERR(msm_mpd.task))
			return -EFAULT;

		msm_mpd.hptask = kthread_run(msm_mpd_do_hotplug,
						&msm_mpd.hpupdate, "msm_hp");
		if (IS_ERR(msm_mpd.hptask))
			return -EFAULT;

		for_each_online_cpu(cpu)
			hrtimer_start(&per_cpu(rq_avg_poll_timer, cpu),
				      msm_mpd.next_update,
				      HRTIMER_MODE_ABS_PINNED);
		cpu_pm_register_notifier(&msm_mpd_idle_nb);
		register_cpu_notifier(&msm_mpd_hotplug_nb);
		msm_mpd.enabled = 1;
	} else {
		for_each_online_cpu(cpu)
			hrtimer_cancel(&per_cpu(rq_avg_poll_timer, cpu));
		kthread_stop(msm_mpd.hptask);
		kthread_stop(msm_mpd.task);
		cpu_pm_unregister_notifier(&msm_mpd_idle_nb);
		unregister_cpu_notifier(&msm_mpd_hotplug_nb);
		msm_mpd.enabled = 0;
	}

	return ret;
}

static int msm_mpd_set_rq_avg_poll_ms(uint32_t val)
{
	/*
	 * No need to do anything. Just let the timer set its own next poll
	 * interval when it next fires.
	 */
	msm_mpd.rq_avg_poll_ms = val;
	return 0;
}

static int msm_mpd_set_iowait_threshold_pct(uint32_t val)
{
	/*
	 * No need to do anything. Just let the timer set its own next poll
	 * interval when it next fires.
	 */
	msm_mpd.iowait_threshold_pct = val;
	return 0;
}

static int msm_mpd_set_rq_avg_divide(uint32_t val)
{
	/*
	 * No need to do anything. New value will be used next time
	 * the decision is made as to whether to update tz.
	 */

	if (val == 0)
		return -EINVAL;

	msm_mpd.rq_avg_divide = val;
	return 0;
}

#define MPD_ALGO_PARAM(_name, _param) \
static ssize_t msm_mpd_attr_##_name##_show(struct kobject *kobj, \
			struct kobj_attribute *attr, char *buf) \
{ \
	return snprintf(buf, PAGE_SIZE, "%d\n", _param); \
} \
static ssize_t msm_mpd_attr_##_name##_store(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	int ret = 0; \
	uint32_t val; \
	uint32_t old_val; \
	mutex_lock(&msm_mpd.lock); \
	ret = kstrtouint(buf, 10, &val); \
	if (ret) { \
		pr_err("Invalid input %s for %s %d\n", \
				buf, __stringify(_name), ret);\
		return 0; \
	} \
	old_val = _param; \
	_param = val; \
	ret = msm_mpd_scm_set_algo_params(&msm_mpd.mp_param); \
	if (ret) { \
		pr_err("Error %d returned when setting algo param %s to %d\n",\
				ret, __stringify(_name), val); \
		_param = old_val; \
	} \
	mutex_unlock(&msm_mpd.lock); \
	return count; \
}

#define MPD_PARAM(_name, _param) \
static ssize_t msm_mpd_attr_##_name##_show(struct kobject *kobj, \
			struct kobj_attribute *attr, char *buf) \
{ \
	return snprintf(buf, PAGE_SIZE, "%d\n", _param); \
} \
static ssize_t msm_mpd_attr_##_name##_store(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	int ret = 0; \
	uint32_t val; \
	uint32_t old_val; \
	mutex_lock(&msm_mpd.lock); \
	ret = kstrtouint(buf, 10, &val); \
	if (ret) { \
		pr_err("Invalid input %s for %s %d\n", \
				buf, __stringify(_name), ret);\
		return 0; \
	} \
	old_val = _param; \
	ret = msm_mpd_set_##_name(val); \
	if (ret) { \
		pr_err("Error %d returned when setting algo param %s to %d\n",\
				ret, __stringify(_name), val); \
		_param = old_val; \
	} \
	mutex_unlock(&msm_mpd.lock); \
	return count; \
}

#define MPD_RW_ATTRIB(i, _name) \
	msm_mpd.attrib._name.attr.name = __stringify(_name); \
	msm_mpd.attrib._name.attr.mode = S_IRUGO | S_IWUSR; \
	msm_mpd.attrib._name.show = msm_mpd_attr_##_name##_show; \
	msm_mpd.attrib._name.store = msm_mpd_attr_##_name##_store; \
	msm_mpd.attrib.attrib_group.attrs[i] = &msm_mpd.attrib._name.attr;

MPD_PARAM(enabled, msm_mpd.enabled);
MPD_PARAM(rq_avg_poll_ms, msm_mpd.rq_avg_poll_ms);
MPD_PARAM(iowait_threshold_pct, msm_mpd.iowait_threshold_pct);
MPD_PARAM(rq_avg_divide, msm_mpd.rq_avg_divide);
MPD_ALGO_PARAM(em_win_size_min_us, msm_mpd.mp_param.em_win_size_min_us);
MPD_ALGO_PARAM(em_win_size_max_us, msm_mpd.mp_param.em_win_size_max_us);
MPD_ALGO_PARAM(em_max_util_pct, msm_mpd.mp_param.em_max_util_pct);
MPD_ALGO_PARAM(mp_em_rounding_point_min,
				msm_mpd.mp_param.mp_em_rounding_point_min);
MPD_ALGO_PARAM(mp_em_rounding_point_max,
				msm_mpd.mp_param.mp_em_rounding_point_max);
MPD_ALGO_PARAM(online_util_pct_min, msm_mpd.mp_param.online_util_pct_min);
MPD_ALGO_PARAM(online_util_pct_max, msm_mpd.mp_param.online_util_pct_max);
MPD_ALGO_PARAM(slack_time_min_us, msm_mpd.mp_param.slack_time_min_us);
MPD_ALGO_PARAM(slack_time_max_us, msm_mpd.mp_param.slack_time_max_us);
MPD_ALGO_PARAM(hp_up_max_ms, hp_latencies.hp_up_max_ms);
MPD_ALGO_PARAM(hp_up_ms, hp_latencies.hp_up_ms);
MPD_ALGO_PARAM(hp_up_count, hp_latencies.hp_up_count);
MPD_ALGO_PARAM(hp_dw_max_ms, hp_latencies.hp_dw_max_ms);
MPD_ALGO_PARAM(hp_dw_ms, hp_latencies.hp_dw_ms);
MPD_ALGO_PARAM(hp_dw_count, hp_latencies.hp_dw_count);

static int __devinit msm_mpd_probe(struct platform_device *pdev)
{
	struct kobject *module_kobj = NULL;
	int ret = 0;
	const int attr_count = 20;
	struct msm_mpd_algo_param *param = NULL;

	param = pdev->dev.platform_data;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("Cannot find kobject for module %s\n", KBUILD_MODNAME);
		ret = -ENOENT;
		goto done;
	}

	msm_mpd.attrib.attrib_group.attrs =
		kzalloc(attr_count * sizeof(struct attribute *), GFP_KERNEL);
	if (!msm_mpd.attrib.attrib_group.attrs) {
		ret = -ENOMEM;
		goto done;
	}

	MPD_RW_ATTRIB(0, enabled);
	MPD_RW_ATTRIB(1, rq_avg_poll_ms);
	MPD_RW_ATTRIB(2, iowait_threshold_pct);
	MPD_RW_ATTRIB(3, rq_avg_divide);
	MPD_RW_ATTRIB(4, em_win_size_min_us);
	MPD_RW_ATTRIB(5, em_win_size_max_us);
	MPD_RW_ATTRIB(6, em_max_util_pct);
	MPD_RW_ATTRIB(7, mp_em_rounding_point_min);
	MPD_RW_ATTRIB(8, mp_em_rounding_point_max);
	MPD_RW_ATTRIB(9, online_util_pct_min);
	MPD_RW_ATTRIB(10, online_util_pct_max);
	MPD_RW_ATTRIB(11, slack_time_min_us);
	MPD_RW_ATTRIB(12, slack_time_max_us);
	MPD_RW_ATTRIB(13, hp_up_max_ms);
	MPD_RW_ATTRIB(14, hp_up_ms);
	MPD_RW_ATTRIB(15, hp_up_count);
	MPD_RW_ATTRIB(16, hp_dw_max_ms);
	MPD_RW_ATTRIB(17, hp_dw_ms);
	MPD_RW_ATTRIB(18, hp_dw_count);

	msm_mpd.attrib.attrib_group.attrs[19] = NULL;
	ret = sysfs_create_group(module_kobj, &msm_mpd.attrib.attrib_group);
	if (ret)
		pr_err("Unable to create sysfs objects :%d\n", ret);

	msm_mpd.rq_avg_poll_ms = DEFAULT_RQ_AVG_POLL_MS;
	msm_mpd.rq_avg_divide = DEFAULT_RQ_AVG_DIVIDE;

	memcpy(&msm_mpd.mp_param, param, sizeof(struct msm_mpd_algo_param));

	debugfs_base = debugfs_create_dir("msm_mpdecision", NULL);
	if (!debugfs_base) {
		pr_err("Cannot create debugfs base msm_mpdecision\n");
		ret = -ENOENT;
		goto done;
	}

done:
	if (ret && debugfs_base)
		debugfs_remove(debugfs_base);

	return ret;
}

static int __devexit msm_mpd_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver msm_mpd_driver = {
	.probe	= msm_mpd_probe,
	.remove	= __devexit_p(msm_mpd_remove),
	.driver	= {
		.name	= "msm_mpdecision",
		.owner	= THIS_MODULE,
	},
};

static int __init msm_mpdecision_init(void)
{
	int cpu;
	if (!msm_mpd_enabled) {
		pr_info("Not enabled\n");
		return 0;
	}

	num_present_hundreds = 100 * num_present_cpus();

	hrtimer_init(&msm_mpd.slack_timer, CLOCK_MONOTONIC,
			HRTIMER_MODE_REL_PINNED);
	msm_mpd.slack_timer.function = msm_mpd_slack_timer;

	for_each_possible_cpu(cpu) {
		hrtimer_init(&per_cpu(rq_avg_poll_timer, cpu),
			     CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
		per_cpu(rq_avg_poll_timer, cpu).function
				= msm_mpd_rq_avg_poll_timer;
	}
	mutex_init(&msm_mpd.lock);
	init_waitqueue_head(&msm_mpd.wait_q);
	init_waitqueue_head(&msm_mpd.wait_hpq);
	return platform_driver_register(&msm_mpd_driver);
}
late_initcall(msm_mpdecision_init);
=======
/*
 * arch/arm/mach-msm/msm_mpdecision.c
 *
 * cpu auto-hotplug/unplug based on system load for MSM dualcore cpus
 * single core while screen is off
 *
 * Copyright (c) 2012, Dennis Rassmann <showp1984@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <asm-generic/cputime.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>

#define DEBUG 1

#define MPDEC_TAG                       "[MPDEC]: "
#define MSM_MPDEC_STARTDELAY            70000
#define MSM_MPDEC_DELAY                 500
#define MSM_MPDEC_PAUSE                 10000
#define MSM_MPDEC_IDLE_FREQ             486000

enum {
	MSM_MPDEC_DISABLED = 0,
	MSM_MPDEC_IDLE,
	MSM_MPDEC_DOWN,
	MSM_MPDEC_UP,
};

struct msm_mpdec_cpudata_t {
	struct mutex suspend_mutex;
	int online;
	int device_suspended;
	cputime64_t on_time;
};
static DEFINE_PER_CPU(struct msm_mpdec_cpudata_t, msm_mpdec_cpudata);

static struct delayed_work msm_mpdec_work;
static DEFINE_MUTEX(msm_cpu_lock);

static struct msm_mpdec_tuners {
	unsigned int startdelay;
	unsigned int delay;
	unsigned int pause;
	bool scroff_single_core;
	unsigned long int idle_freq;
} msm_mpdec_tuners_ins = {
	.startdelay = MSM_MPDEC_STARTDELAY,
	.delay = MSM_MPDEC_DELAY,
	.pause = MSM_MPDEC_PAUSE,
	.scroff_single_core = true,
	.idle_freq = MSM_MPDEC_IDLE_FREQ,
};

static unsigned int NwNs_Threshold[8] = {19, 30, 19, 11, 19, 11, 0, 11};
static unsigned int TwTs_Threshold[8] = {140, 0, 140, 190, 140, 190, 0, 190};

extern unsigned int get_rq_info(void);
extern unsigned long acpuclk_8x60_get_rate(int);

unsigned int state = MSM_MPDEC_IDLE;
bool was_paused = false;

static int get_slowest_cpu(void)
{
        int i, cpu = 0;
        unsigned long rate, slow_rate = 0;

        for (i = 0; i < CONFIG_NR_CPUS; i++) {
                if (!cpu_online(i))
                        continue;

                rate = get_rate(i);
                if (slow_rate == 0) {
                        slow_rate = rate;
                }

                if ((rate <= slow_rate) && (slow_rate != 0)) {
                        if (i == 0)
                                continue;
                        cpu = i;
                        slow_rate = rate;
                }
        }

        return cpu;
}

static int get_slowest_cpu_rate(void)
{
        int i = 0;
        unsigned long rate, slow_rate = 0;

        for (i = 0; i < CONFIG_NR_CPUS; i++) {
                rate = get_rate(i);
                if ((rate < slow_rate) && (slow_rate != 0)) {
                        slow_rate = rate;
                }
                if (slow_rate == 0) {
                        slow_rate = rate;
                }
        }

        return slow_rate;
}

static int mp_decision(void)
{
	static bool first_call = true;
	int new_state = MSM_MPDEC_IDLE;
	int nr_cpu_online;
	int index;
	unsigned int rq_depth;
	static cputime64_t total_time = 0;
	static cputime64_t last_time;
	cputime64_t current_time;
	cputime64_t this_time = 0;

	if (state == MSM_MPDEC_DISABLED)
		return MSM_MPDEC_DISABLED;

	current_time = ktime_to_ms(ktime_get());
	if (current_time <= msm_mpdec_tuners_ins.startdelay)
		return MSM_MPDEC_IDLE;

	if (first_call) {
		first_call = false;
	} else {
		this_time = current_time - last_time;
	}
	total_time += this_time;

	rq_depth = get_rq_info();
	nr_cpu_online = num_online_cpus();

	if (nr_cpu_online) {
		index = (nr_cpu_online - 1) * 2;
		if ((nr_cpu_online < CONFIG_NR_CPUS) && (rq_depth >= NwNs_Threshold[index])) {
			if (total_time >= TwTs_Threshold[index]) {
				new_state = MSM_MPDEC_UP;
                                if (get_slowest_cpu_rate() <=  msm_mpdec_tuners_ins.idle_freq)
                                        new_state = MSM_MPDEC_IDLE;
			}
		} else if ((nr_cpu_online > 1) && (rq_depth <= NwNs_Threshold[index+1])) {
			if (total_time >= TwTs_Threshold[index+1] ) {
				new_state = MSM_MPDEC_DOWN;
                                if (get_slowest_cpu_rate() > msm_mpdec_tuners_ins.idle_freq)
			                new_state = MSM_MPDEC_IDLE;
			}
		} else {
			new_state = MSM_MPDEC_IDLE;
			total_time = 0;
		}
	} else {
		total_time = 0;
	}

	if (new_state != MSM_MPDEC_IDLE) {
		total_time = 0;
	}

	last_time = ktime_to_ms(ktime_get());
#if DEBUG
        pr_info(MPDEC_TAG"[DEBUG] rq: %u, new_state: %i | Mask=[%d%d%d%d]\n",
                rq_depth, new_state, cpu_online(0), cpu_online(1), cpu_online(2), cpu_online(3));
#endif
	return new_state;
}

static void msm_mpdec_work_thread(struct work_struct *work)
{
	unsigned int cpu = nr_cpu_ids;
	cputime64_t on_time = 0;
        bool suspended = false;

        for_each_possible_cpu(cpu) {
                if ((per_cpu(msm_mpdec_cpudata, cpu).device_suspended == true)) {
                        suspended = true;
                        break;
                }
        }
	if (suspended == true)
		goto out;

	if (!mutex_trylock(&msm_cpu_lock))
		goto out;

	/* if sth messed with the cpus, update the check vars so we can proceed */
	if (was_paused) {
		for_each_possible_cpu(cpu) {
			if (cpu_online(cpu))
				per_cpu(msm_mpdec_cpudata, cpu).online = true;
			else if (!cpu_online(cpu))
				per_cpu(msm_mpdec_cpudata, cpu).online = false;
		}
		was_paused = false;
	}

	state = mp_decision();
	switch (state) {
	case MSM_MPDEC_DISABLED:
	case MSM_MPDEC_IDLE:
		break;
	case MSM_MPDEC_DOWN:
		cpu = get_slowest_cpu();
		if (cpu < nr_cpu_ids) {
			if ((per_cpu(msm_mpdec_cpudata, cpu).online == true) && (cpu_online(cpu))) {
				cpu_down(cpu);
				per_cpu(msm_mpdec_cpudata, cpu).online = false;
				on_time = ktime_to_ms(ktime_get()) - per_cpu(msm_mpdec_cpudata, cpu).on_time;
				pr_info(MPDEC_TAG"CPU[%d] on->off | Mask=[%d%d%d%d] | time online: %llu\n",
						cpu, cpu_online(0), cpu_online(1), cpu_online(2), cpu_online(3), on_time);
			} else if (per_cpu(msm_mpdec_cpudata, cpu).online != cpu_online(cpu)) {
				pr_info(MPDEC_TAG"CPU[%d] was controlled outside of mpdecision! | pausing [%d]ms\n",
						cpu, msm_mpdec_tuners_ins.pause);
				msleep(msm_mpdec_tuners_ins.pause);
				was_paused = true;
			}
		}
		break;
	case MSM_MPDEC_UP:
		cpu = cpumask_next_zero(0, cpu_online_mask);;
		if (cpu < nr_cpu_ids) {
			if ((per_cpu(msm_mpdec_cpudata, cpu).online == false) && (!cpu_online(cpu))) {
				cpu_up(cpu);
				per_cpu(msm_mpdec_cpudata, cpu).online = true;
				per_cpu(msm_mpdec_cpudata, cpu).on_time = ktime_to_ms(ktime_get());
				pr_info(MPDEC_TAG"CPU[%d] off->on | Mask=[%d%d%d%d]\n",
						cpu, cpu_online(0), cpu_online(1), cpu_online(2), cpu_online(3));
			} else if (per_cpu(msm_mpdec_cpudata, cpu).online != cpu_online(cpu)) {
				pr_info(MPDEC_TAG"CPU[%d] was controlled outside of mpdecision! | pausing [%d]ms\n",
						cpu, msm_mpdec_tuners_ins.pause);
				msleep(msm_mpdec_tuners_ins.pause);
				was_paused = true;
			}
		}
		break;
	default:
		pr_err(MPDEC_TAG"%s: invalid mpdec hotplug state %d\n",
		       __func__, state);
	}
	mutex_unlock(&msm_cpu_lock);

out:
	if (state != MSM_MPDEC_DISABLED)
		schedule_delayed_work(&msm_mpdec_work,
				msecs_to_jiffies(msm_mpdec_tuners_ins.delay));
	return;
}

static void msm_mpdec_early_suspend(struct early_suspend *h)
{
	int cpu = 0;
	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(msm_mpdec_cpudata, cpu).suspend_mutex);
		if (((cpu >= (CONFIG_NR_CPUS - 1)) && (num_online_cpus() > 1)) && (msm_mpdec_tuners_ins.scroff_single_core)) {
			cpu_down(cpu);
			pr_info(MPDEC_TAG"Screen -> off. Suspended CPU%d | Mask=[%d%d]\n",
					cpu, cpu_online(0), cpu_online(1));
			per_cpu(msm_mpdec_cpudata, cpu).online = false;
		}
		per_cpu(msm_mpdec_cpudata, cpu).device_suspended = true;
		mutex_unlock(&per_cpu(msm_mpdec_cpudata, cpu).suspend_mutex);
	}
}

static void msm_mpdec_late_resume(struct early_suspend *h)
{
	int cpu = 0;
	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(msm_mpdec_cpudata, cpu).suspend_mutex);
		if ((cpu >= (CONFIG_NR_CPUS - 1)) && (num_online_cpus() < CONFIG_NR_CPUS)) {
			/* Always enable cpus when screen comes online.
			 * This boosts the wakeup process.
			 */
			cpu_up(cpu);
			per_cpu(msm_mpdec_cpudata, cpu).on_time = ktime_to_ms(ktime_get());
			per_cpu(msm_mpdec_cpudata, cpu).online = true;
			pr_info(MPDEC_TAG"Screen -> on. Hot plugged CPU%d | Mask=[%d%d]\n",
					cpu, cpu_online(0), cpu_online(1));
		}
		per_cpu(msm_mpdec_cpudata, cpu).device_suspended = false;
		mutex_unlock(&per_cpu(msm_mpdec_cpudata, cpu).suspend_mutex);
	}
}

static struct early_suspend msm_mpdec_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = msm_mpdec_early_suspend,
	.resume = msm_mpdec_late_resume,
};

/**************************** SYSFS START ****************************/
struct kobject *msm_mpdec_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)               \
{									\
	return sprintf(buf, "%u\n", msm_mpdec_tuners_ins.object);	\
}

show_one(startdelay, startdelay);
show_one(delay, delay);
show_one(pause, pause);
show_one(scroff_single_core, scroff_single_core);

static ssize_t show_idle_freq (struct kobject *kobj, struct attribute *attr,
                                   char *buf)
{
	return sprintf(buf, "%lu\n", msm_mpdec_tuners_ins.idle_freq);
}

static ssize_t show_enabled(struct kobject *a, struct attribute *b,
				   char *buf)
{
	unsigned int enabled;
	switch (state) {
	case MSM_MPDEC_DISABLED:
		enabled = 0;
		break;
	case MSM_MPDEC_IDLE:
	case MSM_MPDEC_DOWN:
	case MSM_MPDEC_UP:
		enabled = 1;
		break;
	default:
		enabled = 333;
	}
	return sprintf(buf, "%u\n", enabled);
}

static ssize_t show_nwns_threshold_up(struct kobject *kobj, struct attribute *attr,
					char *buf)
{
	return sprintf(buf, "%u\n", NwNs_Threshold[0]);
}

static ssize_t show_nwns_threshold_down(struct kobject *kobj, struct attribute *attr,
					char *buf)
{
	return sprintf(buf, "%u\n", NwNs_Threshold[3]);
}

static ssize_t show_twts_threshold_up(struct kobject *kobj, struct attribute *attr,
					char *buf)
{
	return sprintf(buf, "%u\n", TwTs_Threshold[0]);
}

static ssize_t show_twts_threshold_down(struct kobject *kobj, struct attribute *attr,
					char *buf)
{
	return sprintf(buf, "%u\n", TwTs_Threshold[3]);
}

static ssize_t store_startdelay(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_mpdec_tuners_ins.startdelay = input;

	return count;
}

static ssize_t store_delay(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_mpdec_tuners_ins.delay = input;

	return count;
}

static ssize_t store_pause(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_mpdec_tuners_ins.pause = input;

	return count;
}

static ssize_t store_idle_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	long unsigned int input, check;
	int ret;
	ret = sscanf(buf, "%lu", &input);
	if (ret != 1)
		return -EINVAL;

	check = acpu_check_khz_value(input);

	if (check == 1) {
		msm_mpdec_tuners_ins.idle_freq = input;
	}
	if (check == 0) {
		msm_mpdec_tuners_ins.idle_freq = MSM_MPDEC_IDLE_FREQ;
	}
	if (check > 1) {
		msm_mpdec_tuners_ins.idle_freq = check;
	}

	return count;
}

static ssize_t store_scroff_single_core(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	switch (buf[0]) {
	case '0':
		msm_mpdec_tuners_ins.scroff_single_core = input;
		break;
	case '1':
		msm_mpdec_tuners_ins.scroff_single_core = input;
		break;
	default:
		ret = -EINVAL;
	}
	return count;
}

static ssize_t store_enabled(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int cpu, input, enabled;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	switch (state) {
	case MSM_MPDEC_DISABLED:
		enabled = 0;
		break;
	case MSM_MPDEC_IDLE:
	case MSM_MPDEC_DOWN:
	case MSM_MPDEC_UP:
		enabled = 1;
		break;
	default:
		enabled = 333;
	}

	if (buf[0] == enabled)
		return -EINVAL;

	switch (buf[0]) {
	case '0':
		state = MSM_MPDEC_DISABLED;
		cpu = (CONFIG_NR_CPUS - 1);
		if (!cpu_online(cpu)) {
			per_cpu(msm_mpdec_cpudata, cpu).on_time = ktime_to_ms(ktime_get());
			per_cpu(msm_mpdec_cpudata, cpu).online = true;
			cpu_up(cpu);
			pr_info(MPDEC_TAG"nap time... Hot plugged CPU[%d] | Mask=[%d%d]\n",
					 cpu, cpu_online(0), cpu_online(1));
		} else {
			pr_info(MPDEC_TAG"nap time...\n");
		}
		break;
	case '1':
		state = MSM_MPDEC_IDLE;
		was_paused = true;
		schedule_delayed_work(&msm_mpdec_work, 0);
		pr_info(MPDEC_TAG"firing up mpdecision...\n");
		break;
	default:
		ret = -EINVAL;
	}
	return count;
}

static ssize_t store_nwns_threshold_up(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	NwNs_Threshold[0] = input;

	return count;
}

static ssize_t store_nwns_threshold_down(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	NwNs_Threshold[3] = input;

	return count;
}

static ssize_t store_twts_threshold_up(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	TwTs_Threshold[0] = input;

	return count;
}

static ssize_t store_twts_threshold_down(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	TwTs_Threshold[3] = input;

	return count;
}

define_one_global_rw(startdelay);
define_one_global_rw(delay);
define_one_global_rw(pause);
define_one_global_rw(scroff_single_core);
define_one_global_rw(idle_freq);
define_one_global_rw(enabled);
define_one_global_rw(nwns_threshold_up);
define_one_global_rw(nwns_threshold_down);
define_one_global_rw(twts_threshold_up);
define_one_global_rw(twts_threshold_down);

static struct attribute *msm_mpdec_attributes[] = {
	&startdelay.attr,
	&delay.attr,
	&pause.attr,
	&scroff_single_core.attr,
	&idle_freq.attr,
	&enabled.attr,
	&nwns_threshold_up.attr,
	&nwns_threshold_down.attr,
	&twts_threshold_up.attr,
	&twts_threshold_down.attr,
	NULL
};


static struct attribute_group msm_mpdec_attr_group = {
	.attrs = msm_mpdec_attributes,
	.name = "conf",
};
/**************************** SYSFS END ****************************/

static int __init msm_mpdec(void)
{
	int cpu, rc, err = 0;

	for_each_possible_cpu(cpu) {
		mutex_init(&(per_cpu(msm_mpdec_cpudata, cpu).suspend_mutex));
		per_cpu(msm_mpdec_cpudata, cpu).device_suspended = false;
		per_cpu(msm_mpdec_cpudata, cpu).online = true;
	}

	INIT_DELAYED_WORK(&msm_mpdec_work, msm_mpdec_work_thread);
	if (state != MSM_MPDEC_DISABLED)
		schedule_delayed_work(&msm_mpdec_work, 0);

	register_early_suspend(&msm_mpdec_early_suspend_handler);

	msm_mpdec_kobject = kobject_create_and_add("msm_mpdecision", kernel_kobj);
	if (msm_mpdec_kobject) {
		rc = sysfs_create_group(msm_mpdec_kobject,
							&msm_mpdec_attr_group);
		if (rc) {
			pr_warn(MPDEC_TAG"sysfs: ERROR, could not create sysfs group");
		}
	} else
		pr_warn(MPDEC_TAG"sysfs: ERROR, could not create sysfs kobj");

	pr_info(MPDEC_TAG"%s init complete.", __func__);

	return err;
}

late_initcall(msm_mpdec);

>>>>>>> f44c95e... mach-msm: Add msm_mpdecision
