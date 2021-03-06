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

'use strict'


// Must match modbus.c
var exports = {
  DISCONNECTED: 0,
  OK:           1,
  CRC:          2,
  INVALID:      3,
  TIMEDOUT:     4
};


exports.status_to_string =
  function (status) {
    if (status == exports.OK)       return 'Ok';
    if (status == exports.CRC)      return 'CRC error';
    if (status == exports.INVALID)  return 'Invalid response';
    if (status == exports.TIMEDOUT) return 'Timedout';
    return 'Disconnected';
  }


module.exports = exports;
