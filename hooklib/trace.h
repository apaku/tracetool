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

#ifndef TRACELIB_TRACE_H
#define TRACELIB_TRACE_H

#include "tracelib_config.h"
#include "backtrace.h"
#include "configuration.h" // for TraceKey
#include "filemodificationmonitor.h"
#include "getcurrentthreadid.h"
#include "mutex.h"
#include "shutdownnotifier.h"
#include "variabledumping.h"
#include "config.h" // for uint64_t

#include <vector>

TRACELIB_NAMESPACE_BEGIN

class Filter;
class Output;
class Serializer;
struct TracePoint;
class Log;
class LogOutput;

class TracePointSet
{
public:
    static const unsigned int IgnoreTracePoint = 0x0000;
    static const unsigned int LogTracePoint = 0x0001;
    static const unsigned int YieldBacktrace = LogTracePoint | 0x0100;
    static const unsigned int YieldVariables = LogTracePoint | 0x0200;

    TracePointSet( Filter *filter, unsigned int actions );
    ~TracePointSet();

    Filter *filter() { return m_filter; }
    void setFilter( Filter *filter ) { m_filter = filter; }

    unsigned int actionForTracePoint( const TracePoint *tracePoint );

private:
    TracePointSet( const TracePointSet &other );
    void operator=( const TracePointSet &rhs );

    Filter *m_filter;
    const unsigned int m_actions;
};

struct TracedProcess
{
    ProcessId id;
    //TODO: Make this milliseconds too, but how?
    uint64_t startTime;
    std::vector<TraceKey> availableTraceKeys;
};

struct TraceEntry
{
    TraceEntry( const TracePoint *tracePoint_, const char *msg = 0 );
    ~TraceEntry();

    static TracedProcess process;
    const ThreadId threadId;
    const uint64_t timeStamp;
    const TracePoint *tracePoint;
    VariableSnapshot *variables;
    Backtrace *backtrace;
    const char * const message;
    const size_t stackPosition;
};

struct ProcessShutdownEvent
{
    ProcessShutdownEvent();

    const TracedProcess * const process;
    const uint64_t shutdownTime;
};


class Trace : public FileModificationMonitorObserver, public ShutdownNotifierObserver
{
public:
    Trace();
    ~Trace();

    void configureTracePoint( TracePoint *tracePoint ) const;
    bool advanceVisit( TracePoint *tracePoint ) const;
    void visitTracePoint( const TracePoint *tracePoint,
                          const char *msg = 0,
                          VariableSnapshot *variables = 0 );

    void addEntry( const TraceEntry &e );

    void setSerializer( Serializer *serializer );
    void setOutput( Output *output );

    virtual void handleFileModification( const std::string &fileName, NotificationReason reason );

    virtual void handleProcessShutdown();

private:
    Trace( const Trace &trace );
    void operator=( const Trace &trace );

    void reloadConfiguration( const std::string &fileName );

    Serializer *m_serializer;
    Mutex m_serializerMutex;
    Output *m_output;
    Mutex m_outputMutex;
    std::vector<TracePointSet *> m_tracePointSets;
    Configuration *m_configuration;
    mutable Mutex m_configurationMutex;
    BacktraceGenerator m_backtraceGenerator;
    FileModificationMonitor *m_configFileMonitor;
    Log *m_log;
    LogOutput *m_errorOutput;
    LogOutput *m_statusOutput;
};

Trace *getActiveTrace();
void setActiveTrace( Trace *trace );

TRACELIB_NAMESPACE_END

#endif // !defined(TRACELIB_H)
