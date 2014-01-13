/**********************************************************************
** Copyright (C) 2010 froglogic GmbH.
** All rights reserved.
**********************************************************************/

#include "server.h"

#include "database.h"
#include "datagramtypes.h"
#include "lru_cache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cassert>
#include <stdexcept>

using namespace std;

static bool getGroupId( QSqlDatabase db, Transaction *transaction, const QString &name, unsigned int *id )
{
    QVariant v = transaction->exec( QString( "SELECT id FROM trace_point_group WHERE name=%1;" ).arg( Database::formatValue( db, name ) ) );
    if ( !v.isValid() ) {
        v = transaction->insert( QString( "INSERT INTO trace_point_group VALUES(NULL, %1);" ).arg( Database::formatValue( db, name ) ) );
    }

    if ( !id ) {
        return true;
    }

    bool ok;
    *id = v.toUInt( &ok );
    return ok;
}

class TraceKeyCache {
public:
    void update( QSqlDatabase db, Transaction *transaction,
		 const QString &groupName,
		 const QList<TraceKey> &traceKeys ) {
        QList<TraceKey>::ConstIterator it, end = traceKeys.end();
        for ( it = traceKeys.begin(); it != end; ++it ) {
            if ( m_map.find( (*it).name ) == m_map.end() ) {
                registerGroupName( db, transaction, (*it).name );
            }
        }
        // in case the entry comes with a name not listed in the
        // AUT-side configuration file
        if ( !groupName.isNull() && m_map.find( groupName ) == m_map.end() ) {
            registerGroupName( db, transaction, groupName );
        }
    }
    void clear() {
        m_map.clear();
    }
    unsigned int fetch( const QString &name ) const {
        std::map<QString, unsigned int>::const_iterator it = m_map.find( name );
        if ( it == m_map.end() ) {
            throw runtime_error( QString( "Failed to find trace point group %1 in cache" ).arg( name ).toUtf8().constData() );
        }
        return (*it).second;
    }
private:
    void registerGroupName( QSqlDatabase db, Transaction *transaction, const QString &name )
    {
        unsigned int id;
        if ( !getGroupId( db, transaction, name, &id ) ) {
            throw runtime_error( "Read non-numeric trace point group id from database - corrupt database?" );
        }
        m_map[name] = id;
    }

    std::map<QString, unsigned int> m_map;
} traceKeyCache;

XmlContentHandler::XmlContentHandler( Server *server )
    : m_server( server ),
    m_inFrameElement( false )
{
}

void XmlContentHandler::addData( const QByteArray &data )
{
    m_xmlReader.addData( data );
}

void XmlContentHandler::continueParsing()
{
    while ( !m_xmlReader.atEnd() ) {
        m_xmlReader.readNext();
        switch ( m_xmlReader.tokenType( )) {
            case QXmlStreamReader::StartElement:
                handleStartElement();
                break;
            case QXmlStreamReader::Characters:
                m_s = m_xmlReader.text().toString();
                break;
            case QXmlStreamReader::EndElement:
                handleEndElement();
                break;
            default:
                break;
        }
    }
}

void XmlContentHandler::handleStartElement()
{
    const QXmlStreamAttributes atts = m_xmlReader.attributes();
    if ( m_xmlReader.name() == QLatin1String( "traceentry" ) ) {
        m_currentEntry = TraceEntry();
        m_currentEntry.pid = atts.value( QLatin1String( "pid" ) ).toString().toUInt();
        qulonglong datetime = atts.value( QLatin1String( "process_starttime" ) ).toString().toULongLong();
        qint64 signedDt = datetime;
        QDateTime dt = QDateTime::fromMSecsSinceEpoch( signedDt );
        m_currentEntry.processStartTime = dt;
        m_currentEntry.tid = atts.value( QLatin1String( "tid" ) ).toString().toUInt();
        m_currentEntry.timestamp = QDateTime::fromMSecsSinceEpoch( atts.value( QLatin1String( "time" ) ).toString().toULongLong() );
    } else if ( m_xmlReader.name() == QLatin1String( "variable" ) ) {
        m_currentVariable = Variable();
        m_currentVariable.name = atts.value( QLatin1String( "name" ) ).toString();
        const QStringRef typeStr = atts.value( QLatin1String( "type" ) );
        if ( typeStr == QLatin1String( "string" ) ) {
            m_currentVariable.type = TRACELIB_NAMESPACE_IDENT(VariableType)::String;
        } else if ( typeStr == QLatin1String( "number" ) ) {
            m_currentVariable.type = TRACELIB_NAMESPACE_IDENT(VariableType)::Number;
        } else if ( typeStr == QLatin1String( "float" ) ) {
            m_currentVariable.type = TRACELIB_NAMESPACE_IDENT(VariableType)::Float;
        } else if ( typeStr == QLatin1String( "boolean" ) ) {
            m_currentVariable.type = TRACELIB_NAMESPACE_IDENT(VariableType)::Boolean;
        }
    } else if ( m_xmlReader.name() == QLatin1String( "location" ) ) {
        m_currentLineNo = atts.value( QLatin1String( "lineno" ) ).toString().toULong();
    } else if ( m_xmlReader.name() == QLatin1String( "frame" ) ) {
        m_inFrameElement = true;
        m_currentFrame = StackFrame();
    } else if ( m_xmlReader.name() == QLatin1String( "function" ) ) {
        m_currentFrame.functionOffset = atts.value( QLatin1String( "offset" ) ).toString().toUInt();
    } else if ( m_xmlReader.name() == QLatin1String( "shutdownevent" ) ) {
        m_currentShutdownEvent = ProcessShutdownEvent();
        m_currentShutdownEvent.pid = atts.value( QLatin1String( "pid" ) ).toString().toUInt();
        m_currentShutdownEvent.startTime = QDateTime::fromMSecsSinceEpoch( atts.value( QLatin1String( "starttime" ) ).toString().toULongLong() );
        m_currentShutdownEvent.stopTime = QDateTime::fromMSecsSinceEpoch( atts.value( QLatin1String( "endtime" ) ).toString().toULongLong() );
    } else if ( m_xmlReader.name() == QLatin1String( "storageconfiguration" ) ) {
        m_currentStorageConfig = StorageConfiguration();
        m_currentStorageConfig.maximumSize = atts.value( QLatin1String( "maxSize" ) ).toString().toULong();
        m_currentStorageConfig.shrinkBy = atts.value( QLatin1String( "shrinkBy" ) ).toString().toUInt();
    } else if ( m_xmlReader.name() == QLatin1String( "key" ) ) {
        m_currentTraceKey = TraceKey();
        m_currentTraceKey.enabled = atts.value( QLatin1String( "enabled" ) ) == QLatin1String( "true" );
    }
}

void XmlContentHandler::handleEndElement()
{
    if ( m_xmlReader.name() == QLatin1String( "traceentry" ) ) {
        m_server->handleTraceEntry( m_currentEntry );
    } else if ( m_xmlReader.name() == QLatin1String( "variable" ) ) {
        m_currentVariable.value = m_s.trimmed();
        m_currentEntry.variables.append( m_currentVariable );
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "processname" ) ) {
        m_currentEntry.processName = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "stackposition" ) ) {
        m_currentEntry.stackPosition = m_s.trimmed().toULong();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "type" ) ) {
        m_currentEntry.type = m_s.trimmed().toUInt();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "location" ) ) {
        if ( m_inFrameElement ) {
            m_currentFrame.sourceFile = m_s.trimmed();
            m_currentFrame.lineNumber = m_currentLineNo;
        } else {
            m_currentEntry.path = m_s.trimmed();
            m_currentEntry.lineno = m_currentLineNo;
        }
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "group" ) ) {
        m_currentEntry.groupName = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "function" ) ) {
        m_currentEntry.function = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "message" ) ) {
        m_currentEntry.message = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "module" ) ) {
        m_currentFrame.module = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "function" ) ) {
        m_currentFrame.function = m_s.trimmed();
        m_s.clear();
    } else if ( m_xmlReader.name() == QLatin1String( "frame" ) ) {
        m_inFrameElement = false;
        m_currentEntry.backtrace.append( m_currentFrame );
    } else if ( m_xmlReader.name() == QLatin1String( "shutdownevent" ) ) {
        m_currentShutdownEvent.name = m_s.trimmed();
        m_s.clear();
        m_server->handleShutdownEvent( m_currentShutdownEvent );
    } else if ( m_xmlReader.name() == QLatin1String( "key" ) ) {
        m_currentTraceKey.name = m_s.trimmed();
        m_s.clear();
        m_currentEntry.traceKeys.append( m_currentTraceKey );
    } else if ( m_xmlReader.name() == QLatin1String( "storageconfiguration" ) ) {
        m_currentStorageConfig.archiveDir = m_s.trimmed();
        m_s.clear();
        m_server->applyStorageConfiguration( m_currentStorageConfig );
    }
}

template <typename KeyType, typename IdType, int CacheSize = 10>
class StorageCache
{
public:
    StorageCache() : m_lru( CacheSize ) { }
    void clear()
    {
	m_lru.clear();
    }
protected:
    typedef KeyType CacheKey;

    IdType* checkCache( const KeyType &key )
    {
	return m_lru.fetch_ptr( key );
    }
    void cache( const KeyType &key, IdType id )
    {
	m_lru.insert( key, id );
    }

private:
    LRUCache<KeyType, IdType> m_lru; 
};

class PathCache : public StorageCache<QString, unsigned int> {
public:
    unsigned int store( QSqlDatabase db, Transaction *transaction,
			const QString &path )
    {
	unsigned int *cachedId = checkCache( path );
	if ( cachedId )
	    return *cachedId;
	QVariant v = transaction->exec( QString( "SELECT id FROM path_name WHERE name=%1;" ).arg( Database::formatValue( db, path ) ) );
	if ( !v.isValid() ) {
	    v = transaction->insert( QString( "INSERT INTO path_name VALUES(NULL, %1);" ).arg( Database::formatValue( db, path ) ) );
	}
	bool ok;
	unsigned int pathId = v.toUInt( &ok );
	if ( !ok ) {
	    throw runtime_error( "Failed to store entry in database: read non-numeric path id from database - corrupt database?" );
	}
	cache( path, pathId );
	return pathId;
    }
} pathCache;

class FunctionCache : public StorageCache<QString, unsigned int> {
public:
    unsigned int store( QSqlDatabase db, Transaction *transaction,
			const QString &function )
    {
	unsigned int *cachedId = checkCache( function );
	if ( cachedId )
	    return *cachedId;
	QVariant v = transaction->exec( QString( "SELECT id FROM function_name WHERE name=%1;" ).arg( Database::formatValue( db, function ) ) );
	if ( !v.isValid() ) {
	    v = transaction->insert( QString( "INSERT INTO function_name VALUES(NULL, %1);" ).arg( Database::formatValue( db, function ) ) );
	}
	bool ok;
	unsigned int functionId = v.toUInt( &ok );
	if ( !ok ) {
	    throw runtime_error( "Failed to store entry in database: read non-numeric function id from database - corrupt database?" );
	}
	cache( function, functionId );
	return functionId;
    }
} functionCache;

class ProcessCache : public StorageCache<std::pair<QString, unsigned int>,
					 unsigned int>
{
public:
    unsigned int store( QSqlDatabase db, Transaction *transaction,
			const QString &processName,
			unsigned int pid,
			const QDateTime &processStartTime )
    {
	CacheKey key( processName, pid );
	unsigned int *cachedId = checkCache( key );
	if ( cachedId )
	    return *cachedId;
	QVariant v = transaction->exec( QString( "SELECT id FROM process WHERE pid=%1 AND start_time=%2;" ).arg( pid ).arg( Database::formatValue( db, processStartTime ) ) );
	if ( !v.isValid() ) {
	    v = transaction->insert( QString( "INSERT INTO process VALUES(NULL, %1, %2, %3, 0);" ).arg( Database::formatValue( db, processName ) ).arg( pid ).arg( Database::formatValue( db, processStartTime ) ) );
	}
	bool ok;
	unsigned int processId = v.toUInt( &ok );
	if ( !ok ) {
	    throw runtime_error( "Failed to store entry in database: read non-numeric process id from database - corrupt database?" );
	}
	cache( key, processId );
	return processId;
    }
} processCache;

class ThreadCache : public StorageCache<std::pair<unsigned int, unsigned int>,
					unsigned int>
{
public:
    unsigned int store( QSqlDatabase db, Transaction *transaction,
			unsigned int processId,
			unsigned int tid )
    {
	CacheKey key( processId, tid );
	unsigned int *cachedId = checkCache( key );
	if ( cachedId )
	    return *cachedId;

	QVariant v = transaction->exec( QString( "SELECT id FROM traced_thread WHERE process_id=%1 AND tid=%2;" ).arg( processId ).arg( tid ) );
	if ( !v.isValid() ) {
	    v = transaction->insert( QString( "INSERT INTO traced_thread VALUES(NULL, %1, %2);" ).arg( processId ).arg( tid ) );
	}
	bool ok;
	unsigned int threadId = v.toUInt( &ok );
	if ( !ok ) {
	    throw runtime_error( "Failed to store entry in database: read non-numeric traced thread id from database - corrupt database?" );
	}
	cache( key, threadId );
	return threadId;
    }
} threadCache;

static unsigned int storeGroup( QSqlDatabase db, Transaction *transaction,
				const QString &groupName,
				const QList<TraceKey> &traceKeys )
{
    traceKeyCache.update( db, transaction, groupName, traceKeys );

    unsigned int groupId = 0;
    if ( !groupName.isNull() ) {
	groupId = traceKeyCache.fetch( groupName );
    }
    return groupId;
}

// ### some portable, ready-made tuple template type would be nice
struct TracePointTuple
{
    unsigned int type;
    unsigned int pathId;
    unsigned long lineno;
    unsigned int functionId;
    unsigned int groupId;

    bool operator<(const TracePointTuple &tp) const
    {
	return type < tp.type &&
	    pathId < tp.pathId &&
	    lineno < tp.lineno &&
	    functionId < tp.functionId &&
	    groupId < tp.groupId;
    };
};

class TracePointCache : public StorageCache<TracePointTuple,
					    unsigned int>
{
public:
    unsigned int store( QSqlDatabase db, Transaction *transaction,
			unsigned int type,
			unsigned int pathId,
			unsigned long lineno,
			unsigned int functionId,
			unsigned int groupId )
    {
	CacheKey key;
	key.type = type;
	key.pathId = pathId;
	key.lineno = lineno;
	key.functionId = functionId;
	key.groupId = groupId;
	unsigned int *cachedId = checkCache( key );
	if ( cachedId )
	    return *cachedId;
	QVariant v = transaction->exec( QString( "SELECT id FROM trace_point WHERE type=%1 AND path_id=%2 AND line=%3 AND function_id=%4 AND group_id=%5;" ).arg( type ).arg( pathId ).arg( lineno ).arg( functionId ).arg( groupId ) );
	if ( !v.isValid() ) {
	    v = transaction->insert( QString( "INSERT INTO trace_point VALUES(NULL, %1, %2, %3, %4, %5);" ).arg( type ).arg( pathId ).arg( lineno ).arg( functionId ).arg( groupId ) );
	}
	bool ok;
	unsigned int tracepointId = v.toUInt( &ok );
	if ( !ok ) {
	    throw runtime_error( "Failed to store entry in database: read non-numeric tracepoint id from database - corrupt database?" );
	}
	cache( key, tracepointId );
	return tracepointId;
    }
} tracePointCache;

struct TraceEntryTuple
{
    unsigned int threadId;
    QDateTime timestamp;
    int pointId;
    QString message;
    unsigned long stackPosition;
};

static unsigned int storeTraceEntry( QSqlDatabase db, Transaction *transaction,
				     unsigned int threadId,
				     const QDateTime &timestamp,
				     unsigned int pointId,
				     const QString &message,
				     unsigned long stackPosition )
{
    return transaction->insert( QString( "INSERT INTO trace_entry VALUES(NULL, %1, %2, %3, %4, %5)" )
                    .arg( threadId )
                    .arg( Database::formatValue( db, timestamp ) )
                    .arg( pointId )
                    .arg( Database::formatValue( db, message ) )
                    .arg( stackPosition ) ).toUInt();
}


static void storeVariables( QSqlDatabase db, Transaction *transaction,
			    unsigned int traceentryId,
			    const QList<Variable> &variables )
{
    QList<Variable>::ConstIterator it, end = variables.end();
    for ( it = variables.begin(); it != end; ++it ) {
	transaction->exec( QString( "INSERT INTO variable VALUES(%1, %2, %3, %4);" ).arg( traceentryId ).arg( Database::formatValue( db, it->name ) ).arg( Database::formatValue( db, it->value ) ).arg( it->type ) );
    }
}

static void storeBacktrace( QSqlDatabase db, Transaction *transaction,
			    unsigned int traceentryId,
			    const QList<StackFrame> &backtrace )
	
{
    unsigned int depthCount = 0;
    QList<StackFrame>::ConstIterator it, end = backtrace.end();
    for ( it = backtrace.begin(); it != end; ++it, ++depthCount ) {
	transaction->exec( QString( "INSERT INTO stackframe VALUES(%1, %2, %3, %4, %5, %6, %7);" ).arg( traceentryId ).arg( depthCount ).arg( Database::formatValue( db, it->module ) ).arg( Database::formatValue( db, it->function ) ).arg( it->functionOffset ).arg( Database::formatValue( db, it->sourceFile ) ).arg( it->lineNumber ) );
    }
}

static void storeEntry( QSqlDatabase db, Transaction *transaction, const TraceEntry &e )
{
    unsigned int pathId = pathCache.store( db, transaction, e.path );
    unsigned int functionId = functionCache.store( db, transaction, e.function );
    unsigned int processId = processCache.store( db, transaction, e.processName,
						 e.pid, e.processStartTime );
    unsigned int threadId = threadCache.store( db, transaction, processId, e.tid );
    unsigned int groupId = storeGroup( db, transaction,
				       e.groupName,
				       e.traceKeys );
    unsigned int tracepointId = tracePointCache.store( db, transaction,
						       e.type, pathId, e.lineno,
						       functionId, groupId );
    unsigned int traceentryId = storeTraceEntry( db, transaction,
						 threadId,
						 e.timestamp,
						 tracepointId,
						 e.message,
						 e.stackPosition );
    storeVariables( db, transaction, traceentryId, e.variables );
    storeBacktrace( db, transaction, traceentryId, e.backtrace );
}

static QString archiveFileName( const QString &archiveDirName, const QString &currentFileName )
{
    const QDir archiveDir( archiveDirName );

    const QStringList entries = archiveDir.entryList( QStringList()
                                                      << "*-" + QFileInfo( currentFileName ).fileName() );
    return QString( "%1/%2-%3" )
        .arg( archiveDirName )
        .arg( entries.size() + 1 )
        .arg( QFileInfo( currentFileName ).fileName() );
}

static void archiveEntries( QSqlDatabase db, unsigned short percentage, const QString &archiveDir )
{
    if ( percentage == 0 ) {
        return;
    }

    if ( percentage > 100 ) {
        percentage = 100;
    }

    Transaction transaction( db );
    qulonglong numCopy = 0;
    {
        QVariant v = transaction.exec( QString( "SELECT ROUND(COUNT(id) / 100.0 * %1) FROM trace_entry;" ).arg( percentage ) );
        bool ok;
        numCopy = v.toULongLong( &ok );
        if ( !ok ) {
            throw runtime_error( "Failed to count number of entries to archive" );
        }
    }

    QString connName;
    {
        QSqlDatabase archiveDB;
        {
            if ( !QDir().mkpath( archiveDir ) ) {
                throw runtime_error( QString( "Failed to create archive database: creating archive directory %1 failed" ).arg( archiveDir ).toUtf8().constData() );
            }

            const QString fn = archiveFileName( archiveDir, db.databaseName() );
            QString errorMsg;
            archiveDB = Database::create( fn, &errorMsg );
            if ( !archiveDB.isValid() ) {
                throw runtime_error( QString( "Failed to create database in %1: %2" ).arg( fn ).arg( errorMsg ).toUtf8().constData() );
            }
        }
        connName = archiveDB.connectionName();

        {
            QString query = QString( "SELECT"
                            " trace_entry.id,"
                            " process.pid,"
                            " process.start_time,"
                            " process.name,"
                            " traced_thread.tid,"
                            " trace_entry.timestamp,"
                            " trace_point.type,"
                            " path_name.name,"
                            " trace_point.line,"
                            " trace_point.group_id,"
                            " function_name.name,"
                            " trace_entry.message, "
                            " trace_entry.stack_position "
                            "FROM"
                            " trace_entry,"
                            " trace_point,"
                            " path_name,"
                            " function_name,"
                            " process,"
                            " traced_thread "
                            "WHERE"
                            " trace_entry.trace_point_id = trace_point.id "
                            "AND"
                            " trace_point.function_id = function_name.id "
                            "AND"
                            " trace_point.path_id = path_name.id "
                            "AND"
                            " trace_entry.traced_thread_id = traced_thread.id "
                            "AND"
                            " traced_thread.process_id = process.id "
                            "ORDER BY"
                            " trace_entry.id "
                            "LIMIT"
                            " %1" ).arg( numCopy );

            QSqlQuery q( db );
            q.setForwardOnly( true );
            if ( !q.exec( query ) ) {
                throw runtime_error( QString( "Cannot archive trace data: failed to extract entry data: %1" ).arg( q.lastError().text() ).toUtf8().constData() );
            }

            Transaction archiveTransaction( archiveDB );
            while ( q.next() ) {
                qulonglong id = q.value( 0 ).toULongLong();

                TraceEntry e;
                e.pid = q.value( 1 ).toUInt();
                e.processStartTime = QDateTime::fromMSecsSinceEpoch( q.value( 2 ).toLongLong() );
                e.processName = q.value( 3 ).toString();
                e.tid = q.value( 4 ).toUInt();
                e.timestamp = QDateTime::fromMSecsSinceEpoch( q.value( 5 ).toLongLong() );
                e.type = q.value( 6 ).toUInt();
                e.path = q.value( 7 ).toString();
                e.lineno = q.value( 8 ).toULongLong();

                const int groupId = q.value( 9 ).toInt();
                if ( groupId != 0 ) {
                    QSqlQuery gq( db );
                    gq.setForwardOnly( true );
                    if ( gq.exec( QString( "SELECT name FROM trace_point_group WHERE id = %1" ).arg( groupId ) ) ) {
                        if ( gq.next() ) {
                            e.groupName = gq.value( 0 ).toString();
                        }
                    }
                }

                e.function = q.value( 10 ).toString();
                e.message = q.value( 11 ).toString();
                e.stackPosition = q.value( 12 ).toULongLong();
                e.backtrace = Database::backtraceForEntry( db, id );

                {
                    QSqlQuery vq( db );
                    vq.setForwardOnly( true );
                    if ( vq.exec( QString( "SELECT "
                                    " name,"
                                    " type,"
                                    " value "
                                    "FROM "
                                    " variable "
                                    "WHERE"
                                    " trace_entry_id = %1" ).arg( id ) ) ) {
                        while ( vq.next() ) {
                            Variable v;
                            v.name = vq.value( 0 ).toString();
                            v.type = (TRACELIB_NAMESPACE_IDENT(VariableType)::Value)( vq.value( 1 ).toInt() );
                            v.value = vq.value( 2 ).toString();
                            e.variables.append( v );
                        }
                    }
                }
                ::storeEntry( archiveDB, &archiveTransaction, e );
            }
        }
    }

    {
        transaction.exec( QString( "DELETE FROM trace_entry WHERE id IN (SELECT id FROM trace_entry ORDER BY id LIMIT %1);" ).arg( numCopy ) );

        transaction.exec( QString( "DELETE FROM trace_point WHERE id NOT IN (SELECT trace_point_id FROM trace_entry);" ) );
        tracePointCache.clear();

        transaction.exec( QString( "DELETE FROM function_name WHERE id NOT IN (SELECT function_id FROM trace_point);" ) );
        functionCache.clear();

        transaction.exec( QString( "DELETE FROM path_name WHERE id NOT IN (SELECT path_id FROM trace_point);" ) );
        pathCache.clear();

        transaction.exec( QString( "DELETE FROM trace_point_group WHERE id NOT IN (SELECT group_id FROM trace_point);" ) );
        traceKeyCache.clear();

        transaction.exec( QString( "DELETE FROM traced_thread WHERE id NOT IN (SELECT traced_thread_id FROM trace_entry);" ) );
        threadCache.clear();

        transaction.exec( QString( "DELETE FROM process WHERE id NOT IN (SELECT process_id FROM traced_thread);" ) );
        processCache.clear();

        transaction.exec( QString( "DELETE FROM variable WHERE trace_entry_id NOT IN (SELECT id FROM trace_entry);" ) );
        transaction.exec( QString( "DELETE FROM stackframe WHERE trace_entry_id NOT IN (SELECT id FROM trace_entry);" ) );
    }
    QSqlDatabase::removeDatabase( connName );
}

ClientSocket::ClientSocket( QObject *parent )
    : QTcpSocket( parent )
{
    connect( this, SIGNAL( readyRead() ),
             this, SLOT( handleIncomingData() ) );
}

void ClientSocket::handleIncomingData()
{
    const QByteArray data = readAll();
    assert( !data.isEmpty() );
    emit dataReceived( data );
}

NetworkingThread::NetworkingThread( int socketDescriptor, QObject *parent )
    : QThread( parent ),
    m_socketDescriptor( socketDescriptor ),
    m_clientSocket( 0 )
{
}

void NetworkingThread::run()
{
    m_clientSocket = new ClientSocket;
    m_clientSocket->setSocketDescriptor( m_socketDescriptor );
    connect( m_clientSocket, SIGNAL( dataReceived( const QByteArray & ) ),
             this, SIGNAL( dataReceived( const QByteArray & ) ),
             Qt::QueuedConnection );
    connect( m_clientSocket, SIGNAL( disconnected() ),
             this, SLOT( quit() ),
             Qt::QueuedConnection  );
    exec();
    delete m_clientSocket;
}

ServerSocket::ServerSocket( Server *server )
    : QTcpServer( server ),
    m_server( server )
{
}

ServerSocket::~ServerSocket()
{
    QList<NetworkingThread *>::Iterator it, end = m_networkingThreads.end();
    for ( it = m_networkingThreads.begin(); it != end; ++it ) {
        ( *it )->quit();
    }

    for ( it = m_networkingThreads.begin(); it != end; ++it ) {
        ( *it )->wait();
    }
}

void ServerSocket::incomingConnection( int socketDescriptor )
{
    NetworkingThread *thread = new NetworkingThread( socketDescriptor,
                                                     this );
    m_networkingThreads.push_back( thread );
    connect( thread, SIGNAL( dataReceived( const QByteArray & ) ),
             m_server, SLOT( handleIncomingData( const QByteArray & ) ) );
    connect( thread, SIGNAL( finished() ),
             thread, SLOT( deleteLater() ) );
    thread->start();
}

GUIConnection::GUIConnection( Server *server, QTcpSocket *sock )
    : QObject( server ),
    m_server( server ),
    m_sock( sock )
{
    connect( m_sock, SIGNAL( readyRead() ), SLOT( handleIncomingData() ) );
    connect( m_sock, SIGNAL( disconnected() ), SLOT( handleDisconnect() ) );
}

void GUIConnection::write( const QByteArray &data )
{
    m_sock->write( data );
}

// Mostly duplicated in gui/mainwindow.cpp (ServerSocket::handleIncomingData)
void GUIConnection::handleIncomingData()
{
    QDataStream stream(m_sock);
    stream.setVersion(QDataStream::Qt_4_0);

    while (true) {
        static quint16 nextPayloadSize = 0;
        if (nextPayloadSize == 0) {
            if (m_sock->bytesAvailable() < sizeof(nextPayloadSize)) {
                return;
            }
            stream >> nextPayloadSize;
        }

        if (m_sock->bytesAvailable() < nextPayloadSize) {
            return;
        }

        quint32 magicCookie;
        stream >> magicCookie;
        if (magicCookie != MagicServerProtocolCookie) {
            m_sock->disconnectFromHost();
            return;
        }

        quint32 protocolVersion;
        stream >> protocolVersion;
        assert(protocolVersion == 1);

        quint8 datagramType;
        stream >> datagramType;
        switch (static_cast<ServerDatagramType>(datagramType)) {
            case DatabaseNukeDatagram:
                emit databaseNukeRequested();
                break;
        }
        nextPayloadSize = 0;
    }
}

void GUIConnection::handleDisconnect()
{
    emit disconnected( this );
    m_sock->deleteLater();
    delete this;
}

Server::Server( const QString &traceFile,
                QSqlDatabase database,
                unsigned short port, unsigned short guiPort,
                QObject *parent )
    : QObject( parent ),
      m_tcpServer( 0 ),
      m_db( database ),
      m_xmlHandler( this ),
      m_shrinkBy( 0 ),
      m_maximumSize( StorageConfiguration::UnlimitedTraceSize )
{
    QFileInfo fi( traceFile );
    m_traceFile = QDir::toNativeSeparators( fi.canonicalFilePath() );

    assert( m_db.isValid() );
    m_db.exec( "PRAGMA synchronous=OFF;");

    m_tcpServer = new ServerSocket( this );
    m_tcpServer->listen( QHostAddress::Any, port );

    m_guiServer = new QTcpServer( this );
    connect( m_guiServer, SIGNAL( newConnection() ), SLOT( handleNewGUIConnection() ) );
    m_guiServer->listen( QHostAddress::LocalHost, guiPort );

    m_xmlHandler.addData( "<toplevel_trace_element>" );
}

// duplicated in gui/mainwindow.cpp
template <typename DatagramType, typename ValueType>
QByteArray serializeDatagram( DatagramType type, const ValueType *v )
{
    QByteArray payload;
    {
        static const quint32 ProtocolVersion = 1;

        QDataStream stream( &payload, QIODevice::WriteOnly );
        stream.setVersion( QDataStream::Qt_4_0 );
        stream << MagicServerProtocolCookie << ProtocolVersion << (quint8)type;
        if ( v ) {
            stream << *v;
        }
    }

    QByteArray data;
    {
        QDataStream stream( &data, QIODevice::WriteOnly );
        stream.setVersion( QDataStream::Qt_4_0 );
        stream << (quint16)payload.size();
        data.append( payload );
    }

    return data;
}

QByteArray serializeGUIClientData( ServerDatagramType type ) {
    return serializeDatagram( type, (int *)0 );
}

template <typename T>
QByteArray serializeGUIClientData( ServerDatagramType type, const T &v ) {
    return serializeDatagram( type, &v );
}

void Server::handleTraceEntry( const TraceEntry &entry )
{
    storeEntry( entry );

    QByteArray serializedEntry = serializeGUIClientData( TraceEntryDatagram, entry );

    QList<GUIConnection *>::Iterator it, end = m_guiConnections.end();
    for ( it = m_guiConnections.begin(); it != end; ++it ) {
        ( *it )->write( serializedEntry );
    }

    emit traceEntryReceived( entry );
}

void Server::handleShutdownEvent( const ProcessShutdownEvent &ev )
{
    storeShutdownEvent( ev );

    QByteArray serializedEvent = serializeGUIClientData( ProcessShutdownEventDatagram, ev );

    QList<GUIConnection *>::Iterator it, end = m_guiConnections.end();
    for ( it = m_guiConnections.begin(); it != end; ++it ) {
        ( *it )->write( serializedEvent );
    }

    emit processShutdown( ev );
}

template <typename T>
T clamp( T v, T lowerBound, T upperBound ) {
    if ( v < lowerBound ) return lowerBound;
    if ( v > upperBound ) return upperBound;
    return v;
}

void Server::applyStorageConfiguration( const StorageConfiguration &cfg )
{
    const unsigned short shrinkBy = clamp<unsigned short>( cfg.shrinkBy, 1, 100 );
    if ( m_maximumSize == cfg.maximumSize &&
         m_shrinkBy == shrinkBy &&
         m_archiveDir == cfg.archiveDir ) {
        return;
    }

    if ( cfg.maximumSize == StorageConfiguration::UnlimitedTraceSize ) {
        /* XXX Don't hardcode this default value, might change if sqlite3 was
         * compiled with different settings.
         */
        m_db.exec( "PRAGMA max_page_count=1073741823" );
        m_maximumSize = cfg.maximumSize;
        m_shrinkBy = shrinkBy;
        m_archiveDir = cfg.archiveDir;
        return;
    }

    qulonglong pageSize = 0;
    {
        QSqlQuery q = m_db.exec( "PRAGMA page_size;" );
        if ( !q.next() ) {
            return;
        }

        bool ok;
        pageSize = q.value( 0 ).toULongLong( &ok );
        if ( !ok || pageSize == 0 ) {
            return;
        }
    }

    qulonglong pageCount = 0;
    {
        QSqlQuery q = m_db.exec( "PRAGMA page_count;" );
        if ( !q.next() ) {
            return;
        }

        bool ok;
        pageCount = q.value( 0 ).toULongLong( &ok );
        if ( !ok ) {
            return;
        }
    }

    /* It's possible that the current file is larger than the given
     * maximum size. In that case, lets just use the current size as
     * the maximum to avoid that it grows even further. We cannot shrink
     * existing files, so this is pretty much the best we can do.
     */
    qulonglong maxPageCount = cfg.maximumSize / pageSize;
    if ( pageCount > maxPageCount ) {
        maxPageCount = pageCount;
    }

    m_db.exec( QString( "PRAGMA max_page_count=%1" ).arg( maxPageCount ) );

    m_maximumSize = cfg.maximumSize;
    m_shrinkBy = shrinkBy;
    m_archiveDir = cfg.archiveDir;
}

void Server::handleIncomingData( const QByteArray &xmlData )
{
    try {
        m_xmlHandler.addData( xmlData );
        m_xmlHandler.continueParsing();
    } catch ( const runtime_error &e ) {
        qWarning() << e.what();
    }
}

// Definition taken from http://www.sqlite.org/c_interface.html
#define SQLITE_FULL        13   /* Insertion failed because database is full */

void Server::storeEntry( const TraceEntry &e )
{
    try {
        Transaction transaction( m_db );
        ::storeEntry( m_db, &transaction, e );
    } catch ( const SQLTransactionException &ex ) {
        if ( ex.driverCode() == SQLITE_FULL ) {
            archiveEntries( m_db, m_shrinkBy, m_archiveDir );

            QByteArray serializedEntry = serializeGUIClientData( DatabaseNukeFinishedDatagram );

            QList<GUIConnection *>::Iterator it, end = m_guiConnections.end();
            for ( it = m_guiConnections.begin(); it != end; ++it ) {
                ( *it )->write( serializedEntry );
            }

            storeEntry( e );
        } else {
            throw;
        }
    }
}

void Server::storeShutdownEvent( const ProcessShutdownEvent &ev )
{
    Transaction transaction( m_db );
    transaction.exec( QString( "UPDATE process SET end_time=%1 WHERE pid=%2 AND start_time=%3;" ).arg( Database::formatValue( m_db, ev.stopTime ) ).arg( ev.pid ).arg( Database::formatValue( m_db, ev.startTime ) ) );
}

void Server::handleNewGUIConnection()
{
    GUIConnection *c = new GUIConnection( this, m_guiServer->nextPendingConnection() );
    connect( c, SIGNAL( databaseNukeRequested() ), SLOT( nukeDatabase() ) );
    connect( c, SIGNAL( disconnected( GUIConnection * ) ),
             SLOT( guiDisconnected( GUIConnection * ) ) );
    m_guiConnections.append( c );
    c->write( serializeGUIClientData( TraceFileNameDatagram, m_traceFile ) );
}

void Server::guiDisconnected( GUIConnection *c )
{
    m_guiConnections.removeAll( c );
}

void Server::nukeDatabase()
{
    Database::trimTo( m_db, 0 );

    QByteArray serializedEntry = serializeGUIClientData( DatabaseNukeFinishedDatagram );

    QList<GUIConnection *>::Iterator it, end = m_guiConnections.end();
    for ( it = m_guiConnections.begin(); it != end; ++it ) {
        ( *it )->write( serializedEntry );
    }
}

