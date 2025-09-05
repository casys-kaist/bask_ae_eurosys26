#ifndef _TIME_UTIL_H_
#define _TIME_UTIL_H_

#include <linux/ktime.h>

// #define get_tid() current->pid
#define get_tid() 1UL

struct event_timer {
	unsigned long   count;
	unsigned long 	time_sum;
	struct timespec64 start_time;
	struct timespec64 end_time;
	unsigned long 	max;
	unsigned long 	min;
	int used;   // for validation.
	unsigned long   aux_data;
	unsigned long   staged_duration;
};

/* Return nanoseconds. */
static inline unsigned long get_duration(struct timespec64 *time_start, struct timespec64 *time_end)
{
	long start_sec, end_sec;
	start_sec = time_start->tv_sec * 1000000000UL + time_start->tv_nsec;
	//        double end_sec = (double)(time_end->tv_sec * 1000000000.0 + (double)time_end->tv_nsec) / 1000000000.0;
	end_sec = time_end->tv_sec * 1000000000UL + time_end->tv_nsec;
	return end_sec - start_sec;
}

/* Return nanoseconds. */
static inline unsigned long get_time(struct timespec64 *time)
{
       return time->tv_sec * 1000000000UL + time->tv_nsec;
}

#define EVENT_TIMER(event) extern struct event_timer event;
#define EVENT_TIMER_EXPORT_SYMBOL(evt) \
	struct event_timer evt; \
	EXPORT_SYMBOL(evt);

#define RESET_TIMER(event) \
    do { \
	    event = ((struct event_timer) {0,}); \
	    event.min = 999999UL; \
    } while (0)

#define START_TIMER(event) \
    do { \
	    if ((event).used) { \
		printk(KERN_INFO "START_TIMER: timer already used. [tid:%lu %s():%d]\n", \
			get_tid(), __func__, __LINE__); \
	    } \
	    ktime_get_real_ts64(&(event).start_time); \
	    (event).used = 1; \
    } while (0)

#define END_TIMER(event) \
    do { \
	    unsigned long dur; \
	    if (!(event).used) { \
		printk(KERN_INFO "END_TIMER: timer not started. [%s():%d]\n", \
			__func__, __LINE__); \
	    } \
	    ktime_get_real_ts64(&(event).end_time); \
	    dur = get_duration(&(event).start_time, &(event).end_time); \
	    (event).time_sum += dur; \
	    (event).count++; \
	    (event).used = 0; \
	    if ((event).max < dur) { (event).max = dur; } \
	    if ((event).min > dur) { (event).min = dur; } \
    } while (0)

#define ACCUM_AUX(event, aux) \
	do { \
	    (event).aux_data += aux; \
	} while (0)

#define STAGE_TIMER(event) \
	do { \
		unsigned long dur; \
		if (!(event).used) { \
		printk(KERN_INFO "END_TIMER: timer not started. [%s():%d]\n", \
			__func__, __LINE__); \
		} \
		ktime_get_real_ts64(&(event).end_time); \
		dur = get_duration(&(event).start_time, &(event).end_time); \
		(event).staged_duration = dur; \
	} while (0)

#define DISCARD_TIMER(event) \
	do { \
		(event).staged_duration = 0; \
		(event).used = 0; \
	} while (0)

#define COMMIT_TIMER(event) \
	do { \
		(event).time_sum += (event).staged_duration; \
		(event).count++; \
		(event).used = 0; \
		if ((event).max < (event).staged_duration) { (event).max = (event).staged_duration; } \
		if ((event).min > (event).staged_duration) { (event).min = (event).staged_duration; } \
		(event).staged_duration = 0; \
	} while (0)

#define PRINT_HDR() \
    do { \
	printk(KERN_INFO ",%-20s, %12s, %10s, %12s, %12s, %12s\n", "evt_name", "nsec", "count", "min", "max", "aux"); \
    } while (0)

#define PRINT_FMT ",%-20s, %12lu, %12lu, %12lu, %12lu, %12lu\n"
#define PRINT_TIMER(event, desc) \
    printk(KERN_INFO PRINT_FMT, \
	    desc, \
	    event.time_sum, \
	    event.count, \
	    event.min, \
	    event.max, \
		event.aux_data)

#define PRINT_START_TIME(event, desc) \
    printk(KERN_INFO "%-20s %lu %12lu\n", \
	    desc, \
	    get_tid(), \
	    get_time(&(event).start_time))

#define PRINT_END_TIME(event, desc) \
    printk(KERN_INFO "%-20s %lu %12lu\n", \
	    desc, \
	    get_tid(), \
	    get_time(&(event).end_time))

#define PRINT_TIME_STAMP(t) \
	ktime_get_real_ts64(&(t).start_time); \
	printk (KERN_INFO "%12lu\n", get_time(&(t)))

//////// Breakdown timers.
//// Common

#define PRINT_TIME 1

#ifdef PRINT_TIME
#define DEBUG_TIME_START(evt) START_TIMER(evt)
#define DEBUG_TIME_END(evt) END_TIMER(evt)
#define DEBUG_EVENT_TIMER(evt) EVENT_TIMER(evt)
#define DEBUG_EVENT_TIMER_EXPORT_SYMBOL(evt) EVENT_TIMER_EXPORT_SYMBOL(evt)
#define DEBUG_TIME_STAGE(evt) STAGE_TIMER(evt)
#define DEBUG_TIME_COMMIT(evt) COMMIT_TIMER(evt)
#define DEBUG_TIME_DISCARD(evt) DISCARD_TIMER(evt)
#define DEBUG_TIME_ACCUM_AUX(evt, aux) ACCUM_AUX(evt, aux)
#define DEBUG_RESET_TIMER(evt) RESET_TIMER(evt)

#else /* PRINT_TIME */
#define DEBUG_TIME_START(...)
#define DEBUG_TIME_END(...)
#define DEBUG_EVENT_TIMER(...)
#define DEBUG_EVENT_TIMER_EXPORT_SYMBOL(...)
#define DEBUG_TIME_STAGE(...)
#define DEBUG_TIME_COMMIT(...)
#define DEBUG_TIME_DISCARD(...)
#define DEBUG_TIME_ACCUM_AUX(...)
#define DEBUG_RESET_TIMER(...)
#endif

#endif