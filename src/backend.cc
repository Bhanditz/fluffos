/* 92/04/18 - cleaned up stylistically by Sulam@TMI */
#include "std.h"
#include "lpc_incl.h"
#include "backend.h"
#include "comm.h"
#include "replace_program.h"
#include "reclaim.h"
#include "socket_efuns.h"
#include "call_out.h"
#include "port.h"
#include "master.h"
#include "eval.h"

#include "event.h"

#ifdef PACKAGE_ASYNC
#include "packages/async.h"
#endif

#ifdef WIN32
#include <process.h>
void CDECL alarm_loop(void *);
#endif

#include <deque>
#include <functional>
#include <map>

error_context_t *current_error_context = 0;

/*
 * The 'current_time' is updated in the backend loop.
 */
long current_virtual_time;

static std::multimap<long, tick_event *, std::less<long>> g_tick_queue;

tick_event *add_tick_event(int delay_secs,
                           tick_event::callback_type callback)
{
  auto event = new tick_event(callback);
  g_tick_queue.insert(std::make_pair(current_virtual_time + delay_secs, event));
  return event;
}

void call_tick_events()
{
  if (g_tick_queue.empty()) {
    return;
  }

  auto iter_start = g_tick_queue.cbegin();
  if (iter_start->first > current_virtual_time) {
    return;
  }
  auto iter_end = g_tick_queue.upper_bound(current_virtual_time);

  std::deque<tick_event *> all_events;

  // Extract all eligible events
  for (auto iter = iter_start; iter != iter_end; iter++) {
    all_events.push_back(iter->second);
  }
  g_tick_queue.erase(iter_start, iter_end);

  // TODO: randomly shuffle the events

  // FIXME: push econ check into event callback!
  error_context_t econ;
  if (!save_context(&econ)) {
    fatal("BUG: call_tick_events can not save context!");
  }
  for (auto event: all_events) {
    if (event->valid) {
      try {
        event->callback();
      } catch (const char *) {
        restore_context(&econ);
      }
    }
    delete event;
  }
  pop_context(&econ);
}

void clear_tick_events()
{
  if (!g_tick_queue.empty()) {
    for (auto iter = g_tick_queue.cbegin(); iter != g_tick_queue.cend();
         iter++) {
      delete iter->second;
    }
    g_tick_queue.clear();
  }
}

object_t *current_heart_beat;
static void look_for_objects_to_swap(void);
void call_heart_beat(void);

#if 0
static void report_holes(void);
#endif

/*
 * There are global variables that must be zeroed before any execution.
 * In case of errors, there will be a LONGJMP(), and the variables will
 * have to be cleared explicitely. They are normally maintained by the
 * code that use them.
 *
 * This routine must only be called from top level, not from inside
 * stack machine execution (as stack will be cleared).
 */
void clear_state()
{
  current_object = 0;
  set_command_giver(0);
  current_interactive = 0;
  previous_ob = 0;
  current_prog = 0;
  caller_type = 0;
  reset_machine(0); /* Pop down the stack. */
} /* clear_state() */

#if 0
static void report_holes()
{
  if (current_object && current_object->name) {
    debug_message("current_object is /%s\n", current_object->name);
  }
  if (command_giver && command_giver->name) {
    debug_message("command_giver is /%s\n", command_giver->name);
  }
  if (current_interactive && current_interactive->name) {
    debug_message("current_interactive is /%s\n", current_interactive->name);
  }
  if (previous_ob && previous_ob->name) {
    debug_message("previous_ob is /%s\n", previous_ob->name);
  }
  if (current_prog && current_prog->name) {
    debug_message("current_prog is /%s\n", current_prog->name);
  }
  if (caller_type) {
    debug_message("caller_type is %s\n", caller_type);
  }
}
#endif

/*
 * This is the backend. We will stay here for ever (almost).
 */
void backend(struct event_base *base)
{
  // FIXME: handle this in call_tick_events().
  error_context_t econ;
  save_context(&econ);

  clear_state();

  // Register various tick events
  add_tick_event(0, tick_event::callback_type(call_heart_beat));
  add_tick_event(5 * 60, tick_event::callback_type(look_for_objects_to_swap));
  add_tick_event(60,
                 tick_event::callback_type(std::bind(reclaim_objects, true)));
#ifdef PACKAGE_MUDLIB_STATS
  add_tick_event(60 * 60, tick_event::callback_type(mudlib_stats_decay));
#endif

  current_virtual_time = get_current_time();

  while (1)
    try {
      clear_state();

      while (1) {
        if (obj_list_replace || obj_list_destruct) {
          remove_destructed_objects();
        }

        /*
         * shut down MudOS if MudOS_is_being_shut_down is set.
         */
        if (MudOS_is_being_shut_down) {
          shutdownMudOS(0);
        }
        if (slow_shut_down_to_do) {
          int tmp = slow_shut_down_to_do;

          slow_shut_down_to_do = 0;
          slow_shut_down(tmp);
        }

#if DEBUG
        try {
#endif
          /* Run event loop for at most 1 second, this current handles
           * listening socket events, user socket events, and lpc socket events.
           *
           * It currently also handles user command, longer term plan is to
           * merge all callbacks execution into tick event loop and move all
           * I/O to dedicated threads.
           */
          run_for_at_least_one_second(base);

#if DEBUG
        } catch (...) { // catch everything
          fatal("BUG: jumped out of event loop!");
        }
#endif
        int64_t real_time = get_current_time();

        while (current_virtual_time < real_time) {
          call_tick_events();
          current_virtual_time++;
        }

#ifdef PACKAGE_ASYNC
        // TODO: Move this into timer based.
        check_reqs();
#endif
      }
    } catch (const char *) {
      restore_context(&econ);
    }
} /* backend() */

/*
 * Despite the name, this routine takes care of several things.
 * It will run once every 5 minutes.
 *
 * . It will loop through all objects.
 *
 *   . If an object is found in a state of not having done reset, and the
 *     delay to next reset has passed, then reset() will be done.
 *
 *   . If the object has a existed more than the time limit given for swapping,
 *     then 'clean_up' will first be called in the object
 *
 * There are some problems if the object self-destructs in clean_up, so
 * special care has to be taken of how the linked list is used.
 */
static void look_for_objects_to_swap()
{
  /* Next time is in 5 minutes */
  add_tick_event(5 * 60, tick_event::callback_type(look_for_objects_to_swap));

  object_t *ob;
  volatile object_t *next_ob, *last_good_ob;
  error_context_t econ;

  /*
   * Objects object can be destructed, which means that next object to
   * investigate is saved in next_ob. If very unlucky, that object can be
   * destructed too. In that case, the loop is simply restarted.
   */
  next_ob = obj_list;
  last_good_ob = obj_list;
  save_context(&econ);
  while (1)
    try {

      while ((ob = (object_t *) next_ob)) {
        int ready_for_clean_up = 0;

        if (ob->flags & O_DESTRUCTED) {
          if (last_good_ob->flags & O_DESTRUCTED) {
            ob = obj_list; /* restart */
          } else {
            ob = (object_t *) last_good_ob;
          }
        }
        next_ob = ob->next_all;

        /*
         * Check reference time before reset() is called.
         */
        if (current_virtual_time - ob->time_of_ref >= time_to_clean_up) {
          ready_for_clean_up = 1;
        }
#if !defined(NO_RESETS) && !defined(LAZY_RESETS)
        /*
         * Should this object have reset(1) called ?
         */
        if ((ob->flags & O_WILL_RESET)
            && (ob->next_reset <= current_virtual_time)
            && !(ob->flags & O_RESET_STATE)) {
          debug(d_flag, "RESET /%s\n", ob->obname);
          set_eval(max_cost);
          reset_object(ob);
          if (ob->flags & O_DESTRUCTED) {
            continue;
          }
        }
#endif
        if (time_to_clean_up > 0) {
          /*
           * Has enough time passed, to give the object a chance to
           * self-destruct ? Save the O_RESET_STATE, which will be cleared.
           *
           * Only call clean_up in objects that has defined such a function.
           *
           * Only if the clean_up returns a non-zero value, will it be called
           * again.
           */

          if (ready_for_clean_up && (ob->flags & O_WILL_CLEAN_UP)) {
            int save_reset_state = ob->flags & O_RESET_STATE;
            svalue_t *svp;

            debug(d_flag, "clean up /%s\n", ob->obname);

            /*
             * Supply a flag to the object that says if this program is
             * inherited by other objects. Cloned objects might as well
             * believe they are not inherited. Swapped objects will not
             * have a ref count > 1 (and will have an invalid ob->prog
             * pointer).
             *
             * Note that if it is in the apply_low cache, it will also
             * get a flag of 1, which may cause the mudlib not to clean
             * up the object.  This isn't bad because:
             * (1) one expects it is rare for objects that have untouched
             * long enough to clean_up to still be in the cache, especially
             * on busy MUDs.
             * (2) the ones that are are the more heavily used ones, so
             * keeping them around seems justified.
             */

            push_number(ob->flags & (O_CLONE) ? 0 : ob->prog->ref);
            set_eval(max_cost);
            svp = apply(APPLY_CLEAN_UP, ob, 1, ORIGIN_DRIVER);
            if (ob->flags & O_DESTRUCTED) {
              continue;
            }
            if (!svp || (svp->type == T_NUMBER && svp->u.number == 0)) {
              ob->flags &= ~O_WILL_CLEAN_UP;
            }
            ob->flags |= save_reset_state;
          }
        }
        last_good_ob = ob;
      }
      break;
    } catch (const char *) {
      restore_context(&econ);

    }
  pop_context(&econ);
} /* look_for_objects_to_swap() */

/* Call all heart_beat() functions in all objects.  Also call the next reset,
 * and the call out.
 * We do heart beats by moving each object done to the end of the heart beat
 * list before we call its function, and always using the item at the head
 * of the list as our function to call.  We keep calling heart beats until
 * a timeout or we have done num_heart_objs calls.  It is done this way so
 * that objects can delete heart beating objects from the list from within
 * their heart beat without truncating the current round of heart beats.
 *
 * Set command_giver to current_object if it is a living object. If the object
 * is shadowed, check the shadowed object if living. There is no need to save
 * the value of the command_giver, as the caller resets it to 0 anyway.  */

typedef struct {
  object_t *ob;
  short heart_beat_ticks;
  short time_to_heart_beat;
} heart_beat_t;

static heart_beat_t *heart_beats = 0;
static int max_heart_beats = 0;
static int heart_beat_index = 0;
static int num_hb_objs = 0;
static int num_hb_to_do = 0;
int time_for_hb = 0;

static int num_hb_calls = 0; /* starts */
static float perc_hb_probes = 100.0; /* decaying avge of how many complete */

void call_heart_beat()
{
  // Register for next call
  add_tick_event(HEARTBEAT_INTERVAL,
                 tick_event::callback_type(call_heart_beat));

  object_t *ob;
  heart_beat_t *curr_hb;
  error_context_t econ;

  current_interactive = 0;

  if ((num_hb_to_do = num_hb_objs)) {
    num_hb_calls++;
    heart_beat_index = 0;
    save_context(&econ);
    while (1) {
      ob = (curr_hb = &heart_beats[heart_beat_index])->ob;
      DEBUG_CHECK(!(ob->flags & O_HEART_BEAT),
                  "Heartbeat not set in object on heartbeat list!");
      /* is it time to do a heart beat ? */
      curr_hb->heart_beat_ticks--;

      if (ob->prog->heart_beat != 0) {
        if (curr_hb->heart_beat_ticks < 1) {
          object_t *new_command_giver;
          curr_hb->heart_beat_ticks = curr_hb->time_to_heart_beat;
          current_heart_beat = ob;
          new_command_giver = ob;
#ifndef NO_SHADOWS
          while (new_command_giver->shadowing) {
            new_command_giver = new_command_giver->shadowing;
          }
#endif
#ifndef NO_ADD_ACTION
          if (!(new_command_giver->flags & O_ENABLE_COMMANDS)) {
            new_command_giver = 0;
          }
#endif
#ifdef PACKAGE_MUDLIB_STATS
          add_heart_beats(&ob->stats, 1);
#endif
          set_eval(max_cost);
          try {
            save_command_giver(new_command_giver);
            if (ob->interactive) { //note, NOT same as new_command_giver
              current_interactive = ob;
            }
            call_direct(ob, ob->prog->heart_beat - 1, ORIGIN_DRIVER, 0);
            current_interactive = 0;
            pop_stack(); /* pop the return value */
            restore_command_giver();
          } catch (const char *) {
            restore_context(&econ);
          }

          current_object = 0;
        }
      }
      if (++heart_beat_index == num_hb_to_do) {
        break;
      }
    }
    pop_context(&econ);
    if (heart_beat_index < num_hb_to_do) {
      perc_hb_probes = 100 * (float) heart_beat_index / num_hb_to_do;
    } else {
      perc_hb_probes = 100.0;
    }
    heart_beat_index = num_hb_to_do = 0;
  }
  current_prog = 0;
  current_heart_beat = 0;
} /* call_heart_beat() */

int query_heart_beat(object_t *ob)
{
  int index;

  if (!(ob->flags & O_HEART_BEAT)) {
    return 0;
  }
  index = num_hb_objs;
  while (index--) {
    if (heart_beats[index].ob == ob) {
      return heart_beats[index].time_to_heart_beat;
    }
  }
  return 0;
} /* query_heart_beat() */

/* add or remove an object from the heart beat list; does the major check...
 * If an object removes something from the list from within a heart beat,
 * various pointers in call_heart_beat could be stuffed, so we must
 * check current_heart_beat and adjust pointers.  */

int set_heart_beat(object_t *ob, int to)
{
  int index;

  if (ob->flags & O_DESTRUCTED) {
    return 0;
  }

  if (!to) {
    int num;

    index = num_hb_objs;
    while (index--) {
      if (heart_beats[index].ob == ob) {
        break;
      }
    }
    if (index < 0) {
      return 0;
    }

    if (num_hb_to_do) {
      if (index <= heart_beat_index) {
        heart_beat_index--;
      }
      if (index < num_hb_to_do) {
        num_hb_to_do--;
      }
    }

    if ((num = (num_hb_objs - (index + 1)))) {
      memmove(heart_beats + index, heart_beats + (index + 1),
              num * sizeof(heart_beat_t));
    }

    num_hb_objs--;
    ob->flags &= ~O_HEART_BEAT;
    return 1;
  }

  if (ob->flags & O_HEART_BEAT) {
    if (to < 0) {
      return 0;
    }

    index = num_hb_objs;
    while (index--) {
      if (heart_beats[index].ob == ob) {
        heart_beats[index].time_to_heart_beat =
          heart_beats[index].heart_beat_ticks = to;
        break;
      }
    }
    DEBUG_CHECK(index < 0,
                "Couldn't find enabled object in heart_beat list!\n");
  } else {
    heart_beat_t *hb;

    if (!max_heart_beats)
      heart_beats = CALLOCATE(max_heart_beats = HEART_BEAT_CHUNK,
                              heart_beat_t, TAG_HEART_BEAT,
                              "set_heart_beat: 1");
    else if (num_hb_objs == max_heart_beats) {
      max_heart_beats += HEART_BEAT_CHUNK;
      heart_beats = RESIZE(heart_beats, max_heart_beats,
                           heart_beat_t, TAG_HEART_BEAT,
                           "set_heart_beat: 1");
    }

    hb = &heart_beats[num_hb_objs++];
    hb->ob = ob;
    if (to < 0) {
      to = 1;
    }
    hb->time_to_heart_beat = to;
    hb->heart_beat_ticks = to;
    ob->flags |= O_HEART_BEAT;
  }

  return 1;
}

int heart_beat_status(outbuffer_t *ob, int verbose)
{
  char buf[20];

  if (verbose == 1) {
    outbuf_add(ob, "Heart beat information:\n");
    outbuf_add(ob, "-----------------------\n");
    outbuf_addv(ob, "Number of objects with heart beat: %d, starts: %d\n",
                num_hb_objs, num_hb_calls);

    /* passing floats to varargs isn't highly portable so let sprintf
     handle it */
    sprintf(buf, "%.2f", perc_hb_probes);
    outbuf_addv(ob, "Percentage of HB calls completed last time: %s\n", buf);
  }
  return (0);
} /* heart_beat_status() */

/* New version used when not in -o mode. The epilog() in master.c is
 * supposed to return an array of files (castles in 2.4.5) to load. The array
 * returned by apply() will be freed at next call of apply(), which means that
 * the ref count has to be incremented to protect against deallocation.
 *
 * The master object is asked to do the actual loading.
 */
void preload_objects(int eflag)
{
  volatile array_t *prefiles;
  svalue_t *ret;
  volatile int ix;
  error_context_t econ;

  save_context(&econ);
  try {
    push_number(eflag);
    ret = apply_master_ob(APPLY_EPILOG, 1);
  } catch (const char *) {
    restore_context(&econ);
    pop_context(&econ);
    return;
  }

  pop_context(&econ);
  if ((ret == 0) || (ret == (svalue_t *) - 1) || (ret->type != T_ARRAY)) {
    return;
  } else {
    prefiles = ret->u.arr;
  }
  if ((prefiles == 0) || (prefiles->size < 1)) {
    return;
  }

  debug_message("\nLoading preloaded files ...\n");
  prefiles->ref++;
  ix = 0;
  /* in case of an error, effectively do a 'continue' */
  save_context(&econ);
  while (1)
    try {
      for (; ix < prefiles->size; ix++) {
        if (prefiles->item[ix].type != T_STRING) {
          continue;
        }

        set_eval(max_cost);

        push_svalue(((array_t *)prefiles)->item + ix);
        (void) apply_master_ob(APPLY_PRELOAD, 1);
      }
      free_array((array_t *) prefiles);
      break;
    } catch (const char *) {
      restore_context(&econ);
      ix++;
    }
  pop_context(&econ);
} /* preload_objects() */

/* All destructed objects are moved into a sperate linked list,
 * and deallocated after program execution.  */

void remove_destructed_objects()
{
  object_t *ob, *next;

  if (obj_list_replace) {
    replace_programs();
  }
  for (ob = obj_list_destruct; ob; ob = next) {
    next = ob->next_all;
    destruct2(ob);
  }
  obj_list_destruct = 0;
} /* remove_destructed_objects() */

static double load_av = 0.0;

void update_load_av()
{
  static int last_time;
  int n;
  double c;
  static int acc = 0;

  acc++;
  if (current_virtual_time == last_time) {
    return;
  }
  n = current_virtual_time - last_time;
  if (n < NUM_CONSTS) {
    c = consts[n];
  } else {
    c = exp(-n / 900.0);
  }
  load_av = c * load_av + acc * (1 - c) / n;
  last_time = current_virtual_time;
  acc = 0;
} /* update_load_av() */

static double compile_av = 0.0;

void update_compile_av(int lines)
{
  static int last_time;
  int n;
  double c;
  static int acc = 0;

  acc += lines;
  if (current_virtual_time == last_time) {
    return;
  }
  n = current_virtual_time - last_time;
  if (n < NUM_CONSTS) {
    c = consts[n];
  } else {
    c = exp(-n / 900.0);
  }
  compile_av = c * compile_av + acc * (1 - c) / n;
  last_time = current_virtual_time;
  acc = 0;
} /* update_compile_av() */

char *query_load_av()
{
  static char buff[100];

  sprintf(buff, "%.2f cmds/s, %.2f comp lines/s", load_av, compile_av);
  return (buff);
} /* query_load_av() */

#ifdef F_HEART_BEATS
array_t *get_heart_beats()
{
  int nob = 0, n = num_hb_objs;
  heart_beat_t *hb = heart_beats;
  object_t **obtab;
  array_t *arr;
#ifdef F_SET_HIDE
  int apply_valid_hide = 1, display_hidden = 0;
#endif
  if (n) {
    obtab = CALLOCATE(n, object_t *, TAG_TEMPORARY, "heart_beats");
  } else {
    obtab = NULL;
  }
  while (n--) {
#ifdef F_SET_HIDE
    if (hb->ob->flags & O_HIDDEN) {
      if (apply_valid_hide) {
        apply_valid_hide = 0;
        display_hidden = valid_hide(current_object);
      }
      if (!display_hidden) {
        continue;
      }
    }
#endif
    obtab[nob++] = (hb++)->ob;
  }

  arr = allocate_empty_array(nob);
  while (nob--) {
    arr->item[nob].type = T_OBJECT;
    arr->item[nob].u.ob = obtab[nob];
    add_ref(arr->item[nob].u.ob, "get_heart_beats");
  }
  if (obtab) {
    FREE(obtab);
  }

  return arr;
}
#endif
