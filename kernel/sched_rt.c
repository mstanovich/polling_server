/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */

static inline ktime_t ss_get_now(struct task_struct *p)
{
	return hrtimer_cb_get_time(&p->ss_repl_timer);
}

static inline int ss_fg_prio(struct task_struct *p)
{
	return MAX_RT_PRIO-1 - p->rt_priority;
}

static inline int ss_bg_prio(struct task_struct *p)
{
	return p->sched_ss_low_priority;
}

static inline bool ss_curr_prio_fg(struct task_struct *p)
{
	if (p->normal_prio == ss_fg_prio(p))
		return true;

	return false;
}

static inline bool ss_curr_prio_bg(struct task_struct *p)
{
	if (p->normal_prio == ss_bg_prio(p))
		return true;

	return false;
}

/* Replenishment list functions */
static bool ss_rl_empty(struct task_struct *p)
{
	return (p->repl_head == -1);
}

static bool ss_rl_full(struct task_struct *p)
{
	/* > should not be possible, means we exceeded max_repl */
	return (p->repl_head+1 >= p->sched_ss_max_repl);
}

/**
 * @return: number of replenishments currently in use.
 */
static int ss_rl_size(struct task_struct *p)
{
	return (p->sched_ss_max_repl - (p->repl_head+1));
}

static struct sched_ss_repl ss_rl_pop(struct task_struct *p)
{
	BUG_ON(ss_rl_empty(p));

	return p->ss_repl_list[p->repl_head--];
}

/*
static void ss_rl_push(struct task_struct *p, struct sched_ss_repl repl)
{
	BUG_ON(ss_rl_full(p));

	p->ss_repl_list[++p->repl_head] = repl;
}

static void ss_rl_replace_front(struct task_struct *p, struct sched_ss_repl repl)
{
	p->ss_repl_list[p->repl_head] = repl;
}
*/

/**
 * Assumes there is one space/slot available in the replenishment list.
 */
static void ss_rl_add(struct task_struct *p, struct sched_ss_repl repl)
{
	int i;

	for (i=p->repl_head; i>=0; --i) {
		p->ss_repl_list[i+1] = p->ss_repl_list[i];
	}

	p->ss_repl_list[0] = repl;

	p->repl_head++;
}

static bool ss_valid_rl(struct task_struct *p)
{
	/*
	 * -1 (empty), but cannot ever be empty
	 * >= p->sched_ss_max_repl-1 (full)
	 */
	if (p->repl_head < 0 || p->repl_head >= p->sched_ss_max_repl) {
		return false;
	}

	/* TODO: validate capacity is not > sched_ss_init_budget
	 * iterate through all replenishments
	 * BUG_ON();
	 */
/*
	int i;
	for (i=p->repl_head; i>=0; --i) {
		if (ktime_cmp(now, p->ss_repl_list[i].time) < 0) {
			break;
		}
		capacity = ktime_add(capacity, p->ss_repl_list[i].amt);
	}
*/

	return true;
}

/**
 * Assumes p is scheduled using SCHED_SPORADIC policy.
 */
static inline ktime_t ss_capacity(struct task_struct *p, ktime_t now)
{
    BUG_ON(!ss_valid_rl(p));

    return ktime_sub(p->ss_repl_list[p->repl_head].amt, p->ss_usage);
}

static inline bool ss_out_of_budget(struct task_struct *p, ktime_t now)
{
	return ktime_cmp(ss_capacity(p, now), ns_to_ktime(0)) <= 0;
}

static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio);

static void ss_change_prio(struct rq *rq, struct task_struct *p,
	int new_prio);

static void ss_split_check(struct task_struct *p, ktime_t now)
{
	p->ss_usage = ns_to_ktime(0);
}

/**
 * Assumes rq->lock is held.
 */
static void ss_budget_check(struct rq *rq, struct task_struct *p, ktime_t now, bool blocked, bool running, bool set_repl_timer)
{
	assert_raw_spin_locked(&task_rq(p)->lock);

	if (ss_out_of_budget(p, now)) {
		ss_change_prio(rq, p, ss_bg_prio(p));
	}
}

/* forward the replenishment time in interval(polling) increments */
static int ss_fwd_repl_timer(struct task_struct *p, ktime_t now)
{
	int periods = 0;
	struct hrtimer *timer = &p->ss_repl_timer;
	ktime_t interval = p->sched_ss_repl_period;

	if (ktime_cmp(hrtimer_get_expires(timer), now) > 0) {
		/* timer already set to beginning of next period */
		return 0;
	}

	do {
		periods++;
		hrtimer_add_expires(timer, interval);
	} while(ktime_cmp(hrtimer_get_expires(timer), now) <= 0);

	return periods;
}

/**
 * Assumes p is ready to run when called.
 *
 * @return:
 * 	- p has capacity to run now.
 */
static bool ss_unblock_check(struct rq *rq, struct task_struct *p, ktime_t now, bool start_repl_timer, bool running)
{
		ss_fwd_repl_timer(p, now);

		/* activate the timer */
		hrtimer_restart(&p->ss_repl_timer);

		return false;
}


#ifdef CONFIG_RT_GROUP_SCHED

#define rt_entity_is_task(rt_se) (!(rt_se)->my_q)

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_SCHED_DEBUG
	WARN_ON_ONCE(!rt_entity_is_task(rt_se));
#endif
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return rt_rq->rq;
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	return rt_se->rt_rq;
}

#else /* CONFIG_RT_GROUP_SCHED */

#define rt_entity_is_task(rt_se) (1)

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);
	struct rq *rq = task_rq(p);

	return &rq->rt;
}

#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP

static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 */
	wmb();
	atomic_inc(&rq->rd->rto_count);
}

static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

static void update_rt_migration(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_nr_migratory && rt_rq->rt_nr_total > 1) {
		if (!rt_rq->overloaded) {
			rt_set_overload(rq_of_rt_rq(rt_rq));
			rt_rq->overloaded = 1;
		}
	} else if (rt_rq->overloaded) {
		rt_clear_overload(rq_of_rt_rq(rt_rq));
		rt_rq->overloaded = 0;
	}
}

static void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (!rt_entity_is_task(rt_se))
		return;

	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total++;
	if (rt_se->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory++;

	update_rt_migration(rt_rq);
}

static void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (!rt_entity_is_task(rt_se))
		return;

	rt_rq = &rq_of_rt_rq(rt_rq)->rt;

	rt_rq->rt_nr_total--;
	if (rt_se->nr_cpus_allowed > 1)
		rt_rq->rt_nr_migratory--;

	update_rt_migration(rt_rq);
}

static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->rt.pushable_tasks);
}

static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
}

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

#else

static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

#endif /* CONFIG_SMP */

static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return !list_empty(&rt_se->run_list);
}

#ifdef CONFIG_RT_GROUP_SCHED

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	if (!rt_rq->tg)
		return RUNTIME_INF;

	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(rt_rq->tg->rt_bandwidth.rt_period);
}

typedef struct task_group *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for (iter = list_entry_rcu(task_groups.next, typeof(*iter), list); \
	     (&iter->list != &task_groups) && \
	     (rt_rq = iter->rt_rq[cpu_of(rq)]); \
	     iter = list_entry_rcu(iter->list.next, typeof(*iter), list))

static inline void list_add_leaf_rt_rq(struct rt_rq *rt_rq)
{
	list_add_rcu(&rt_rq->leaf_rt_rq_list,
			&rq_of_rt_rq(rt_rq)->leaf_rt_rq_list);
}

static inline void list_del_leaf_rt_rq(struct rt_rq *rt_rq)
{
	list_del_rcu(&rt_rq->leaf_rt_rq_list);
}

#define for_each_leaf_rt_rq(rt_rq, rq) \
	list_for_each_entry_rcu(rt_rq, &rq->leaf_rt_rq_list, leaf_rt_rq_list)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = rt_se->parent)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->my_q;
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head);
static void dequeue_rt_entity(struct sched_rt_entity *rt_se);

static void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct task_struct *curr = rq_of_rt_rq(rt_rq)->curr;
	struct sched_rt_entity *rt_se;

	int cpu = cpu_of(rq_of_rt_rq(rt_rq));

	rt_se = rt_rq->tg->rt_se[cpu];

	if (rt_rq->rt_nr_running) {
		if (rt_se && !on_rt_rq(rt_se))
			enqueue_rt_entity(rt_se, false);
		if (rt_rq->highest_prio.curr < curr->prio)
			resched_task(curr);
	}
}

static void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	struct sched_rt_entity *rt_se;
	int cpu = cpu_of(rq_of_rt_rq(rt_rq));

	rt_se = rt_rq->tg->rt_se[cpu];

	if (rt_se && on_rt_rq(rt_se))
		dequeue_rt_entity(rt_se);
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled && !rt_rq->rt_nr_boosted;
}

static int rt_se_boosted(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = group_rt_rq(rt_se);
	struct task_struct *p;

	if (rt_rq)
		return !!rt_rq->rt_nr_boosted;

	p = rt_task_of(rt_se);
	return p->prio != p->normal_prio;
}

#ifdef CONFIG_SMP
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_rq(smp_processor_id())->rd->span;
}
#else
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}
#endif

static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return container_of(rt_b, struct task_group, rt_bandwidth)->rt_rq[cpu];
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &rt_rq->tg->rt_bandwidth;
}

#else /* !CONFIG_RT_GROUP_SCHED */

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(def_rt_bandwidth.rt_period);
}

typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

static inline void list_add_leaf_rt_rq(struct rt_rq *rt_rq)
{
}

static inline void list_del_leaf_rt_rq(struct rt_rq *rt_rq)
{
}

#define for_each_leaf_rt_rq(rt_rq, rq) \
	for (rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = NULL)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return NULL;
}

static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_nr_running)
		resched_task(rq_of_rt_rq(rt_rq)->curr);
}

static inline void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled;
}

static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}

static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return &cpu_rq(cpu)->rt;
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &def_rt_bandwidth;
}

#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP
/*
 * We ran out of runtime, see if we can borrow some from our neighbours.
 */
static int do_balance_runtime(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	int i, weight, more = 0;
	u64 rt_period;

	weight = cpumask_weight(rd->span);

	raw_spin_lock(&rt_b->rt_runtime_lock);
	rt_period = ktime_to_ns(rt_b->rt_period);
	for_each_cpu(i, rd->span) {
		struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
		s64 diff;

		if (iter == rt_rq)
			continue;

		raw_spin_lock(&iter->rt_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disalow stealing.
		 */
		if (iter->rt_runtime == RUNTIME_INF)
			goto next;

		/*
		 * From runqueues with spare time, take 1/n part of their
		 * spare time, but no more than our period.
		 */
		diff = iter->rt_runtime - iter->rt_time;
		if (diff > 0) {
			diff = div_u64((u64)diff, weight);
			if (rt_rq->rt_runtime + diff > rt_period)
				diff = rt_period - rt_rq->rt_runtime;
			iter->rt_runtime -= diff;
			rt_rq->rt_runtime += diff;
			more = 1;
			if (rt_rq->rt_runtime == rt_period) {
				raw_spin_unlock(&iter->rt_runtime_lock);
				break;
			}
		}
next:
		raw_spin_unlock(&iter->rt_runtime_lock);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);

	return more;
}

/*
 * Ensure this RQ takes back all the runtime it lend to its neighbours.
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
		s64 want;
		int i;

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		if (rt_rq->rt_runtime == RUNTIME_INF ||
				rt_rq->rt_runtime == rt_b->rt_runtime)
			goto balanced;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		want = rt_b->rt_runtime - rt_rq->rt_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			s64 diff;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == rt_rq || iter->rt_runtime == RUNTIME_INF)
				continue;

			raw_spin_lock(&iter->rt_runtime_lock);
			if (want > 0) {
				diff = min_t(s64, iter->rt_runtime, want);
				iter->rt_runtime -= diff;
				want -= diff;
			} else {
				iter->rt_runtime -= want;
				want -= want;
			}
			raw_spin_unlock(&iter->rt_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		BUG_ON(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		rt_rq->rt_runtime = RUNTIME_INF;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

static void disable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__disable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static void __enable_runtime(struct rq *rq)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_b->rt_runtime;
		rt_rq->rt_time = 0;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

static void enable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__enable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static int balance_runtime(struct rt_rq *rt_rq)
{
	int more = 0;

	if (rt_rq->rt_time > rt_rq->rt_runtime) {
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		more = do_balance_runtime(rt_rq);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
	}

	return more;
}
#else /* !CONFIG_SMP */
static inline int balance_runtime(struct rt_rq *rt_rq)
{
	return 0;
}
#endif /* CONFIG_SMP */

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	int i, idle = 1;
	const struct cpumask *span;

	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return 1;

	span = sched_rt_period_mask();
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct rt_rq *rt_rq = sched_rt_period_rt_rq(rt_b, i);
		struct rq *rq = rq_of_rt_rq(rt_rq);

		raw_spin_lock(&rq->lock);
		if (rt_rq->rt_time) {
			u64 runtime;

			raw_spin_lock(&rt_rq->rt_runtime_lock);
			if (rt_rq->rt_throttled)
				balance_runtime(rt_rq);
			runtime = rt_rq->rt_runtime;
			rt_rq->rt_time -= min(rt_rq->rt_time, overrun*runtime);
			if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
				rt_rq->rt_throttled = 0;
				enqueue = 1;

				/*
				 * Force a clock update if the CPU was idle,
				 * lest wakeup -> unthrottle time accumulate.
				 */
				if (rt_rq->rt_nr_running && rq->curr == rq->idle)
					rq->skip_clock_update = -1;
			}
			if (rt_rq->rt_time || rt_rq->rt_nr_running)
				idle = 0;
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		} else if (rt_rq->rt_nr_running) {
			idle = 0;
			if (!rt_rq_throttled(rt_rq))
				enqueue = 1;
		}

		if (enqueue)
			sched_rt_rq_enqueue(rt_rq);
		raw_spin_unlock(&rq->lock);
	}

	return idle;
}

static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_RT_GROUP_SCHED
	struct rt_rq *rt_rq = group_rt_rq(rt_se);

	if (rt_rq)
		return rt_rq->highest_prio.curr;
#endif

	return rt_task_of(rt_se)->prio;
}

static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = sched_rt_runtime(rt_rq);

	if (rt_rq->rt_throttled)
		return rt_rq_throttled(rt_rq);

	if (sched_rt_runtime(rt_rq) >= sched_rt_period(rt_rq))
		return 0;

	balance_runtime(rt_rq);
	runtime = sched_rt_runtime(rt_rq);
	if (runtime == RUNTIME_INF)
		return 0;

	if (rt_rq->rt_time > runtime) {
		rt_rq->rt_throttled = 1;
		if (rt_rq_throttled(rt_rq)) {
			sched_rt_rq_dequeue(rt_rq);
			return 1;
		}
	}

	return 0;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_rt_entity *rt_se = &curr->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 delta_exec;

	if (curr->sched_class != &rt_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max, max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq->clock_task;
	cpuacct_charge(curr, delta_exec);

	sched_rt_avg_update(rq, delta_exec);

	if (curr->policy == SCHED_SPORADIC && ss_curr_prio_fg(curr)) {
		/* ss usage is updated only if we are consuming fg priority
		 * time.  That is, the priority is rt_priority (fg
		 * priority) */
		curr->ss_usage = ktime_add_ns(curr->ss_usage, delta_exec);
	}

	if (!rt_bandwidth_enabled())
		return;

	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);

		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			if (sched_rt_runtime_exceeded(rt_rq))
				resched_task(curr);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		}
	}
}

#if defined CONFIG_SMP

static struct task_struct *pick_next_highest_task_rt(struct rq *rq, int cpu);

static inline int next_prio(struct rq *rq)
{
	struct task_struct *next = pick_next_highest_task_rt(rq, rq->cpu);

	if (next && rt_prio(next->prio))
		return next->prio;
	else
		return MAX_RT_PRIO;
}

static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (prio < prev_prio) {

		/*
		 * If the new task is higher in priority than anything on the
		 * run-queue, we know that the previous high becomes our
		 * next-highest.
		 */
		rt_rq->highest_prio.next = prev_prio;

		if (rq->online)
			cpupri_set(&rq->rd->cpupri, rq->cpu, prio);

	} else if (prio == rt_rq->highest_prio.curr)
		/*
		 * If the next task is equal in priority to the highest on
		 * the run-queue, then we implicitly know that the next highest
		 * task cannot be any lower than current
		 */
		rt_rq->highest_prio.next = prio;
	else if (prio < rt_rq->highest_prio.next)
		/*
		 * Otherwise, we need to recompute next-highest
		 */
		rt_rq->highest_prio.next = next_prio(rq);
}

static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (rt_rq->rt_nr_running && (prio <= rt_rq->highest_prio.next))
		rt_rq->highest_prio.next = next_prio(rq);

	if (rq->online && rt_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rt_rq->highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}
static inline
void dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (prio < prev_prio)
		rt_rq->highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	if (rt_rq->rt_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else
		rt_rq->highest_prio.curr = MAX_RT_PRIO;

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#else

static inline void inc_rt_prio(struct rt_rq *rt_rq, int prio) {}
static inline void dec_rt_prio(struct rt_rq *rt_rq, int prio) {}

#endif /* CONFIG_SMP || CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted++;

	if (rt_rq->tg)
		start_rt_bandwidth(&rt_rq->tg->rt_bandwidth);
}

static void
dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted--;

	WARN_ON(!rt_rq->rt_nr_running && rt_rq->rt_nr_boosted);
}

#else /* CONFIG_RT_GROUP_SCHED */

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	start_rt_bandwidth(&def_rt_bandwidth);
}

static inline
void dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq) {}

#endif /* CONFIG_RT_GROUP_SCHED */

static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running++;

	inc_rt_prio(rt_rq, prio);
	inc_rt_migration(rt_se, rt_rq);
	inc_rt_group(rt_se, rt_rq);
}

static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	WARN_ON(!rt_rq->rt_nr_running);
	rt_rq->rt_nr_running--;

	dec_rt_prio(rt_rq, rt_se_prio(rt_se));
	dec_rt_migration(rt_se, rt_rq);
	dec_rt_group(rt_se, rt_rq);
}

static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	/*
	 * Don't enqueue the group if its throttled, or when empty.
	 * The latter is a consequence of the former when a child group
	 * get throttled and the current group doesn't have any other
	 * active members.
	 */
	if (group_rq && (rt_rq_throttled(group_rq) || !group_rq->rt_nr_running))
		return;

	if (!rt_rq->rt_nr_running)
		list_add_leaf_rt_rq(rt_rq);

	if (head)
		list_add(&rt_se->run_list, queue);
	else
		list_add_tail(&rt_se->run_list, queue);
	__set_bit(rt_se_prio(rt_se), array->bitmap);

	inc_rt_tasks(rt_se, rt_rq);
}

static void __dequeue_rt_entity(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;

	list_del_init(&rt_se->run_list);
	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	dec_rt_tasks(rt_se, rt_rq);
	if (!rt_rq->rt_nr_running)
		list_del_leaf_rt_rq(rt_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
static void dequeue_rt_stack(struct sched_rt_entity *rt_se)
{
	struct sched_rt_entity *back = NULL;

	for_each_sched_rt_entity(rt_se) {
		rt_se->back = back;
		back = rt_se;
	}

	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if (on_rt_rq(rt_se))
			__dequeue_rt_entity(rt_se);
	}
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head)
{
	dequeue_rt_stack(rt_se);
	for_each_sched_rt_entity(rt_se)
		__enqueue_rt_entity(rt_se, head);
}

static void dequeue_rt_entity(struct sched_rt_entity *rt_se)
{
	dequeue_rt_stack(rt_se);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = group_rt_rq(rt_se);

		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, false);
	}
}

/**
 * Similar to operations in __setscheduler().
 * 
 * Change the priority of a task.
 *
 * @blocked:
 * 	- need to know if task is blocked, so we do not enqueue it and make it
 * 	  incorrectly runnable
 * 	- p->on_rq is a good indicator, but enqueue and dequeue only set these
 * 	  after, so be careful when using on_rq in enqueue and dequeue
 *
 * TODO: would like to allow option for bg prio/not running/etc.
 *
 * NOTE: will not remove task from rq
 */
static void ss_change_prio(struct rq *rq, struct task_struct *p,
	int new_prio)
{
	unsigned long flags;
	int oldprio = p->prio;
	bool on_rq = p->on_rq;

	if (p->normal_prio == new_prio) {
		//printk(KERN_ERR "trying to set prio to the same value\n");
		return;
	}

	if (p->normal_prio == new_prio) {
		//printk(KERN_ERR "trying to set prio to the same value\n");
		return;
	}

	if (on_rq) {
		dequeue_rt_stack(&p->rt);
	}
	
	p->normal_prio = new_prio;
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	p->prio = rt_mutex_getprio(p);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	/* 
	 * minimal dequeue/enqueue to set bit in priority array and change
	 * location in rt queue.
	 * Changing location in rt queue only should be done if we are currently
	 * enqueued.
	 */
	if (on_rq) {
		struct sched_rt_entity *rt_se = &p->rt;

		/* do not want to enqueue rt_se if it was not previously */
		/* NOTE: rt_se will be NULL after for_each */
		for_each_sched_rt_entity(rt_se) {
			__enqueue_rt_entity(rt_se, true);
		}
		/* 
		 * calls resched_task() if necessary.  Could possibly reschedule only
		 * if p is runnable.
		 */
		prio_changed_rt(rq, p, oldprio);
		resched_task(rq->curr);
	}

	/* OPTION: do not execute in background */
	if (on_rq && ss_curr_prio_bg(p)) {
		/* remove from rq */
		dequeue_rt_entity(&p->rt);
		/* 
		 * force reschedule if p is running, if p is not running, p cannot be
		 * picked since p is removed from rq above, prio_changed_rt() will not
		 * necessarily cause a reschedule since bg priority may be above other
		 * tasks' priorities, but here we really want to irrevocably remove p
		 * from the rq (TODO: may want to think about priority inheritance case)
		 */
		if (task_running(rq, p))
			resched_task(rq->curr);
	}
}

/**
 * Handle setting and canceling the exhaust timer.
 *
 * @running:
 * 	- TODO: probably should be named set_timer
 * 	- sets/cancels timer based if true/false
 */
static void ss_do_exh_timer(struct rq *rq, struct task_struct *p, ktime_t now, bool running)
{
	ktime_t budget = ss_capacity(p, now);

	if (!running) {
		/* p was previously running, but is no longer,
		 * no need for exh timer */
		WARN_ON_ONCE(hrtimer_try_to_cancel(&p->ss_exh_timer) == -1);

		return;
	}

	if (running) {
		ktime_t timer_exp = ktime_add(now, budget);

		if (ss_out_of_budget(p, now)) {
			printk(KERN_ERR "running task with no budget\n");
		}

		if (ktime_cmp(timer_exp, ss_get_now(p)) <= 0) {
			printk(KERN_ERR "setting exhaust timer to expire in the past\n");
		}

		if (ktime_cmp(timer_exp, hrtimer_get_expires(&p->ss_repl_timer)) > 0) {
			printk(KERN_ERR "expiration time greater than replenishment (overloaded?)\n");
		} else {
			if (!ss_curr_prio_bg(p)) {
				/* only set exhaust timer if it is < repl timer */
				/* TODO: make sure repl timer is set! */
				WARN_ON_ONCE(hrtimer_start(&p->ss_exh_timer,
					timer_exp, HRTIMER_MODE_ABS));
			}
		}

		return;
	}
}

/**
 * A context switch is about to occur.
 *
 * rq->lock is held.
 *
 * ss->usage should have been updated in put_prev_task
 */
static void cs_notify_rt(struct rq *rq, struct task_struct *prev,
              struct task_struct *next)
{
	/* arm exhaust timer to minimize overrun */
	if (next->policy == SCHED_SPORADIC) {
		ss_do_exh_timer(rq, next, ss_get_now(next), true);
	}

	if (prev->policy == SCHED_SPORADIC) {
		ktime_t now = ss_get_now(prev);

		/* TODO: !prev->on_rq or false */
		ss_budget_check(rq, prev, now, !prev->on_rq, false, true);
		ss_do_exh_timer(rq, prev, now, false);
	}
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se;

	if (p->policy == SCHED_SPORADIC) {
		bool running = task_running(rq, p);

		/* sets repl timer, exh timer set in cs_notify */
		ss_unblock_check(rq, p, ss_get_now(p), true, running);

		if (!ss_curr_prio_bg(p)) {
			printk(KERN_ERR "enqueued but not at bg prio, but should be\n");
		}
	}

	rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	enqueue_rt_entity(rt_se, flags & ENQUEUE_HEAD);

	if (!task_current(rq, p) && p->rt.nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);

}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se;

	/* 
	 * place here so p->on_rq is consistent, that is before we actually
	 * dequeue, so any functions called can use p->on_rq and it will be
	 * accurate.  p->on_rq will be set after this function.
	 */
	if (p->policy == SCHED_SPORADIC) {
		ktime_t now = ss_get_now(p);

		/*
		 * exh timer should have been active if we are running in fg
		 * Possibly we could have been dequeued but exhaust timer came before
		 * rq->lock obtained on dequeue path through kernel
		 */
		/* TODO: only warn if in fg, and we allow bg execution */
		WARN_ON_ONCE(!hrtimer_active(&p->ss_exh_timer));

 		/* can't use hrtimer_cancel here because we are holding rq->lock */
		WARN_ON_ONCE(hrtimer_try_to_cancel(&p->ss_exh_timer) == -1);

		/* replenishment timer should only be deactivated by a dequeue, so make
		 * sure it is active before we cancel it */
		WARN_ON_ONCE(!hrtimer_active(&p->ss_repl_timer));

		/* 
		 * TODO: if repl_timer is not canceled, task_struct could go away
		 * taking timer along with it (oops when timer is dereferenced at
		 * expiration).  If we can't cancel it here better make sure it is
		 * canceled before deallocation of task_struct
		 */
		WARN_ON_ONCE(hrtimer_try_to_cancel(&p->ss_repl_timer) == -1);

		ss_change_prio(rq, p, ss_bg_prio(p));

		/* expire budget */
		p->ss_usage = p->ss_repl_list[p->repl_head].amt;
	}

	rt_se = &p->rt;

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se);

	dequeue_pushable_task(rq, p);
}

/*
 * Put task to the end of the run list without the overhead of dequeue
 * followed by enqueue.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}

static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

static int
select_task_rq_rt(struct task_struct *p, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;
	int cpu;

	if (sd_flag != SD_BALANCE_WAKE)
		return smp_processor_id();

	cpu = task_cpu(p);
	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = ACCESS_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	if (curr && unlikely(rt_task(curr)) &&
	    (curr->rt.nr_cpus_allowed < 2 ||
	     curr->prio < p->prio) &&
	    (p->rt.nr_cpus_allowed > 1)) {
		int target = find_lowest_rq(p);

		if (target != -1)
			cpu = target;
	}
	rcu_read_unlock();

	return cpu;
}

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	if (rq->curr->rt.nr_cpus_allowed == 1)
		return;

	if (p->rt.nr_cpus_allowed != 1
	    && cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	if (!cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * There appears to be other cpus that can accept
	 * current and none to run 'p', so lets reschedule
	 * to try and push current away:
	 */
	requeue_task_rt(rq, p, 1);
	resched_task(rq->curr);
}

#endif /* CONFIG_SMP */

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_task(rq->curr);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

static enum hrtimer_restart ss_repl_cb(struct hrtimer *timer)
{
	struct task_struct *p;
	struct rq *rq;
	int periods_passed;
	ktime_t now;
	bool running;

	//printk(KERN_ERR "%s\n", __func__);

	p = container_of(timer, struct task_struct, ss_repl_timer);
	rq = task_rq(p);
	
	raw_spin_lock(&rq->lock);

	update_rq_clock(rq);
	update_curr_rt(rq);

	running = task_running(rq, p);

	/* exh timer may be active if task was preempted for a long time */
	if (ktime_cmp(p->ss_usage, p->sched_ss_init_budget) > 0) {
		ktime_t budget = ktime_sub(p->sched_ss_init_budget, p->ss_usage);

		/* -5000 is just a fudge factor */
		if (budget.tv64 < -5000) {
			printk(KERN_ERR "budget overrun: %lld\n", (u64)budget.tv64);
		}
		WARN_ON(hrtimer_active(&p->ss_exh_timer));
	}

	/* exh timer may be active if task was preempted for a long time */
	if (hrtimer_try_to_cancel(&p->ss_exh_timer) != 0) {
		printk(KERN_ERR "exh timer active in ss_repl_cb(), probably overloaded (full budget not received)\n");
	}

	/* replenishment arrived, set usage to zero */
	p->ss_usage = ns_to_ktime(0);

	periods_passed = ss_fwd_repl_timer(p, hrtimer_get_expires(timer));

	/* handles skipped periods */
	/* now is relative to start of previous period, prevents drift */
	now = ktime_sub(hrtimer_get_expires(timer), p->sched_ss_repl_period);

	if (ktime_cmp(now, ns_to_ktime(0)) == -1)
		printk(KERN_ERR "SCHED_SPORADIC: negative now\n");

	if (periods_passed != 1)
		printk(KERN_ERR "SCHED_SPORADIC: replenishment timer skipped a period\n");

	/* even if taken of rt runqueue, on_rq will be 1 and ss_change_prio will
	 * return p to the rt runqueue */
	/* exhaust timer will be set when task is context switched to run */
	ss_change_prio(rq, p, ss_fg_prio(p));

	/* 
	 * HOWEVER, if we are already running, we will not be context switched
	 * in so set exhaust timer
	 */

	if (running)
		ss_do_exh_timer(rq, p, now, running);
	
	raw_spin_unlock(&rq->lock);

	/* TODO: we may have tried to cancel timer elsewhere, but failed since
	 * timer cb was running.  Maybe check if on rq? */

	return HRTIMER_RESTART;
}

static enum hrtimer_restart ss_exh_cb(struct hrtimer *timer)
{
	struct task_struct *p;
	struct rq *rq;
    ktime_t now;

	p = container_of(timer, struct task_struct, ss_exh_timer);
	rq = task_rq(p);
	
	raw_spin_lock(&rq->lock);

	now = ss_get_now(p);

	update_rq_clock(rq);
	update_curr_rt(rq);

	/* if p has budget, exh timer should not expire */
	if (!ss_out_of_budget(p, now)) {
		ktime_t budget = ktime_sub(p->sched_ss_init_budget, p->ss_usage);

		if (budget.tv64 < -3000) {
			printk(KERN_ERR "exh timer expired with budget remaining: %lld\n", (u64)budget.tv64);
		}
	}

	ss_change_prio(rq, p, ss_bg_prio(p));

	raw_spin_unlock(&rq->lock);

	return HRTIMER_NORESTART;
}

static struct sched_rt_entity *pick_next_rt_entity(struct rq *rq,
						   struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct task_struct *p;
	struct rt_rq *rt_rq;

	rt_rq = &rq->rt;

	if (unlikely(!rt_rq->rt_nr_running))
		return NULL;

	if (rt_rq_throttled(rt_rq))
		return NULL;

	do {
		rt_se = pick_next_rt_entity(rq, rt_rq);
		BUG_ON(!rt_se);
		rt_rq = group_rt_rq(rt_se);
	} while (rt_rq);

	p = rt_task_of(rt_se);
	p->se.exec_start = rq->clock_task;

	return p;
}

static struct task_struct *pick_next_task_rt(struct rq *rq)
{
	struct task_struct *p = _pick_next_task_rt(rq);

	/* The running task is never eligible for pushing */
	if (p)
		dequeue_pushable_task(rq, p);

#ifdef CONFIG_SMP
	/*
	 * We detect this state here so that we can avoid taking the RQ
	 * lock again later if there is no need to push
	 */
	rq->post_schedule = has_pushable_tasks(rq);
#endif

/* Careful, p may be NULL meaning no tasks to execute */
/*
	if (p && p->policy == SCHED_SPORADIC) {
		printk(KERN_ERR "picked SCHED_SPORADIC\n");
	}
*/

	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	update_curr_rt(rq);
	p->se.exec_start = 0;

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	if (on_rt_rq(&p->rt) && p->rt.nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

static void deactivate_task(struct rq *rq, struct task_struct *p, int sleep);

static int pick_rt_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    (cpu < 0 || cpumask_test_cpu(cpu, &p->cpus_allowed)) &&
	    (p->rt.nr_cpus_allowed > 1))
		return 1;
	return 0;
}

/* Return the second highest RT task, NULL otherwise */
static struct task_struct *pick_next_highest_task_rt(struct rq *rq, int cpu)
{
	struct task_struct *next = NULL;
	struct sched_rt_entity *rt_se;
	struct rt_prio_array *array;
	struct rt_rq *rt_rq;
	int idx;

	for_each_leaf_rt_rq(rt_rq, rq) {
		array = &rt_rq->active;
		idx = sched_find_first_bit(array->bitmap);
next_idx:
		if (idx >= MAX_RT_PRIO)
			continue;
		if (next && next->prio < idx)
			continue;
		list_for_each_entry(rt_se, array->queue + idx, run_list) {
			struct task_struct *p;

			if (!rt_entity_is_task(rt_se))
				continue;

			p = rt_task_of(rt_se);
			if (pick_rt_task(rq, p, cpu)) {
				next = p;
				break;
			}
		}
		if (!next) {
			idx = find_next_bit(array->bitmap, MAX_RT_PRIO, idx+1);
			goto next_idx;
		}
	}

	return next;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = __get_cpu_var(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->rt.nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of cpus representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last cpu that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which cpu is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;
	return -1;
}

/* Will lock the rq it finds */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu,
						       &task->cpus_allowed) ||
				     task_running(rq, task) ||
				     !task->on_rq)) {

				raw_spin_unlock(&lowest_rq->lock);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	p = plist_first_entry(&rq->rt.pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->rt.nr_cpus_allowed <= 1);

	BUG_ON(!p->on_rq);
	BUG_ON(!rt_task(p));

	return p;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
static int push_rt_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;

	if (!rq->rt.overloaded)
		return 0;

	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_task(rq->curr);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		task = pick_next_pushable_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * If we get here, the task hasn't moved at all, but
			 * it has failed to push.  We will not try again,
			 * since the other cpus will pull from us when they
			 * are ready.
			 */
			dequeue_pushable_task(rq, next_task);
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);

	resched_task(lowest_rq->curr);

	double_unlock_balance(rq, lowest_rq);

out:
	put_task_struct(next_task);

	return 1;
}

static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq))
		;
}

static int pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;

	if (likely(!rt_overloaded(this_rq)))
		return 0;

	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		if (src_rq->rt.highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * Are there still pullable RT tasks?
		 */
		if (src_rq->rt.rt_nr_running <= 1)
			goto skip;

		p = pick_next_highest_task_rt(src_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->on_rq);

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its cpu.
			 * This is just that p is wakeing up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

			ret = 1;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	return ret;
}

static void pre_schedule_rt(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	if (unlikely(rt_task(prev)) && rq->rt.highest_prio.curr > prev->prio)
		pull_rt_task(rq);
}

static void post_schedule_rt(struct rq *rq)
{
	push_rt_tasks(rq);
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    has_pushable_tasks(rq) &&
	    p->rt.nr_cpus_allowed > 1 &&
	    rt_task(rq->curr) &&
	    (rq->curr->rt.nr_cpus_allowed < 2 ||
	     rq->curr->prio < p->prio))
		push_rt_tasks(rq);
}

static void set_cpus_allowed_rt(struct task_struct *p,
				const struct cpumask *new_mask)
{
	int weight = cpumask_weight(new_mask);

	BUG_ON(!rt_task(p));

	/*
	 * Update the migration status of the RQ if we have an RT task
	 * which is running AND changing its weight value.
	 */
	if (p->on_rq && (weight != p->rt.nr_cpus_allowed)) {
		struct rq *rq = task_rq(p);

		if (!task_current(rq, p)) {
			/*
			 * Make sure we dequeue this task from the pushable list
			 * before going further.  It will either remain off of
			 * the list because we are no longer pushable, or it
			 * will be requeued.
			 */
			if (p->rt.nr_cpus_allowed > 1)
				dequeue_pushable_task(rq, p);

			/*
			 * Requeue if our weight is changing and still > 1
			 */
			if (weight > 1)
				enqueue_pushable_task(rq, p);

		}

		if ((p->rt.nr_cpus_allowed <= 1) && (weight > 1)) {
			rq->rt.rt_nr_migratory++;
		} else if ((p->rt.nr_cpus_allowed > 1) && (weight <= 1)) {
			BUG_ON(!rq->rt.rt_nr_migratory);
			rq->rt.rt_nr_migratory--;
		}

		update_rt_migration(&rq->rt);
	}

	cpumask_copy(&p->cpus_allowed, new_mask);
	p->rt.nr_cpus_allowed = weight;
}

/* Assumes rq->lock is held */
static void rq_online_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	__disable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	if (p->on_rq && !rq->rt.rt_nr_running)
		pull_rt_task(rq);
}

static inline void init_sched_rt_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
}
#endif /* CONFIG_SMP */

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	int check_resched = 1;

	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	if (p->on_rq && rq->curr != p) {
#ifdef CONFIG_SMP
		if (rq->rt.overloaded && push_rt_task(rq) &&
		    /* Don't resched if we changed runqueues */
		    rq != task_rq(p))
			check_resched = 0;
#endif /* CONFIG_SMP */
		if (check_resched && p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->on_rq)
		return;

	if (rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			pull_rt_task(rq);
		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule. Note, the above pull_rt_task
		 * can release the rq lock and p could migrate.
		 * Only reschedule if p is still on the same runqueue.
		 */
		if (p->prio > rq->rt.highest_prio.curr && rq->curr == p)
			resched_task(p);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_task(p);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		p->rt.timeout++;
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}

static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_rt(rq);

	watchdog(rq, p);

	/*
	 * RR tasks need a special form of timeslice management.
	 * FIFO tasks have no timeslices.
	 */
	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = DEF_TIMESLICE;

	/*
	 * Requeue to the end of queue if we are not the only element
	 * on the queue:
	 */
	if (p->rt.run_list.prev != p->rt.run_list.next) {
		requeue_task_rt(rq, p, 0);
		set_tsk_need_resched(p);
	}
}

static void set_curr_task_rt(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock_task;

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rq, p);
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return DEF_TIMESLICE;
	else
		return 0;
}

static const struct sched_class rt_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	.check_preempt_curr	= check_preempt_curr_rt,

	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_rt,

	.set_cpus_allowed       = set_cpus_allowed_rt,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.pre_schedule		= pre_schedule_rt,
	.post_schedule		= post_schedule_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
#endif

	.set_curr_task          = set_curr_task_rt,
	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,
};

#ifdef CONFIG_SCHED_DEBUG
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);

static void print_rt_stats(struct seq_file *m, int cpu)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	rcu_read_lock();
	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
		print_rt_rq(m, cpu, rt_rq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */

