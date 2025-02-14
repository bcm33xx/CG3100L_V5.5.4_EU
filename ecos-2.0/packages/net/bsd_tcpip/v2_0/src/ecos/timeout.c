//==========================================================================
//
//      src/ecos/timeout.c
//
//==========================================================================
//####BSDCOPYRIGHTBEGIN####
//
// -------------------------------------------
//
// Portions of this software may have been derived from OpenBSD, 
// FreeBSD or other sources, and are covered by the appropriate
// copyright disclaimers included herein.
//
// Portions created by Red Hat are
// Copyright (C) 2002 Red Hat, Inc. All Rights Reserved.
//
// -------------------------------------------
//
//####BSDCOPYRIGHTEND####
//==========================================================================

//==========================================================================
//
//        lib/timeout.c
//
//        timeout support
//
//==========================================================================
//####BSDCOPYRIGHTBEGIN####
//
// -------------------------------------------
//
// Portions of this software may have been derived from OpenBSD or other sources,
// and are covered by the appropriate copyright disclaimers included herein.
//
// -------------------------------------------
//
//####BSDCOPYRIGHTEND####
//==========================================================================
//#####DESCRIPTIONBEGIN####
//
// Author(s):     gthomas, hmt
// Contributors:  gthomas, hmt
// Date:          1999-02-05
// Description:   Simple timeout functions
//####DESCRIPTIONEND####

#include <sys/param.h>
#include <pkgconf/net.h>
#include <cyg/kernel/kapi.h>
#include <cyg/infra/cyg_ass.h>

// Timeout support

void alarm_timeout_init(void);

#ifndef NTIMEOUTS
#define NTIMEOUTS 8
#endif
static timeout_entry _timeouts[NTIMEOUTS];
static timeout_entry *timeouts = (timeout_entry *)NULL;
static cyg_handle_t timeout_alarm_handle;
static cyg_alarm timeout_alarm;
static cyg_int32 last_delta;
static cyg_tick_count_t last_set_time;

#define STACK_SIZE CYGNUM_HAL_STACK_SIZE_TYPICAL
static char alarm_stack[STACK_SIZE];
static cyg_thread alarm_thread_data;
static cyg_handle_t alarm_thread_handle;

static cyg_flag_t alarm_flag;  

// ------------------------------------------------------------------------
// This routine exists so that this module can synchronize:
extern cyg_uint32 cyg_splinternal(void);

#ifdef TIMEOUT_DEBUG
static void
_show_timeouts(void)
{
    timeout_entry *f;
    for (f = timeouts;  f;  f = f->next) {
        diag_printf("%p: delta: %d, fun: %p, param: %p\n", f, f->delta, f->fun, f->arg);
    }
}
#endif // TIMEOUT_DEBUG

// ------------------------------------------------------------------------
// CALLBACK FUNCTION
// Called from the thread, this runs the alarm callbacks.
// Locking is already in place when this is called.
static void
do_timeout(void)
{
    cyg_int32 min_delta;
    timeout_entry *e, *e_next;

    CYG_ASSERT( 0 < last_delta, "last_delta underflow" );

    min_delta = last_delta; // local copy
    last_delta = -1; // flag recursive call underway

    e = timeouts;
    while (e) {
        e_next = e->next;  // Because this can change during processing
        if (e->delta) {
#ifdef TIMEOUT_DEBUG
                if ( !(e->delta >= min_delta)) {
                    diag_printf("Bad delta in timeout: %p, delta: %d, min: %d, last: %ld\n", e, e->delta, min_delta, last_set_time);
                    _show_timeouts();
                }                
#endif
// Note: this _can_ happen if timeouts are scheduled before the clock starts!
//            CYG_ASSERT( e->delta >= min_delta, "e->delta underflow" );
            e->delta -= min_delta;
            if (e->delta <= 0) { // Defensive
                // Time for this item to 'fire'
                timeout_fun *fun = e->fun;
                void *arg = e->arg;
                // Call it *after* cleansing the record
//                diag_printf("%s(%p, %p, %p)\n", __FUNCTION__, e, e->fun, e->arg);
                e->flags &= ~CALLOUT_PENDING;
                e->delta = 0;
                if (e->next) {
                    e->next->prev = e->prev;
                }
                if (e->prev) {
                    e->prev->next = e->next;
                } else {
                    timeouts = e->next;
                }
                (*fun)(arg);
            }
        }
        e = e_next;
    }

    // Now scan for a new timeout *after* running all the callbacks
    // (because they can add timeouts themselves)
    min_delta = 0x7FFFFFFF;  // Maxint
    for (e = timeouts;  e;  e = e->next )
        if (e->delta)
            if (e->delta < min_delta)
                min_delta = e->delta;

    CYG_ASSERT( 0 < min_delta, "min_delta underflow" );

    if (min_delta != 0x7FFFFFFF) {
        // Still something to do, schedule it
        last_set_time = cyg_current_time();
        cyg_alarm_initialize(timeout_alarm_handle, last_set_time+min_delta, 0);
        last_delta = min_delta;
    } else {
        last_delta = 0; // flag no activity
    }
#ifdef TIMEOUT_DEBUG
    diag_printf("Timeout list after %s\n", __FUNCTION__);
    _show_timeouts();
#endif
}

// ------------------------------------------------------------------------
// ALARM EVENT FUNCTION
// This is the DSR for the alarm firing:
static void
do_alarm(cyg_handle_t alarm, cyg_addrword_t data)
{
    cyg_flag_setbits( &alarm_flag, 1 ); 
}

void ecos_synch_eth_drv_dsr(void)
{
    cyg_flag_setbits( &alarm_flag, 2 ); 
}

// ------------------------------------------------------------------------
// HANDLER THREAD ENTRY ROUTINE
// This waits on the DSR to tell it to run:
static void
alarm_thread(cyg_addrword_t param)
{
    // This is from the logical ethernet dev; it calls those delivery
    // functions who need attention.
    extern void eth_drv_run_deliveries( void );

    // This is from the logical ethernet dev; it tickles somehow
    // all ethernet devices in case one is wedged.
    extern void eth_drv_tickle_devices( void );

    while ( 1 ) {
        int spl;
        int x;
#ifdef CYGPKG_NET_FAST_THREAD_TICKLE_DEVS
        cyg_tick_count_t later = cyg_current_time();
        later += CYGNUM_NET_FAST_THREAD_TICKLE_DEVS_DELAY;
        x = cyg_flag_timed_wait(
            &alarm_flag,
            -1,
            CYG_FLAG_WAITMODE_OR | CYG_FLAG_WAITMODE_CLR,
            later );
#else
        x = cyg_flag_wait(
            &alarm_flag,
            -1,
            CYG_FLAG_WAITMODE_OR | CYG_FLAG_WAITMODE_CLR );

        CYG_ASSERT( 3 & x, "Lost my bits" );
#endif // CYGPKG_NET_FAST_THREAD_TICKLE_DEVS
        CYG_ASSERT( !((~3) & x), "Extra bits" );

        spl = cyg_splinternal();

        CYG_ASSERT( 0 == spl, "spl nonzero" );

        if ( 2 & x )
            eth_drv_run_deliveries();
#ifdef CYGPKG_NET_FAST_THREAD_TICKLE_DEVS
        // This is in the else clause for "do we deliver" because the
        // network stack might have continuous timing events anyway - so
        // the timeout would not occur, x would be 1 every time.
        else // Tickle the devices...
            eth_drv_tickle_devices();
#endif // CYGPKG_NET_FAST_THREAD_TICKLE_DEVS

        if ( 1 & x )
            do_timeout();

        cyg_splx(spl);
    }
}

// ------------------------------------------------------------------------
// INITIALIZATION FUNCTION
void
cyg_alarm_timeout_init( void )
{
    // Init the alarm object, attached to the real time clock
    cyg_handle_t h;
    cyg_clock_to_counter(cyg_real_time_clock(), &h);
    cyg_alarm_create(h, do_alarm, 0, &timeout_alarm_handle, &timeout_alarm);
    // Init the flag of waking up
    cyg_flag_init( &alarm_flag );
    // Create alarm background thread to run the callbacks
    cyg_thread_create(
        CYGPKG_NET_FAST_THREAD_PRIORITY, // Priority
        alarm_thread,                   // entry
        0,                              // entry parameter
        "Network alarm support",        // Name
        &alarm_stack[0],                // Stack
        STACK_SIZE,                     // Size
        &alarm_thread_handle,           // Handle
        &alarm_thread_data              // Thread data structure
        );
    cyg_thread_resume(alarm_thread_handle);    // Start it
}

// ------------------------------------------------------------------------
// EXPORTED API: SET A TIMEOUT
// This can be called from anywhere, including recursively from the timeout
// functions themselves.
cyg_uint32
timeout(timeout_fun *fun, void *arg, cyg_int32 delta)
{
    int i;
    timeout_entry *e;
    cyg_uint32 stamp;

    // this needs to be atomic - recursive calls from the alarm
    // handler thread itself are allowed:
    int spl = cyg_splinternal();

    stamp = 0;  // Assume no slots available
    for (e = _timeouts, i = 0;  i < NTIMEOUTS;  i++, e++) {
        if ((e->flags & CALLOUT_PENDING) == 0) {
            // Free entry
            callout_init(e);
            e->flags = CALLOUT_LOCAL;
            callout_reset(e, delta, fun, arg);
            stamp = (cyg_uint32)e;
            break;
        }
    }
    cyg_splx(spl);
    return stamp;
}

// ------------------------------------------------------------------------
// EXPORTED API: CANCEL A TIMEOUT
// This can be called from anywhere, including recursively from the timeout
// functions themselves.
void
untimeout(timeout_fun *fun, void * arg)
{
    int i;
    timeout_entry *e;
    int spl = cyg_splinternal();

    for (e = _timeouts, i = 0; i < NTIMEOUTS; i++, e++) {
        if (e->delta && (e->fun == fun) && (e->arg == arg)) {
            callout_stop(e);
            break;
        }
    }
    cyg_splx(spl);
}

void 
callout_init(struct callout *c) 
{
    bzero(c, sizeof(*c));
}

void 
callout_reset(struct callout *c, int delta, timeout_fun *f, void *p) 
{
    int spl = cyg_splinternal();

    CYG_ASSERT( 0 < delta, "delta is right now, or even sooner!" );

    // Renormalize delta wrt the existing set alarm, if there is one
    if (last_delta > 0) {
#ifdef TIMEOUT_DEBUG
        int _delta = delta;    
        int _time = cyg_current_time();
#endif // TIMEOUT_DEBUG
        // There is an active alarm
        if (last_set_time != 0) {
            // Adjust the delta to be absolute, relative to the alarm
            delta += (cyg_int32)(cyg_current_time() - last_set_time);
        } else {
            // We don't know exactly when the alarm will fire, so just
            // schedule this event for the first time, or sometime after
            ;  // Leaving the value alone won't be "too wrong"
        }
#ifdef TIMEOUT_DEBUG
        diag_printf("delta changed from %d to %d, now: %d, then: %d, last_delta: %d\n", 
                    _delta, delta, _time, (int)last_set_time, last_delta);
        _show_timeouts();
#endif
    }
    // So recorded_delta is set to either:
    // alarm is active:   delta + NOW - THEN
    // alarm is inactive: delta

    // Add this callout/timeout to the list of things to do
    if (c->flags & CALLOUT_PENDING) {
        callout_stop(c);
    }
    c->prev = (timeout_entry *)NULL;    
    c->next = timeouts;
    if (c->next != (timeout_entry *)NULL) {
        c->next->prev = c;
    }
    timeouts = c;
    c->flags |= CALLOUT_PENDING | CALLOUT_ACTIVE;
    c->fun = f;
    c->arg = p;
    c->delta = delta;

#ifdef TIMEOUT_DEBUG
    diag_printf("%s(%p, %d, %p, %p)\n", __FUNCTION__, c, delta, f, p);
    _show_timeouts();
#endif

    if ((0 == last_delta ||      // alarm was inactive  OR
         delta < last_delta) ) { // alarm was active but later than we need

        // (if last_delta is -1, this call is recursive from the handler so
        //  also do nothing in that case)

        // Here, we know the new item added is sooner than that which was
        // most recently set, if any, so we can just go and set it up.
        if ( 0 == last_delta )
            last_set_time = cyg_current_time();
        
        // So we use, to set the alarm either:
        // alarm is active:   (delta + NOW - THEN) + THEN
        // alarm is inactive:  delta + NOW
        // and in either case it is true that
        //  (recorded_delta + last_set_time) == (delta + NOW)
        cyg_alarm_initialize(timeout_alarm_handle, last_set_time+delta, 0);
#ifdef TIMEOUT_DEBUG
        if ((int)last_set_time == 0) {
            diag_printf("delta: %d, time: %ld, last_delta: %d\n", delta, last_set_time, last_delta);
        }
#endif
        last_delta = delta;
    }
    // Otherwise, the alarm is active, AND it is set to fire sooner than we
    // require, so when it does, that will sort out calling the item we
    // just added.

#ifdef CYGPKG_INFRA_DEBUG
    // Do some more checking akin to that in the alarm handler:
    if ( last_delta != -1 ) { // not a recursive call
        cyg_tick_count_t now = cyg_current_time();
        timeout_entry *e;

        CYG_ASSERT( last_delta >= 0, "Bad last delta" );
        delta = 0x7fffffff;
        for (e = timeouts;  e;  e = e->next) {
            if (e->delta) {
                CYG_ASSERT( e->delta >= last_delta, "e->delta underflow" );
                CYG_ASSERT( last_set_time + e->delta + 1000 > now,
                            "Recorded alarm not in the future!" );
                if ( e->delta < delta )
                    delta = e->delta;
            } else {
                CYG_ASSERT( 0 == e->fun, "Function recorded for 0 delta" );
            }
        }
        if (delta < last_delta) {
            diag_printf("Failed to pick smallest delta - picked: %d, last: %d\n", delta, last_delta);
            for (e = timeouts;  e;  e = e->next) {
                diag_printf("  timeout: %p at %d\n", e->fun, e->delta);
            }
        }
        CYG_ASSERT( delta >= last_delta, "We didn't pick the smallest delta!" );
    }
#endif
    cyg_splx(spl);
}

void 
callout_stop(struct callout *c) 
{
    int spl = cyg_splinternal();

#ifdef TIMEOUT_DEBUG
    diag_printf("%s(%p) = %x\n", __FUNCTION__, c, c->flags);
#endif
    if ((c->flags & CALLOUT_PENDING) == 0) {
        c->flags &= ~CALLOUT_ACTIVE;
        cyg_splx(spl);
        return;
    }
    c->flags &= ~(CALLOUT_PENDING | CALLOUT_ACTIVE);
    if (c->next) {
        c->next->prev = c->prev;
    }
    if (c->prev) {
        c->prev->next = c->next;
    } else {
        timeouts = c->next;
    }
    cyg_splx(spl);
}

int  
callout_active(struct callout *c) 
{
    return ((c->flags & CALLOUT_ACTIVE) != 0);
}

void 
callout_deactivate(struct callout *c) 
{
    c->flags &= ~CALLOUT_ACTIVE;
}

int  
callout_pending(struct callout *c) 
{
    return ((c->flags & CALLOUT_PENDING) != 0);
}


// ------------------------------------------------------------------------

// EOF timeout.c
