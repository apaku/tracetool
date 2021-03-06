/* tracetool - a framework for tracing the execution of C++ programs
 * Copyright 2010-2016 froglogic GmbH
 *
 * This file is part of tracetool.
 *
 * tracetool is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * tracetool is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with tracetool.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRACELIB_GETCURRENTTHREADID_H
#define TRACELIB_GETCURRENTTHREADID_H

#include "tracelib_config.h"
#include "config.h" // for uint64_t

TRACELIB_NAMESPACE_BEGIN

typedef unsigned long ProcessId;
typedef unsigned long ThreadId;

uint64_t getCurrentProcessStartTime();
ProcessId getCurrentProcessId();
ThreadId getCurrentThreadId();

TRACELIB_NAMESPACE_END

#endif // !defined(TRACELIB_GETCURRENTTHREADID_H)

