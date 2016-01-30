/*
 * General purpose implementation of a simple periodic countdown timer.
 *
 * Copyright (c) 2007 CodeSourcery.
 *
 * This code is licensed under the GNU LGPL.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "qemu/timer.h"
#include "hw/ptimer.h"
#include "qemu/host-utils.h"
#include "sysemu/replay.h"

struct ptimer_state
{
    uint8_t enabled; /* 0 = disabled, 1 = periodic, 2 = oneshot.  */
    uint64_t limit;
    uint64_t delta;
    uint32_t period_frac;
    int64_t period;
    int64_t last_event;
    int64_t next_event;
    uint8_t policy;
    QEMUBH *bh;
    QEMUTimer *timer;
};

/* Use a bottom-half routine to avoid reentrancy issues.  */
static void ptimer_trigger(ptimer_state *s)
{
    if (s->bh) {
        replay_bh_schedule_event(s->bh);
    }
}

static void ptimer_reload(ptimer_state *s)
{
    uint32_t period_frac = s->period_frac;
    uint64_t period = s->period;
    uint64_t delta = MAX(1, s->delta);

    if (s->delta == 0 && s->policy == UNIMPLEMENTED) {
        hw_error("ptimer: Running with counter=0 is unimplemented by " \
                 "this timer, fix it!\n");
    }

    if (period == 0) {
        hw_error("ptimer: Timer tries to run with period=0, behaviour is " \
                 "undefined, fix it!\n");
    }

    /*
     * Artificially limit timeout rate to something
     * achievable under QEMU.  Otherwise, QEMU spends all
     * its time generating timer interrupts, and there
     * is no forward progress.
     * About ten microseconds is the fastest that really works
     * on the current generation of host machines.
     */

    if (s->enabled == 1 && (delta * period < 10000) && !use_icount) {
        period = 10000 / delta;
        period_frac = 0;
    }

    s->last_event = s->next_event;
    s->next_event = s->last_event + delta * period;
    if (period_frac) {
        s->next_event += ((int64_t)period_frac * delta) >> 32;
    }
    timer_mod(s->timer, s->next_event);
}

static void ptimer_tick(void *opaque)
{
    ptimer_state *s = (ptimer_state *)opaque;

    s->delta = (s->enabled == 1) ? s->limit : 0;

    if (s->delta == 0) {
        s->enabled = 0;
    } else {
        ptimer_reload(s);
    }

    ptimer_trigger(s);
}

uint64_t ptimer_get_count(ptimer_state *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t counter;

    if (s->enabled && s->delta != 0 && now != s->last_event) {
        int64_t next = s->next_event;
        bool expired = (now - next >= 0);
        bool oneshot = (s->enabled == 2);

        /* Figure out the current counter value.  */
        if (expired && (oneshot || use_icount)) {
            /* Prevent timer underflowing if it should already have
               triggered.  */
            counter = 0;
        } else {
            uint64_t rem;
            uint64_t div;
            int clz1, clz2;
            int shift;
            uint32_t period_frac = s->period_frac;
            uint64_t period = s->period;

            if (!oneshot && (s->delta * period < 10000) && !use_icount) {
                period = 10000 / s->delta;
                period_frac = 0;
            }

            /* We need to divide time by period, where time is stored in
               rem (64-bit integer) and period is stored in period/period_frac
               (64.32 fixed point).
              
               Doing full precision division is hard, so scale values and
               do a 64-bit division.  The result should be rounded down,
               so that the rounding error never causes the timer to go
               backwards.
            */

            rem = expired ? now - next : next - now;
            div = period;

            clz1 = clz64(rem);
            clz2 = clz64(div);
            shift = clz1 < clz2 ? clz1 : clz2;

            rem <<= shift;
            div <<= shift;
            if (shift >= 32) {
                div |= ((uint64_t)period_frac << (shift - 32));
            } else {
                if (shift != 0)
                    div |= (period_frac >> (32 - shift));
                /* Look at remaining bits of period_frac and round div up if 
                   necessary.  */
                if ((uint32_t)(period_frac << shift))
                    div += 1;
            }
            counter = rem / div + (expired ? 0 : 1);

            if (expired && counter != 0) {
                /* Wrap around periodic counter.  */
                counter = s->limit - (counter - 1) % s->limit;
            }
        }
    } else {
        counter = s->delta;
    }
    return counter;
}

void ptimer_set_count(ptimer_state *s, uint64_t count)
{
    s->delta = count;
    if (s->enabled) {
        s->next_event = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_reload(s);
    }
}

void ptimer_run(ptimer_state *s, int oneshot)
{
    bool was_disabled = !s->enabled;

    s->enabled = oneshot ? 2 : 1;

    if (was_disabled) {
        s->next_event = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_reload(s);
    }
}

/* Pause a timer.  Note that this may cause it to "lose" time, even if it
   is immediately restarted.  */
void ptimer_stop(ptimer_state *s)
{
    if (!s->enabled)
        return;

    s->delta = ptimer_get_count(s);
    timer_del(s->timer);
    s->enabled = 0;
}

/* Set counter increment interval in nanoseconds.  */
void ptimer_set_period(ptimer_state *s, int64_t period)
{
    s->delta = ptimer_get_count(s);
    s->period = period;
    s->period_frac = 0;
    if (s->enabled) {
        s->next_event = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_reload(s);
    }
}

/* Set counter frequency in Hz.  */
void ptimer_set_freq(ptimer_state *s, uint32_t freq)
{
    g_assert(freq != 0 && freq <= 1000000000);
    s->delta = ptimer_get_count(s);
    s->period = 1000000000ll / freq;
    s->period_frac = (1000000000ll << 32) / freq;
    if (s->enabled) {
        s->next_event = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_reload(s);
    }
}

/* Set the initial countdown value.  If reload is nonzero then also set
   count = limit.  */
void ptimer_set_limit(ptimer_state *s, uint64_t limit, int reload)
{
    s->limit = limit;
    if (reload)
        s->delta = limit;
    if (s->enabled && reload) {
        s->next_event = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ptimer_reload(s);
    }
}

uint64_t ptimer_get_limit(ptimer_state *s)
{
    return s->limit;
}

void ptimer_set_policy(ptimer_state *s, enum ptimer_policy policy)
{
    s->policy = policy;
}

const VMStateDescription vmstate_ptimer = {
    .name = "ptimer",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(enabled, ptimer_state),
        VMSTATE_UINT64(limit, ptimer_state),
        VMSTATE_UINT64(delta, ptimer_state),
        VMSTATE_UINT32(period_frac, ptimer_state),
        VMSTATE_INT64(period, ptimer_state),
        VMSTATE_INT64(last_event, ptimer_state),
        VMSTATE_INT64(next_event, ptimer_state),
        VMSTATE_TIMER_PTR(timer, ptimer_state),
        VMSTATE_UINT8(policy, ptimer_state),
        VMSTATE_END_OF_LIST()
    }
};

ptimer_state *ptimer_init(QEMUBH *bh)
{
    ptimer_state *s;

    s = (ptimer_state *)g_malloc0(sizeof(ptimer_state));
    s->bh = bh;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ptimer_tick, s);
    return s;
}
