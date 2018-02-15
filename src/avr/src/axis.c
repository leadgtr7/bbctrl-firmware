/******************************************************************************\

                 This file is part of the Buildbotics firmware.

                   Copyright (c) 2015 - 2018, Buildbotics LLC
                              All rights reserved.

      This file ("the software") is free software: you can redistribute it
      and/or modify it under the terms of the GNU General Public License,
       version 2 as published by the Free Software Foundation. You should
       have received a copy of the GNU General Public License, version 2
      along with the software. If not, see <http://www.gnu.org/licenses/>.

      The software is distributed in the hope that it will be useful, but
           WITHOUT ANY WARRANTY; without even the implied warranty of
       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                Lesser General Public License for more details.

        You should have received a copy of the GNU Lesser General Public
                 License along with the software.  If not, see
                        <http://www.gnu.org/licenses/>.

                 For information regarding this software email:
                   "Joseph Coffland" <joseph@buildbotics.com>

\******************************************************************************/

#include "axis.h"
#include "motor.h"
#include "switch.h"
#include "util.h"

#include <math.h>
#include <string.h>
#include <ctype.h>


int motor_map[AXES] = {-1, -1, -1, -1, -1, -1};


typedef struct {
  float velocity_max;    // max velocity in mm/min or deg/min
  float accel_max;       // max acceleration in mm/min^2
  float jerk_max;        // max jerk (Jm) in km/min^3
} axis_t;


axis_t axes[MOTORS] = {};


bool axis_is_enabled(int axis) {
  int motor = axis_get_motor(axis);
  return motor != -1 && motor_is_enabled(motor) &&
    !fp_ZERO(axis_get_velocity_max(axis));
}


char axis_get_char(int axis) {
  return (axis < 0 || AXES <= axis) ? '?' : "XYZABCUVW"[axis];
}


int axis_get_id(char axis) {
  const char *axes = "XYZABCUVW";
  char *ptr = strchr(axes, toupper(axis));
  return ptr == 0 ? -1 : (ptr - axes);
}


int axis_get_motor(int axis) {return motor_map[axis];}


// Map axes to first matching motor
void axis_map_motors() {
  for (int axis = 0; axis < AXES; axis++)
    for (int motor = 0; motor < MOTORS; motor++)
      if (motor_get_axis(motor) == axis) {
        motor_map[axis] = motor;
        break;
      }
}


float axis_get_vector_length(const float a[], const float b[]) {
  return sqrt(square(a[AXIS_X] - b[AXIS_X]) + square(a[AXIS_Y] - b[AXIS_Y]) +
              square(a[AXIS_Z] - b[AXIS_Z]) + square(a[AXIS_A] - b[AXIS_A]) +
              square(a[AXIS_B] - b[AXIS_B]) + square(a[AXIS_C] - b[AXIS_C]));
}


#define AXIS_VAR_GET(NAME, TYPE)                        \
  TYPE get_##NAME(int axis) {return axes[axis].NAME;}


#define AXIS_VAR_SET(NAME, TYPE)                        \
  void set_##NAME(int axis, TYPE value) {axes[axis].NAME = value;}


#define AXIS_GET(NAME, TYPE, DEFAULT)                   \
  TYPE axis_get_##NAME(int axis) {                      \
    int motor = axis_get_motor(axis);                   \
    return motor == -1 ? DEFAULT : axes[motor].NAME;    \
  }                                                     \
  AXIS_VAR_GET(NAME, TYPE)


#define AXIS_SET(NAME, TYPE)                            \
  void axis_set_##NAME(int axis, TYPE value) {          \
    int motor = axis_get_motor(axis);                   \
    if (motor != -1) axes[motor].NAME = value;          \
  }                                                     \
  AXIS_VAR_SET(NAME, TYPE)


/// Velocity is scaled by 1,000.
float axis_get_velocity_max(int axis) {
  int motor = axis_get_motor(axis);
  return motor == -1 ? 0 : axes[motor].velocity_max * VELOCITY_MULTIPLIER;
}
AXIS_VAR_GET(velocity_max, float)


/// Acceleration is scaled by 1,000.
float axis_get_accel_max(int axis) {
  int motor = axis_get_motor(axis);
  return motor == -1 ? 0 : axes[motor].accel_max * ACCEL_MULTIPLIER;
}
AXIS_VAR_GET(accel_max, float)


/// Jerk is scaled by 1,000,000.
float axis_get_jerk_max(int axis) {
  int motor = axis_get_motor(axis);
  return motor == -1 ? 0 : axes[motor].jerk_max * JERK_MULTIPLIER;
}
AXIS_VAR_GET(jerk_max, float)


AXIS_VAR_SET(velocity_max, float)
AXIS_VAR_SET(accel_max, float)
AXIS_VAR_SET(jerk_max, float)
