/*
 * planner.c - Cartesian trajectory planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2015 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2015 Rob Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* --- Planner Notes ----
 *
 *    The planner works below the canonical machine and above the motor mapping
 *    and stepper execution layers. A rudimentary multitasking capability is
 *    implemented for long-running commands such as lines, arcs, and dwells.
 *    These functions are coded as non-blocking continuations - which are simple
 *    state machines that are re-entered multiple times until a particular
 *    operation is complete. These functions have 2 parts - the initial call,
 *    which sets up the local context, and callbacks (continuations) that are
 *    called from the main loop (in controller.c).
 *
 *    One important concept is isolation of the three layers of the data model -
 *    the Gcode model (gm), planner model (bf queue & mm), and runtime model (mr).
 *    These are designated as "model", "planner" and "runtime" in function names.
 *
 *    The Gcode model is owned by the canonical machine and should only be accessed
 *    by cm_xxxx() functions. Data from the Gcode model is transferred to the planner
 *    by the mp_xxx() functions called by the canonical machine.
 *
 *    The planner should only use data in the planner model. When a move (block)
 *    is ready for execution the planner data is transferred to the runtime model,
 *    which should also be isolated.
 *
 *    Lower-level models should never use data from upper-level models as the data
 *    may have changed and lead to unpredictable results.
 */

#include "planner.h"
#include "arc.h"
#include "canonical_machine.h"
#include "kinematics.h"
#include "stepper.h"
#include "encoder.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>


mpBufferPool_t mb;              // move buffer queue
mpMoveMasterSingleton_t mm;     // context for line planning
mpMoveRuntimeSingleton_t mr;    // context for line runtime

#define _bump(a) ((a < PLANNER_BUFFER_POOL_SIZE - 1) ? a + 1 : 0) // buffer incr & wrap
#define spindle_speed move_time  // local alias for spindle_speed to the time variable
#define value_vector gm.target   // alias for vector of values
#define flag_vector unit         // alias for vector of flags


// Execution routines (NB: These are all called from the LO interrupt)
static stat_t _exec_dwell(mpBuf_t *bf);
static stat_t _exec_command(mpBuf_t *bf);


void planner_init() {
  // If you know all memory has been zeroed by a hard reset you don't need these next 2 lines
  memset(&mr, 0, sizeof(mr));    // clear all values, pointers and status
  memset(&mm, 0, sizeof(mm));    // clear all values, pointers and status
  mp_init_buffers();
}


/*
 * Flush all moves in the planner and all arcs
 *
 *    Does not affect the move currently running in mr.
 *    Does not affect mm or gm model positions
 *    This function is designed to be called during a hold to reset the planner
 *    This function should not generally be called; call cm_queue_flush() instead
 */
void mp_flush_planner() {
  cm_abort_arc();
  mp_init_buffers();
  cm_set_motion_state(MOTION_STOP);
}


/*
 *    Since steps are in motor space you have to run the position vector through inverse
 *    kinematics to get the right numbers. This means that in a non-Cartesian robot changing
 *    any position can result in changes to multiple step values. So this operation is provided
 *    as a single function and always uses the new position vector as an input.
 *
 *    Keeping track of position is complicated by the fact that moves exist in several reference
 *    frames. The scheme to keep this straight is:
 *
 *     - mm.position    - start and end position for planning
 *     - mr.position    - current position of runtime segment
 *     - mr.target      - target position of runtime segment
 *     - mr.endpoint    - final target position of runtime segment
 *
 *    Note that position is set immediately when called and may not be not an accurate representation
 *    of the tool position. The motors are still processing the action and the real tool position is
 *    still close to the starting point.
 */

/// Set planner position for a single axis
void mp_set_planner_position(uint8_t axis, const float position) {mm.position[axis] = position;}


/// Set runtime position for a single axis
void mp_set_runtime_position(uint8_t axis, const float position) {mr.position[axis] = position;}


/// Set encoder counts to the runtime position
void mp_set_steps_to_runtime_position() {
  float step_position[MOTORS];

  ik_kinematics(mr.position, step_position);                // convert lengths to steps in floating point

  for (uint8_t motor = 0; motor < MOTORS; motor++) {
    mr.target_steps[motor] = step_position[motor];
    mr.position_steps[motor] = step_position[motor];
    mr.commanded_steps[motor] = step_position[motor];
    en_set_encoder_steps(motor, step_position[motor]);    // write steps to encoder register

    // These must be zero:
    mr.following_error[motor] = 0;
    st_pre.mot[motor].corrected_steps = 0;
  }
}


/************************************************************************************
 * How this works:
 *   - The command is called by the Gcode interpreter (cm_<command>, e.g. an M code)
 *   - cm_ function calls mp_queue_command which puts it in the planning queue (bf buffer).
 *     This involves setting some parameters and registering a callback to the
 *     execution function in the canonical machine
 *   - the planning queue gets to the function and calls _exec_command()
 *   - ...which puts a pointer to the bf buffer in the prep struct (st_pre)
 *   - When the runtime gets to the end of the current activity (sending steps, counting a dwell)
 *     if executes mp_runtime_command...
 *   - ...which uses the callback function in the bf and the saved parameters in the vectors
 *   - To finish up mp_runtime_command() needs to free the bf buffer
 *
 * Doing it this way instead of synchronizing on queue empty simplifies the
 * handling of feedholds, feed overrides, buffer flushes, and thread blocking,
 * and makes keeping the queue full much easier - therefore avoiding starvation
 */

/// Queue a synchronous Mcode, program control, or other command
void mp_queue_command(void(*cm_exec)(float[], float[]), float *value, float *flag) {
  mpBuf_t *bf;

  // Never supposed to fail as buffer availability was checked upstream in the controller
  if ((bf = mp_get_write_buffer()) == 0) {
    cm_hard_alarm(STAT_BUFFER_FULL_FATAL);
    return;
  }

  bf->move_type = MOVE_TYPE_COMMAND;
  bf->bf_func = _exec_command;                          // callback to planner queue exec function
  bf->cm_func = cm_exec;                                // callback to canonical machine exec function

  for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
    bf->value_vector[axis] = value[axis];
    bf->flag_vector[axis] = flag[axis];
  }

  mp_commit_write_buffer(MOVE_TYPE_COMMAND);            // must be final operation before exit
}


/// callback to execute command
static stat_t _exec_command(mpBuf_t *bf) {
  st_prep_command(bf);
  return STAT_OK;
}


stat_t mp_runtime_command(mpBuf_t *bf) {
  bf->cm_func(bf->value_vector, bf->flag_vector); // 2 vectors used by callbacks

  // free buffer & perform cycle_end if planner is empty
  if (mp_free_run_buffer()) cm_cycle_end();

  return STAT_OK;
}


/* Dwells are performed by passing a dwell move to the stepper drivers.
 * When the stepper driver sees a dwell it times the dwell on a separate
 * timer than the stepper pulse timer.
 */

/// Queue a dwell
stat_t mp_dwell(float seconds) {
  mpBuf_t *bf;

  if ((bf = mp_get_write_buffer()) == 0)              // get write buffer or fail
    return cm_hard_alarm(STAT_BUFFER_FULL_FATAL);     // not ever supposed to fail

  bf->bf_func = _exec_dwell;                          // register callback to dwell start
  bf->gm.move_time = seconds;                         // in seconds, not minutes
  bf->move_state = MOVE_NEW;
  mp_commit_write_buffer(MOVE_TYPE_DWELL);            // must be final operation before exit

  return STAT_OK;
}


/// Dwell execution
static stat_t _exec_dwell(mpBuf_t *bf) {
  st_prep_dwell((uint32_t)(bf->gm.move_time * 1000000)); // convert seconds to uSec
  if (mp_free_run_buffer()) cm_cycle_end();  // free buffer & perform cycle_end if planner is empty

  return STAT_OK;
}


/**** PLANNER BUFFERS *****************************************************
 *
 * Planner buffers are used to queue and operate on Gcode blocks. Each buffer
 * contains one Gcode block which may be a move, and M code, or other command
 * that must be executed synchronously with movement.
 *
 * Buffers are in a circularly linked list managed by a WRITE pointer and a RUN pointer.
 * New blocks are populated by (1) getting a write buffer, (2) populating the buffer,
 * then (3) placing it in the queue (queue write buffer). If an exception occurs
 * during population you can unget the write buffer before queuing it, which returns
 * it to the pool of available buffers.
 *
 * The RUN buffer is the buffer currently executing. It may be retrieved once for
 * simple commands, or multiple times for long-running commands like moves. When
 * the command is complete the run buffer is returned to the pool by freeing it.
 *
 * Notes:
 *    The write buffer pointer only moves forward on _queue_write_buffer, and
 *    the read buffer pointer only moves forward on free_read calls.
 *    (test, get and unget have no effect)
 */


/// Returns # of available planner buffers
uint8_t mp_get_planner_buffers_available() {return mb.buffers_available;}


/// Initializes or resets buffers
void mp_init_buffers() {
  mpBuf_t *pv;

  memset(&mb, 0, sizeof(mb));      // clear all values, pointers and status

  mb.w = &mb.bf[0];                // init write and read buffer pointers
  mb.q = &mb.bf[0];
  mb.r = &mb.bf[0];
  pv = &mb.bf[PLANNER_BUFFER_POOL_SIZE - 1];

  for (uint8_t i = 0; i < PLANNER_BUFFER_POOL_SIZE; i++) { // setup ring pointers
    mb.bf[i].nx = &mb.bf[_bump(i)];
    mb.bf[i].pv = pv;
    pv = &mb.bf[i];
  }

  mb.buffers_available = PLANNER_BUFFER_POOL_SIZE;
}


/// Get pointer to next available write buffer
/// Returns pointer or 0 if no buffer available.
mpBuf_t *mp_get_write_buffer() {
  // get & clear a buffer
  if (mb.w->buffer_state == MP_BUFFER_EMPTY) {
    mpBuf_t *w = mb.w;
    mpBuf_t *nx = mb.w->nx;                   // save linked list pointers
    mpBuf_t *pv = mb.w->pv;
    memset(mb.w, 0, sizeof(mpBuf_t));         // clear all values
    w->nx = nx;                               // restore pointers
    w->pv = pv;
    w->buffer_state = MP_BUFFER_LOADING;
    mb.buffers_available--;
    mb.w = w->nx;

    return w;
  }

  return 0;
}


/// Free write buffer if you decide not to commit it.
void mp_unget_write_buffer() {
  mb.w = mb.w->pv;                            // queued --> write
  mb.w->buffer_state = MP_BUFFER_EMPTY;       // not loading anymore
  mb.buffers_available++;
}


/* Commit the next write buffer to the queue
 * Advances write pointer & changes buffer state
 *
 * WARNING: The calling routine must not use the write buffer
 *  once it has been queued as it may be processed and freed (wiped)
 *  before mp_queue_write_buffer() returns.
 *
 * WARNING: The routine calling mp_commit_write_buffer() must not use the write buffer
 *  once it has been queued. Action may start on the buffer immediately,
 *  invalidating its contents
 */
void mp_commit_write_buffer(const uint8_t move_type) {
  mb.q->move_type = move_type;
  mb.q->move_state = MOVE_NEW;
  mb.q->buffer_state = MP_BUFFER_QUEUED;
  mb.q = mb.q->nx;                            // advance the queued buffer pointer
  st_request_exec_move();                     // requests an exec if the runtime is not busy
  // NB: BEWARE! the exec may result in the planner buffer being
  // processed immediately and then freed - invalidating the contents
}


/* Get pointer to the next or current run buffer
 * Returns a new run buffer if prev buf was ENDed
 * Returns same buf if called again before ENDing
 * Returns 0 if no buffer available
 * The behavior supports continuations (iteration)
 */
mpBuf_t *mp_get_run_buffer() {
  // CASE: fresh buffer; becomes running if queued or pending
  if ((mb.r->buffer_state == MP_BUFFER_QUEUED) ||
      (mb.r->buffer_state == MP_BUFFER_PENDING))
    mb.r->buffer_state = MP_BUFFER_RUNNING;

  // CASE: asking for the same run buffer for the Nth time
  if (mb.r->buffer_state == MP_BUFFER_RUNNING) return mb.r; // return same buffer

  return 0; // CASE: no queued buffers. fail it.
}


/* Release the run buffer & return to buffer pool.
 * Returns true if queue is empty, false otherwise.
 * This is useful for doing queue empty / end move functions.
 */
uint8_t mp_free_run_buffer() {                // EMPTY current run buf & adv to next
  mp_clear_buffer(mb.r);                      // clear it out (& reset replannable)
  mb.r = mb.r->nx;                            // advance to next run buffer

  if (mb.r->buffer_state == MP_BUFFER_QUEUED) // only if queued...
    mb.r->buffer_state = MP_BUFFER_PENDING;   // pend next buffer

  mb.buffers_available++;

  return mb.w == mb.r; // return true if the queue emptied
}


///  Returns pointer to first buffer, i.e. the running block
mpBuf_t *mp_get_first_buffer() {
  return mp_get_run_buffer();    // returns buffer or 0 if nothing's running
}


/// Returns pointer to last buffer, i.e. last block (zero)
mpBuf_t *mp_get_last_buffer() {
  mpBuf_t *bf = mp_get_run_buffer();
  mpBuf_t *bp;

  for (bp = bf; bp && bp->nx != bf; bp = mp_get_next_buffer(bp))
    if (bp->nx->move_state == MOVE_OFF) break;

  return bp;
}


/// Zeroes the contents of the buffer
void mp_clear_buffer(mpBuf_t *bf) {
  mpBuf_t *nx = bf->nx;            // save pointers
  mpBuf_t *pv = bf->pv;
  memset(bf, 0, sizeof(mpBuf_t));
  bf->nx = nx;                     // restore pointers
  bf->pv = pv;
}


///  Copies the contents of bp into bf - preserves links
void mp_copy_buffer(mpBuf_t *bf, const mpBuf_t *bp) {
  mpBuf_t *nx = bf->nx;            // save pointers
  mpBuf_t *pv = bf->pv;
  memcpy(bf, bp, sizeof(mpBuf_t));
  bf->nx = nx;                     // restore pointers
  bf->pv = pv;
}