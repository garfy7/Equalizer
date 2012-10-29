// -*- mode: c++ -*-
/* Copyright (c) 2012, Computer Integration & Programming Solutions, Corp. and
 *                     United States Naval Research Laboratory
 *               2012, Stefan Eilemann <eile@eyescale.ch>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "rdmaConnection.h"

#include "connectionType.h"
#include "connectionDescription.h"
#include "global.h"
#include "eventConnection.h"

#include <co/base/clock.h>
#include <co/base/sleep.h>

#ifdef _WIN32
#pragma warning( disable : 4018 )
#include <boost/interprocess/mapped_region.hpp>
#pragma warning( default : 4018 )
#else
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <poll.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <stddef.h>
#include <unistd.h>

#include <rdma/rdma_verbs.h>

#define IPV6_DEFAULT 0

#define RDMA_PROTOCOL_MAGIC     0xC0
#define RDMA_PROTOCOL_VERSION   0x03

namespace co
{
//namespace { static const uint64_t ONE = 1ULL; }

/**
 * Message types
 */
enum OpCode
{
    SETUP = 1 << 0,
    FC    = 1 << 1,
};

/**
 * Initial setup message used to exchange sink MR parameters.
 */
struct RDMASetupPayload
{
    uint64_t rbase;
    uint64_t rlen;
    uint64_t rkey;
};

/**
 * "ACK" messages sent after read, tells source about receive progress.
 */
struct RDMAFCPayload
{
    uint32_t bytes_received;
    uint32_t writes_received;
};

/**
 * Payload wrapper
 */
struct RDMAMessage
{
    enum OpCode opcode;
    uint8_t length;
    union
    {
        struct RDMASetupPayload setup;
        struct RDMAFCPayload fc;
    } payload;
};

/**
 * "IMM" data sent with RDMA write, tells sink about send progress.
 */
struct RDMAFCImm
{
    uint32_t bytes_sent:28;
    uint32_t fcs_received:4;
};

namespace {
// We send a max of 28 bits worth of byte counts per RDMA write.
static const uint64_t MAX_BS = (( 2 << ( 28 - 1 )) - 1 );
// We send a max of four bits worth of fc counts per RDMA write.
static const uint16_t MAX_FC = (( 2 << ( 4 - 1 )) - 1 );

static const uint32_t MASK_BUF_EVENT = 0x00000001;
static const uint32_t MASK_CQ_EVENT = 0x00000002;
static const uint32_t MASK_CM_EVENT = 0x00000004;

static const uint32_t RINGBUFFER_ALLOC_RETRIES = 8;
static const uint32_t WINDOWS_CONNECTION_BACKLOG = 1024;
}

/**
 * An RDMA connection implementation.
 *
 * The protocol is simple, e.g.:
 *
 *      initiator                        target
 * -----------------------------------------------------
 *                                  resolve/bind/listen
 * resolve/prepost/connect
 *                                    prepost/accept
 *     send setup         <------->    send setup
 *   wait for setup                   wait for setup
 * RDMA_WRITE_WITH_IMM WR  -------> RDMA_WRITE(DATA) WC
 *     RECV(FC) WC        <-------      SEND WR
 *                            .
 *                            .
 *                            .
 *
 * The setup phase exchanges the MR parameters of a fixed size circular buffer
 * to which remote writes are sent.  Sender tracks available space on the
 * receiver by accepting "Flow Control" messages that update the tail pointer
 * of the local "view" of the remote sink MR.
 *
 * Once setup is complete, either side may begin operations on the other's MR
 * (the initiator doesn't have to send first, as in the above example).
 *
 * If either credits or buffer space are exhausted, sender will spin waiting
 * for flow control messages.  Receiver will also not send flow control if
 * there are no credits available.
 *
 * One catch is that Collage will only monitor a single "notifier" for events
 * and we have three that need to be monitored: one for connection status
 * events (the RDMA event channel) - RDMA_CM_EVENT_DISCONNECTED in particular,
 * one for the receive completion queue (upon incoming RDMA write), and an
 * additional eventfd(2) used to keep the notifier "hot" after partial reads.
 * We leverage the feature of epoll(7) in that "If an epoll file descriptor
 * has events waiting then it will indicate as being readable".
 *
 * Quite interesting is the effect of RDMA_RING_BUFFER_SIZE_MB and
 * RDMA_SEND_QUEUE_DEPTH depending on the communication pattern.  Basically,
 * bigger doesn't necessarily equate to faster!  The defaults are suited for
 * low latency conditions and would need tuning otherwise.
 *
 * ib_write_bw
 * -----------
 *  #bytes    num iterations  BW peak[MB/sec]    BW average[MB/sec]
 * 1048576    10000           3248.10            3247.94
 *
 * netperf
 * -------
 * Send perf: 3240.72MB/s (3240.72pps)
 * Send perf: 3240.72MB/s (3240.72pps)
 * Send perf: 3240.95MB/s (3240.95pps)
 */
RDMAConnection::RDMAConnection( )
#ifdef _WIN32
    : _event( new EventConnection )
#else
    : _notifier( -1 )
#endif
    , _timeout( Global::getIAttribute( Global::IATTR_RDMA_RESOLVE_TIMEOUT_MS ))
    , _rai( NULL )
    , _cm( NULL )
    , _cm_id( NULL )
    , _new_cm_id( NULL )
    , _cc( NULL )
    , _cq( NULL )
    , _pd( NULL )
    , _wcs ( 0 )
    , _established( false )
    , _depth( 0L )
    , _writes( 0L )
    , _fcs( 0L )
    , _wcredits( 0L )
    , _fcredits( 0L )
    , _completions( 0U )
    , _msgbuf( sizeof(RDMAMessage) )
    , _sourcebuf( 0 )
    , _sourceptr( 0 )
    , _sinkbuf( IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE )
    , _sinkptr( 0 )
    , _rptr( 0UL )
    , _rbase( 0ULL )
    , _rkey( 0ULL )
#ifdef _WIN32
    , _availBytes( 0 )
    , _eventFlag( 0 )
    , _cmWaitObj( 0 )
    , _ccWaitObj( 0 )
#endif
{
#ifndef _WIN32
    _pipe_fd[0] = -1;
    _pipe_fd[1] = -1;
#endif
    
    ::memset( (void *)&_addr, 0, sizeof(_addr) );
    ::memset( (void *)&_serv, 0, sizeof(_serv) );

    ::memset( (void *)&_cpd, 0, sizeof(struct RDMAConnParamData) );

    _description->type = CONNECTIONTYPE_RDMA;
    _description->bandwidth = // QDR default, report "actual" 8b/10b bandwidth
        ( ::ibv_rate_to_mult( IBV_RATE_40_GBPS ) * 2.5 * 1024000 / 8 ) * 0.8;
}

bool RDMAConnection::connect( )
{
    EQASSERT( CONNECTIONTYPE_RDMA == _description->type );

    if( STATE_CLOSED != _state )
        return false;

    if( 0u == _description->port )
        return false;

    _cleanup( );

    setState( STATE_CONNECTING );

    if( !_lookupAddress( false ) || ( NULL == _rai ))
    {
        EQERROR << "Failed to lookup destination address." << std::endl;
        goto err;
    }

    _updateInfo( _rai->ai_dst_addr );

    if( !_createNotifier( ))
    {
        EQERROR << "Failed to create master notifier." << std::endl;
        goto err;
    }

    if( !_createEventChannel( ))
    {
        EQERROR << "Failed to create communication event channel." << std::endl;
        goto err;
    }

    if( !_createId( ))
    {
        EQERROR << "Failed to create communication identifier." << std::endl;
        goto err;
    }

    if( !_resolveAddress( ))
    {
        EQERROR << "Failed to resolve destination address for : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    _updateInfo( ::rdma_get_peer_addr( _cm_id ));

    _device_name = ::ibv_get_device_name( _cm_id->verbs->device );

    EQVERB << "Initiating connection on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " to "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    if( !_initProtocol( Global::getIAttribute(
            Global::IATTR_RDMA_SEND_QUEUE_DEPTH )))
    {
        EQERROR << "Failed to initialize protocol variables." << std::endl;
        goto err;
    }

    if( !_initVerbs( ))
    {
        EQERROR << "Failed to initialize verbs." << std::endl;
        goto err;
    }

    if( !_createQP( ))
    {
        EQERROR << "Failed to create queue pair." << std::endl;
        goto err;
    }

    if( !_createBytesAvailableFD( ))
    {
        EQERROR << "Failed to create available byte notifier." << std::endl;
        goto err;
    }

    if( !_initBuffers( ))
    {
        EQERROR << "Failed to initialize ring buffers." << std::endl;
        goto err;
    }

    if( !_resolveRoute( ))
    {
        EQERROR << "Failed to resolve route to destination : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    if( !_postReceives( _depth ))
    {
        EQERROR << "Failed to pre-post receives." << std::endl;
        goto err;
    }

    if( !_connect( ))
    {
        EQERROR << "Failed to connect to destination : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    EQASSERT( _established );

    if(( RDMA_PROTOCOL_MAGIC != _cpd.magic ) ||
        ( RDMA_PROTOCOL_VERSION != _cpd.version ))
    {
        EQERROR << "Protocol mismatch with target : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    if( !_postSetup( ))
    {
        EQERROR << "Failed to post setup message." << std::endl;
        goto err;
    }

    if( !_waitRecvSetup( ))
    {
        EQERROR << "Failed to receive setup message." << std::endl;
        goto err;
    }

    EQVERB << "Connection established on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " to "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    setState( STATE_CONNECTED );

    return true;

err:
    EQVERB << "Connection failed on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " to "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    close( );

    return false;
}

bool RDMAConnection::listen( )
{
    EQASSERT( CONNECTIONTYPE_RDMA == _description->type );

    if( STATE_CLOSED != _state )
        return false;

    _cleanup( );

    setState( STATE_CONNECTING );

    if( !_lookupAddress( true ))
    {
        EQERROR << "Failed to lookup local address." << std::endl;
        goto err;
    }

    if( NULL != _rai )
        _updateInfo( _rai->ai_src_addr );

    if( !_createNotifier( ))
    {
        EQERROR << "Failed to create master notifier." << std::endl;
        goto err;
    }

    if( !_createEventChannel( ))
    {
        EQERROR << "Failed to create communication event channel." << std::endl;
        goto err;
    }

    if( !_createId( ))
    {
        EQERROR << "Failed to create communication identifier." << std::endl;
        goto err;
    }

#if 0
    /* NOT IMPLEMENTED */

    if( ::rdma_set_option( _cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_REUSEADDR,
            (void *)&ONE, sizeof(ONE) ))
    {
        EQERROR << "rdma_set_option : " << co::base::sysError << std::endl;
        goto err;
    }
#endif

    if( !_bindAddress( ))
    {
        EQERROR << "Failed to bind to local address : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    _updateInfo( ::rdma_get_local_addr( _cm_id ));

#ifdef _WIN32
    if( !_listen( WINDOWS_CONNECTION_BACKLOG ))
#else
    if( !_listen( SOMAXCONN ))
#endif
    {
        EQERROR << "Failed to listen on bound address : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    if( NULL != _cm_id->verbs )
        _device_name = ::ibv_get_device_name( _cm_id->verbs->device );

    EQINFO << "Listening on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " at "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    setState( STATE_LISTENING );

    return true;

err:
    close( );

    return false;
}

void RDMAConnection::acceptNB( ) { /* NOP */ }

ConnectionPtr RDMAConnection::acceptSync( )
{
    RDMAConnection *newConnection = NULL;

    if( STATE_LISTENING != _state )
    {
        EQERROR << "Connection not in listening state." << std::endl;
        goto out;
    }

    if( !_waitForCMEvent( RDMA_CM_EVENT_CONNECT_REQUEST ))
    {
        EQERROR << "Failed to receive valid connect request." << std::endl;
        goto out;
    }

    EQASSERT( NULL != _new_cm_id );

    newConnection = new RDMAConnection( );

    if( !newConnection->_finishAccept( _new_cm_id, _cpd ))
    {
        delete newConnection;
        newConnection = NULL;
    }

out:
    _new_cm_id = NULL;
#ifdef _WIN32    
    _event->reset();
#else
    eventset events;
    _checkEvents( events );
#endif

    return newConnection;
}

void RDMAConnection::readNB( void* buffer, const uint64_t bytes ) { /* NOP */ }

int64_t RDMAConnection::readSync( void* buffer, const uint64_t bytes,
        const bool block )
{
    co::base::Clock clock;
    const int64_t start = clock.getTime64( );
    const uint32_t timeout = Global::getTimeout( );
    eventset events;
    uint64_t available_bytes = 0ULL;
    uint32_t bytes_taken = 0UL;
    bool extra_event = false;

    if( STATE_CONNECTED != _state )
        goto err;

//    EQWARN << (void *)this << std::dec << ".read(" << bytes << ")"
//       << std::endl;

    _stats.reads++;

retry:
    if( !_checkDisconnected( events ))
    {
        EQERROR << "Error while checking event state." << std::endl;
        goto err;
    }

    if( events.test( CQ_EVENT ))
    {
        if( !_rearmCQ( ))
        {
            EQERROR << "Error while rearming receive channel." << std::endl;
            goto err;
        }
    }

    // Modifies _sourceptr.TAIL, _sinkptr.HEAD & _rptr.TAIL
    if( !_checkCQ( events.test( CQ_EVENT )))
    {
        EQERROR << "Error while polling completion queues." << std::endl;
        goto err;
    }

    EQASSERT( _fcredits >= 0L );

    if( _established && _needFC( ) && ( 0L == _fcredits ))
    {
        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out trying to acquire credit." << std::endl;
                goto err;
            }
        }

        //EQWARN << "No credit for flow control." << std::endl;
        co::base::Thread::yield( );
        _stats.no_credits_fc++;
        goto retry;
    }

    // "Note that an extra event may be triggered without having a
    // corresponding completion entry in the CQ." (per ibv_get_cq_event(3))
    if( _established && !events.test( BUF_EVENT ))
    {
        // Special case: If LocalNode is reading the length part of a message
        // it will ignore this zero return and restart the select.
        if( extra_event && !block )
            return 0LL;

        extra_event = true;
        goto retry;
    }

    if( events.test( BUF_EVENT ))
    {
        available_bytes = _getAvailableBytes( );
        if( 0ULL == available_bytes )
        {
            EQERROR << "Error while reading from event fd." << std::endl;
            goto err;
        }
    }

    // Modifies _sinkptr.TAIL
    bytes_taken = _drain( buffer, bytes );

    if( 0UL == bytes_taken )
    {
        if( _sinkptr.isEmpty( ) && !_established )
        {
            EQINFO << "Got EOF, closing " << getDescription( )->toString( )
                << std::endl;
            goto err;
        }

        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out trying to drain buffer." << std::endl;
                goto err;
            }
        }

        //EQWARN << "Sink buffer empty." << std::endl;
        co::base::Thread::yield( );
        _stats.buffer_empty++;
        goto retry;
    }

    // Put back what wasn't taken (ensure the master notifier stays "hot").
    if( available_bytes > bytes_taken )
        _incrAvailableBytes( available_bytes - bytes_taken );
#ifdef _WIN32
    else
    {
        HANDLE lpHandles[2];
        lpHandles[0] = _cc->comp_channel.Event;
        lpHandles[1] = _cm->channel.Event;
        if ( WaitForMultipleObjects( 2, lpHandles, false, 0 ) == WAIT_TIMEOUT )
            _event->reset();
    }
#endif

    if( _established && _needFC( ) && !_postFC( bytes_taken ))
        EQWARN << "Error while posting flow control message." << std::endl;

//    EQWARN << (void *)this << std::dec << ".read(" << bytes << ")"
//       << " took " << bytes_taken << " bytes"
//       << " (" << _sinkptr.available( ) << " still available)" << std::endl;

    return bytes_taken;

err:
    close( );

    return -1LL;
}

int64_t RDMAConnection::write( const void* buffer, const uint64_t bytes )
{
    co::base::Clock clock;
    const int64_t start = clock.getTime64( );
    const uint32_t timeout = Global::getTimeout( );
    eventset events;
    const uint32_t can_put = std::min( bytes, MAX_BS );
    uint32_t bytes_put;

    if( STATE_CONNECTED != _state )
        goto err;

//    EQWARN << (void *)this << std::dec << ".write(" << bytes << ")"
//        << std::endl;

    _stats.writes++;

retry:
    if( !_checkDisconnected( events ))
    {
        EQERROR << "Error while checking connection state." << std::endl;
        goto err;
    }

    if( !_established )
    {
        EQWARN << "Disconnected in write." << std::endl;
        goto err;
    }

    // Modifies _sourceptr.TAIL, _sinkptr.HEAD & _rptr.TAIL
    if( !_checkCQ( false ))
    {
        EQERROR << "Error while polling completion queues." << std::endl;
        goto err;
    }

    EQASSERT( _wcredits >= 0L );

    if( 0L == _wcredits )
    {
        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out trying to acquire credit." << std::endl;
                goto err;
            }
        }

        //EQWARN << "No credits for RDMA." << std::endl;
        co::base::Thread::yield( );
        _stats.no_credits_rdma++;
        goto retry;
    }

    // Modifies _sourceptr.HEAD
    bytes_put = _fill( buffer, can_put );

    if( 0UL == bytes_put )
    {
        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out trying to fill buffer." << std::endl;
                goto err;
            }
        }

        //EQWARN << "Source buffer full." << std::endl;
        co::base::Thread::yield( );
        if( _sourceptr.isFull( ) || _rptr.isFull( ))
            _stats.buffer_full++;
        goto retry;
    }

    // Modifies _sourceptr.MIDDLE & _rptr.HEAD
    if( !_postRDMAWrite( ))
    {
        EQERROR << "Error while posting RDMA write." << std::endl;
        goto err;
    }

//    EQWARN << (void *)this << std::dec << ".write(" << bytes << ")"
//       << " put " << bytes_put << " bytes" << std::endl;

    return bytes_put;

err:
    return -1LL;
}

RDMAConnection::~RDMAConnection( )
{
    _close( );
}

void RDMAConnection::setState( const State state )
{
    if( state != _state )
    {
        _state = state;
        _fireStateChanged( );
    }
}

////////////////////////////////////////////////////////////////////////////////

void RDMAConnection::_close( )
{
    co::base::ScopedWrite close_mutex( _poll_lock );

    if( STATE_CLOSED != _state )
    {
        EQASSERT( STATE_CLOSING != _state );
        setState( STATE_CLOSING );

        if( _established && ::rdma_disconnect( _cm_id ))
            EQWARN << "rdma_disconnect : " << co::base::sysError << std::endl;

        setState( STATE_CLOSED );

        _cleanup( );
    }
}

void RDMAConnection::_cleanup( )
{
    EQASSERT( STATE_CLOSED == _state );

    _sourcebuf.clear( );
    _sinkbuf.clear( );
    _msgbuf.clear( );

    if( _completions > 0U )
    {
        ::ibv_ack_cq_events( _cq, _completions );
        _completions = 0U;
    }

    delete[] _wcs;
    _wcs = 0;

#ifdef _WIN32
    if ( _ccWaitObj )
        UnregisterWait( _ccWaitObj );
    _ccWaitObj = 0;

    if ( _cmWaitObj )
        UnregisterWait( _cmWaitObj );
    _cmWaitObj = 0;
#endif

    if( NULL != _cm_id )
        ::rdma_destroy_ep( _cm_id );
    _cm_id = NULL;

    if(( NULL != _cq ) && ::rdma_seterrno( ::ibv_destroy_cq( _cq )))
        EQWARN << "ibv_destroy_cq : " << co::base::sysError << std::endl;
    _cq = NULL;

    if(( NULL != _cc ) && ::rdma_seterrno( ::ibv_destroy_comp_channel( _cc )))
        EQWARN << "ibv_destroy_comp_channel : " << co::base::sysError
            << std::endl;
    _cc = NULL;

    if(( NULL != _pd ) && ::rdma_seterrno( ::ibv_dealloc_pd( _pd )))
        EQWARN << "ibv_dealloc_pd : " << co::base::sysError << std::endl;
    _pd = NULL;

    if( NULL != _cm )
        ::rdma_destroy_event_channel( _cm );
    _cm = NULL;

    if( NULL != _rai )
        ::rdma_freeaddrinfo( _rai );
    _rai = NULL;

    _rptr = 0UL;
    _rbase = _rkey = 0ULL;
#ifndef _WIN32
    if(( _notifier >= 0 ) && TEMP_FAILURE_RETRY( ::close( _notifier )))
        EQWARN << "close : " << co::base::sysError << std::endl;
    _notifier = -1;
#endif
}

bool RDMAConnection::_finishAccept( struct rdma_cm_id *new_cm_id,
        const RDMAConnParamData &cpd )
{
    EQASSERT( STATE_CLOSED == _state );
    setState( STATE_CONNECTING );

    EQASSERT( NULL != new_cm_id );

    _cm_id = new_cm_id;

    {
        // FIXME : RDMA CM appears to send up invalid addresses when receiving
        // connections that use a different protocol than what was bound.  E.g.
        // if an IPv6 listener gets an IPv4 connection then the sa_family
        // will be AF_INET6 but the actual data is struct sockaddr_in.  Example:
        //
        // 0000000: 0a00 bc10 c0a8 b01a 0000 0000 0000 0000  ................
        //
        // However, in the reverse case, when an IPv4 listener gets an IPv6
        // connection not only is the address family incorrect, but the actual
        // IPv6 address is only partially there:
        //
        // 0000000: 0200 bc11 0000 0000 fe80 0000 0000 0000  ................
        // 0000010: 0000 0000 0000 0000 0000 0000 0000 0000  ................
        // 0000020: 0000 0000 0000 0000 0000 0000 0000 0000  ................
        // 0000030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
        //
        // Surely seems to be a bug in RDMA CM.

        union
        {
            struct sockaddr     addr;
            struct sockaddr_in  sin;
            struct sockaddr_in6 sin6;
            struct sockaddr_storage storage;
        } sss;

        // Make a copy since we might change it.
        //sss.storage = _cm_id->route.addr.dst_storage;
        ::memcpy( (void *)&sss.storage,
            (const void *)::rdma_get_peer_addr( _cm_id ),
            sizeof(struct sockaddr_storage) );
#ifndef _WIN32
        if(( AF_INET == sss.storage.ss_family ) &&
           ( sss.sin6.sin6_addr.s6_addr32[0] != 0 ||
             sss.sin6.sin6_addr.s6_addr32[1] != 0 ||
             sss.sin6.sin6_addr.s6_addr32[2] != 0 ||
             sss.sin6.sin6_addr.s6_addr32[3] != 0 ))
        {
            EQWARN << "IPv6 address detected but likely invalid!" << std::endl;
            sss.storage.ss_family = AF_INET6;
        }
        else 
#endif
        if(( AF_INET6 == sss.storage.ss_family ) &&
                ( INADDR_ANY != sss.sin.sin_addr.s_addr ))
        {
            sss.storage.ss_family = AF_INET;
        }

        _updateInfo( &sss.addr );
    }

    _device_name = ::ibv_get_device_name( _cm_id->verbs->device );

    EQVERB << "Connection initiated on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " from "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    if(( RDMA_PROTOCOL_MAGIC != cpd.magic ) ||
        ( RDMA_PROTOCOL_VERSION != cpd.version ))
    {
        EQERROR << "Protocol mismatch with initiator : "
            << _addr << ":" << _serv << std::endl;
        goto err_reject;
    }

    if( !_createNotifier( ))
    {
        EQERROR << "Failed to create master notifier." << std::endl;
        goto err_reject;
    }

    if( !_createEventChannel( ))
    {
        EQERROR << "Failed to create event channel." << std::endl;
        goto err_reject;
    }

    if( !_migrateId( ))
    {
        EQERROR << "Failed to migrate communication identifier." << std::endl;
        goto err_reject;
    }

    if( !_initProtocol( cpd.depth ))
    {
        EQERROR << "Failed to initialize protocol variables." << std::endl;
        goto err_reject;
    }

    if( !_initVerbs( ))
    {
        EQERROR << "Failed to initialize verbs." << std::endl;
        goto err_reject;
    }

    if( !_createQP( ))
    {
        EQERROR << "Failed to create queue pair." << std::endl;
        goto err_reject;
    }

    if( !_createBytesAvailableFD( ))
    {
        EQERROR << "Failed to create available byte notifier." << std::endl;
        goto err_reject;
    }

    if( !_initBuffers( ))
    {
        EQERROR << "Failed to initialize ring buffers." << std::endl;
        goto err_reject;
    }

    if( !_postReceives( _depth ))
    {
        EQERROR << "Failed to pre-post receives." << std::endl;
        goto err_reject;
    }

    if( !_accept( ))
    {
        EQERROR << "Failed to accept remote connection from : "
            << _addr << ":" << _serv << std::endl;
        goto err;
    }

    EQASSERT( _established );

    if( !_waitRecvSetup( ))
    {
        EQERROR << "Failed to receive setup message." << std::endl;
        goto err;
    }

    if( !_postSetup( ))
    {
        EQERROR << "Failed to post setup message." << std::endl;
        goto err;
    }

    EQVERB << "Connection accepted on " << std::dec
        << _device_name << ":" << (int)_cm_id->port_num
        << " from "
        << _addr << ":" << _serv
        << " (" << _description->toString( ) << ")"
        << std::endl;

    setState( STATE_CONNECTED );

    return true;

err_reject:
    EQWARN << "Rejecting connection from remote address : "
        << _addr << ":" << _serv << std::endl;

    if( !_reject( ))
        EQWARN << "Failed to issue connection reject." << std::endl;

err:
    close( );

    return false;
}

bool RDMAConnection::_lookupAddress( const bool passive )
{
    struct rdma_addrinfo hints;
    char *node = NULL, *service = NULL;
    std::string s;

    ::memset( (void *)&hints, 0, sizeof(struct rdma_addrinfo) );
    //hints.ai_flags |= RAI_NOROUTE;
    if( passive )
        hints.ai_flags |= RAI_PASSIVE;

    const std::string &hostname = _description->getHostname( );
    if( !hostname.empty( ))
        node = const_cast< char * >( hostname.c_str( ));

    if( 0u != _description->port )
    {
        std::stringstream ss;
        ss << _description->port;
        s = ss.str( );
        service = const_cast< char * >( s.c_str( ));
    }

    if(( NULL != node ) && ::rdma_getaddrinfo( node, service, &hints, &_rai ))
    {
        EQERROR << "rdma_getaddrinfo : " << co::base::sysError << std::endl;
        goto err;
    }

    if(( NULL != _rai ) && ( NULL != _rai->ai_next ))
        EQWARN << "Multiple getaddrinfo results, using first." << std::endl;

    if(( NULL != _rai ) && ( _rai->ai_connect_len > 0 ))
        EQWARN << "WARNING : ai_connect data specified!" << std::endl;

    return true;

err:
    return false;
}

void RDMAConnection::_updateInfo( struct sockaddr *addr )
{
    int salen = sizeof(struct sockaddr);
    bool is_unspec = false;

    if( AF_INET == addr->sa_family )
    {
        struct sockaddr_in *sin =
            reinterpret_cast< struct sockaddr_in * >( addr );
        is_unspec = ( INADDR_ANY == sin->sin_addr.s_addr );
        salen = sizeof(struct sockaddr_in);
    }
    // TODO: IPv6 for WIndows
#ifndef _WIN32
    else if( AF_INET6 == addr->sa_family )
    {
        struct sockaddr_in6 *sin6 =
            reinterpret_cast< struct sockaddr_in6 * >( addr );

        is_unspec = ( sin6->sin6_addr.s6_addr32[0] == 0 &&
                      sin6->sin6_addr.s6_addr32[1] == 0 &&
                      sin6->sin6_addr.s6_addr32[2] == 0 &&
                      sin6->sin6_addr.s6_addr32[3] == 0 );
        salen = sizeof(struct sockaddr_in6);
    }
#endif
    int err;
    if(( err = ::getnameinfo( addr, salen, _addr, sizeof(_addr),
            _serv, sizeof(_serv), NI_NUMERICHOST | NI_NUMERICSERV )))
        EQWARN << "Name info lookup failed : " << err << std::endl;

    if( is_unspec )
        ::gethostname( _addr, NI_MAXHOST );

    if( _description->getHostname( ).empty( ))
        _description->setHostname( _addr );
    if( 0u == _description->port )
        _description->port = atoi( _serv );
}

bool RDMAConnection::_createEventChannel( )
{
    EQASSERT( NULL == _cm );

    _cm = ::rdma_create_event_channel( );
    if( NULL == _cm )
    {
        EQERROR << "rdma_create_event_channel : " << co::base::sysError <<
            std::endl;
        goto err;
    }

#ifdef _WIN32
    if ( !RegisterWaitForSingleObject( 
            &_cmWaitObj, 
            _cm->channel.Event, 
            WAITORTIMERCALLBACK( &_triggerNotifierCM ), 
            this, 
            INFINITE, 
            WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE ))
    {
        EQERROR << "RegisterWaitForSingleObject : " << co::base::sysError 
                << std::endl;
        goto err;
    }
#else
    struct epoll_event evctl;

    EQASSERT( _notifier >= 0 );

    // Use the CM fd to signal Collage of connection events.
    ::memset( (void *)&evctl, 0, sizeof(struct epoll_event) );
    evctl.events = EPOLLIN;
    evctl.data.fd = _cm->fd;
    if( ::epoll_ctl( _notifier, EPOLL_CTL_ADD, evctl.data.fd, &evctl ))
    {
        EQERROR << "epoll_ctl : " << co::base::sysError << std::endl;
        goto err;
    }
#endif

    return true;

err:
    return false;
}

bool RDMAConnection::_createId( )
{
    EQASSERT( NULL != _cm );
    EQASSERT( NULL == _cm_id );

    if( ::rdma_create_id( _cm, &_cm_id, NULL, RDMA_PS_TCP ))
    {
        EQERROR << "rdma_create_id : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_initVerbs( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( NULL != _cm_id->verbs );

    EQASSERT( NULL == _pd );

    // Allocate protection domain.
    _pd = ::ibv_alloc_pd( _cm_id->verbs );
    if( NULL == _pd )
    {
        EQERROR << "ibv_alloc_pd : " << co::base::sysError << std::endl;
        goto err;
    }

    EQASSERT( NULL == _cc );

    // Create completion channel.
    _cc = ::ibv_create_comp_channel( _cm_id->verbs );
    if( NULL == _cc )
    {
        EQERROR << "ibv_create_comp_channel : " << co::base::sysError
            << std::endl;
        goto err;
    }

#ifdef _WIN32
    if ( !RegisterWaitForSingleObject( 
        &_ccWaitObj, 
        _cc->comp_channel.Event, 
        WAITORTIMERCALLBACK( &_triggerNotifierCQ ), 
        this, 
        INFINITE, 
        WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE ))
    {
        EQERROR << "RegisterWaitForSingleObject : " << co::base::sysError 
            << std::endl;
        goto err;
    }
#else
    EQASSERT( _notifier >= 0 );

    // Use the completion channel fd to signal Collage of RDMA writes received.
    struct epoll_event evctl;
    ::memset( (void *)&evctl, 0, sizeof(struct epoll_event) );
    evctl.events = EPOLLIN;
    evctl.data.fd = _cc->fd;
    if( ::epoll_ctl( _notifier, EPOLL_CTL_ADD, evctl.data.fd, &evctl ))
    {
        EQERROR << "epoll_ctl : " << co::base::sysError << std::endl;
        goto err;
    }
#endif

    EQASSERT( NULL == _cq );

    // Create a single completion queue for both sends & receives */
    _cq = ::ibv_create_cq( _cm_id->verbs, _depth * 2, NULL, _cc, 0 );
    if( NULL == _cq )
    {
        EQERROR << "ibv_create_cq : " << co::base::sysError << std::endl;
        goto err;
    }

    // Request IBV_SEND_SOLICITED events only (i.e. RDMA writes, not FC)
    if( ::rdma_seterrno( ::ibv_req_notify_cq( _cq, 1 )))
    {
        EQERROR << "ibv_req_notify_cq : " << co::base::sysError << std::endl;
        goto err;
    }

    _wcs = new struct ibv_wc[ _depth ];

    return true;

err:
    return false;
}

bool RDMAConnection::_createQP( )
{
    struct ibv_qp_init_attr init_attr;

    EQASSERT( _depth > 0 );
    EQASSERT( NULL != _cm_id );
    EQASSERT( NULL != _pd );
    EQASSERT( NULL != _cq );

    ::memset( (void *)&init_attr, 0, sizeof(struct ibv_qp_init_attr) );
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.cap.max_send_wr = _depth;
    init_attr.cap.max_recv_wr = _depth;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.recv_cq = _cq;
    init_attr.send_cq = _cq;
    init_attr.sq_sig_all = 1; // i.e. always IBV_SEND_SIGNALED

    // Create queue pair.
    if( ::rdma_create_qp( _cm_id, _pd, &init_attr ))
    {
        EQERROR << "rdma_create_qp : " << co::base::sysError << std::endl;
        goto err;
    }

    EQVERB << "RDMA QP caps : " << std::dec <<
        init_attr.cap.max_recv_wr << " receives, " <<
        init_attr.cap.max_send_wr << " sends, " << std::endl;

    return true;

err:
    return false;
}

bool RDMAConnection::_initBuffers( )
{
    const size_t rbs = 1024 * 1024 *
        Global::getIAttribute( Global::IATTR_RDMA_RING_BUFFER_SIZE_MB );

    EQASSERT( _depth > 0 );
    EQASSERT( NULL != _pd );

    if( 0 == rbs )
    {
        EQERROR << "Invalid RDMA ring buffer size." << std::endl;
        goto err;
    }

    if( !_sourcebuf.resize( _pd, rbs ))
    {
        EQERROR << "Failed to resize source buffer." << std::endl;
        goto err;
    }

    if( !_sinkbuf.resize( _pd, rbs ))
    {
        EQERROR << "Failed to resize sink buffer." << std::endl;
        goto err;
    }

    _sourceptr.clear( static_cast<uint32_t>( _sourcebuf.getSize( )));
    _sinkptr.clear( static_cast<uint32_t>( _sinkbuf.getSize( )));

    // Need enough space for both sends and receives.
    if( !_msgbuf.resize( _pd, _depth * 2 ))
    {
        EQERROR << "Failed to resize message buffer pool." << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_resolveAddress( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( NULL != _rai );

    if( ::rdma_resolve_addr( _cm_id, _rai->ai_src_addr, _rai->ai_dst_addr,
            _timeout ))
    {
        EQERROR << "rdma_resolve_addr : " << co::base::sysError << std::endl;
        goto err;
    }

    return _waitForCMEvent( RDMA_CM_EVENT_ADDR_RESOLVED );

err:
    return false;
}

bool RDMAConnection::_resolveRoute( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( NULL != _rai );

    if(( IBV_TRANSPORT_IB == _cm_id->verbs->device->transport_type ) &&
            ( _rai->ai_route_len > 0 ))
    {
#ifdef _WIN32
        if( ::rdma_set_option( _cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_TOS,
#else
        if( ::rdma_set_option( _cm_id, RDMA_OPTION_IB, RDMA_OPTION_IB_PATH,
#endif
                _rai->ai_route, _rai->ai_route_len ))
        {
            EQERROR << "rdma_set_option : " << co::base::sysError << std::endl;
            goto err;
        }

        // rdma_resolve_route not required (TODO : is this really true?)
        return true;
    }

    if( ::rdma_resolve_route( _cm_id, _timeout ))
    {
        EQERROR << "rdma_resolve_route : " << co::base::sysError << std::endl;
        goto err;
    }

    return _waitForCMEvent( RDMA_CM_EVENT_ROUTE_RESOLVED );

err:
    return false;
}

bool RDMAConnection::_connect( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( !_established );

#if 0 // TODO
    static const uint8_t DSCP = 0;

    if( ::rdma_set_option( _cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_TOS,
            (void *)&DSCP, sizeof(DSCP) ))
    {
        EQERROR << "rdma_set_option : " << co::base::sysError << std::endl;
        goto err;
    }
#endif

    struct rdma_conn_param conn_param;

    ::memset( (void *)&conn_param, 0, sizeof(struct rdma_conn_param) );

    _cpd.magic = RDMA_PROTOCOL_MAGIC;
    _cpd.version = RDMA_PROTOCOL_VERSION;
    _cpd.depth = _depth;
    conn_param.private_data = reinterpret_cast< const void * >( &_cpd );
    conn_param.private_data_len = sizeof(struct RDMAConnParamData);
    conn_param.initiator_depth = RDMA_MAX_INIT_DEPTH;
    conn_param.responder_resources = RDMA_MAX_RESP_RES;
    // Magic 3-bit values.
    //conn_param.retry_count = 5;
    //conn_param.rnr_retry_count = 7;

    //EQINFO << "Connect on source lid : " << std::showbase
    //    << std::hex << ntohs( _cm_id->route.path_rec->slid ) << " ("
    //    << std::dec << ntohs( _cm_id->route.path_rec->slid ) << ") "
    //    << "to dest lid : "
    //    << std::hex << ntohs( _cm_id->route.path_rec->dlid ) << " ("
    //    << std::dec << ntohs( _cm_id->route.path_rec->dlid ) << ") "
    //    << std::endl;

    if( ::rdma_connect( _cm_id, &conn_param ))
    {
        EQERROR << "rdma_connect : " << co::base::sysError << std::endl;
        goto err;
    }

    return _waitForCMEvent( RDMA_CM_EVENT_ESTABLISHED );

err:
    return false;
}

bool RDMAConnection::_bindAddress( )
{
    EQASSERT( NULL != _cm_id );

#if IPV6_DEFAULT
    struct sockaddr_in6 sin;
    memset( (void *)&sin, 0, sizeof(struct sockaddr_in6) );
    sin.sin6_family = AF_INET6;
    sin.sin6_port = htons( _description->port );
    sin.sin6_addr = in6addr_any;
#else
    struct sockaddr_in sin;
    memset( (void *)&sin, 0, sizeof(struct sockaddr_in) );
    sin.sin_family = AF_INET;
    sin.sin_port = htons( _description->port );
    sin.sin_addr.s_addr = INADDR_ANY;
#endif

    if( ::rdma_bind_addr( _cm_id, ( NULL != _rai ) ? _rai->ai_src_addr :
            reinterpret_cast< struct sockaddr * >( &sin )))
    {
        EQERROR << "rdma_bind_addr : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_listen( int backlog )
{
    EQASSERT( NULL != _cm_id );

    if( ::rdma_listen( _cm_id, backlog ))
    {
        EQERROR << "rdma_listen : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_migrateId( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( NULL != _cm );

    if( ::rdma_migrate_id( _cm_id, _cm ))
    {
        EQERROR << "rdma_migrate_id : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_accept( )
{
    struct rdma_conn_param accept_param;

    EQASSERT( NULL != _cm_id );
    EQASSERT( !_established );

    ::memset( (void *)&accept_param, 0, sizeof(struct rdma_conn_param) );

    _cpd.magic = RDMA_PROTOCOL_MAGIC;
    _cpd.version = RDMA_PROTOCOL_VERSION;
    _cpd.depth = _depth;
    accept_param.private_data = reinterpret_cast< const void * >( &_cpd );
    accept_param.private_data_len = sizeof(struct RDMAConnParamData);
    accept_param.initiator_depth = RDMA_MAX_INIT_DEPTH;
    accept_param.responder_resources = RDMA_MAX_RESP_RES;
    // Magic 3-bit value.
    //accept_param.rnr_retry_count = 7;

    //EQINFO << "Accept on source lid : "<< std::showbase
    //       << std::hex << ntohs( _cm_id->route.path_rec->slid ) << " ("
    //       << std::dec << ntohs( _cm_id->route.path_rec->slid ) << ") "
    //       << "from dest lid : "
    //       << std::hex << ntohs( _cm_id->route.path_rec->dlid ) << " ("
    //       << std::dec << ntohs( _cm_id->route.path_rec->dlid ) << ") "
    //       << std::endl;

    if( ::rdma_accept( _cm_id, &accept_param ))
    {
        EQERROR << "rdma_accept : " << co::base::sysError << std::endl;
        goto err;
    }

    return _waitForCMEvent( RDMA_CM_EVENT_ESTABLISHED );

err:
    return false;
}

bool RDMAConnection::_reject( )
{
    EQASSERT( NULL != _cm_id );
    EQASSERT( !_established );

    if( ::rdma_reject( _cm_id, NULL, 0 ))
    {
        EQERROR << "rdma_reject : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

////////////////////////////////////////////////////////////////////////////////

bool RDMAConnection::_initProtocol( int32_t depth )
{
    if( depth < 2L )
    {
        EQERROR << "Invalid queue depth." << std::endl;
        goto err;
    }

    _depth = depth;
    _writes = 0L;
    _fcs = 0L;
    _wcredits = _depth / 2 - 2;
    _fcredits = _depth / 2 + 2;

    return true;

err:
    return false;
}

/* inline */
bool RDMAConnection::_needFC( )
{
    // TODO : TODO : TODO : TODO : TODO : TODO : TODO : TODO : TODO
    // This isn't sufficient to guarantee deadlock-free operation and
    // RNR avoidance.  The credit-based flow control protocol needs
    // work for higher latency conditions and/or smaller queue depths.
    return true;//( _writes > 0 );
}

bool RDMAConnection::_postReceives( const uint32_t count )
{
    EQASSERT( NULL != _cm_id->qp );
    EQASSERT( count > 0UL );

    ibv_sge* sge = new ibv_sge[count];
    ibv_recv_wr* wrs = new ibv_recv_wr[count];

    for( uint32_t i = 0UL; i != count; i++ )
    {
        sge[i].addr = (uint64_t)(uintptr_t)_msgbuf.getBuffer( );
        sge[i].length = static_cast<uint32_t>( _msgbuf.getBufferSize( ));
        sge[i].lkey = _msgbuf.getMR( )->lkey;

        wrs[i].wr_id = sge[i].addr;
        wrs[i].next = &wrs[i + 1];
        wrs[i].sg_list = &sge[i];
        wrs[i].num_sge = 1;
    }
    wrs[count - 1].next = NULL;

    struct ibv_recv_wr *bad_wr;
    if( ::rdma_seterrno( ::ibv_post_recv( _cm_id->qp, wrs, &bad_wr )))
    {
        EQERROR << "ibv_post_recv : "  << co::base::sysError << std::endl;
        goto err;
    }

    delete[] sge;
    delete[] wrs;
    return true;

err:
    delete[] sge;
    delete[] wrs;
    return false;
}

/* inline */
void RDMAConnection::_recvRDMAWrite( const uint32_t imm_data )
{
    union
    {
        uint32_t val;
        RDMAFCImm fc;
    } x;
    x.val = ntohl( imm_data );

    // Analysis:
    //
    // Since the ring pointers are circular, a malicious (presumably overflow)
    // value here would at worst only result in us reading arbitrary regions
    // from our sink buffer, not segfaulting.  If the other side wanted us to
    // reread a previous message it should just resend it!
    _sinkptr.incrHead( x.fc.bytes_sent );
    _incrAvailableBytes( x.fc.bytes_sent );

    _fcredits += x.fc.fcs_received;
    EQASSERTINFO( _fcredits <= _depth, _fcredits << " > " << _depth );

    _writes++;
}

/* inline */
uint32_t RDMAConnection::_makeImm( const uint32_t b )
{
    union
    {
        uint32_t val;
        RDMAFCImm fc;
    } x;

    x.fc.fcs_received = std::min( MAX_FC, static_cast< uint16_t >( _fcs ));
    _fcs -= x.fc.fcs_received;
    EQASSERT( _fcs >= 0 );

    EQASSERT( b <= MAX_BS );
    x.fc.bytes_sent = b;

    return htonl( x.val );
}

bool RDMAConnection::_postRDMAWrite( )
{
    struct ibv_sge sge;
    struct ibv_send_wr wr;

    sge.addr = (uint64_t)( (uintptr_t)_sourcebuf.getBase( ) +
        _sourceptr.ptr( _sourceptr.MIDDLE ));
    sge.length = (uint64_t)_sourceptr.available( _sourceptr.HEAD,
        _sourceptr.MIDDLE );
    sge.lkey = _sourcebuf.getMR( )->lkey;
    _sourceptr.incr( _sourceptr.MIDDLE, (uint32_t)sge.length );

    wr.wr_id = (uint64_t)sge.length;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SOLICITED; // Important!
    wr.imm_data = _makeImm( (uint32_t)sge.length );
    wr.wr.rdma.rkey = _rkey;
    wr.wr.rdma.remote_addr = (uint64_t)( (uintptr_t)_rbase +
        _rptr.ptr( _rptr.HEAD ));
    _rptr.incrHead( (uint32_t)sge.length );

    _wcredits--;
    EQASSERT( _wcredits >= 0L );

    struct ibv_send_wr *bad_wr;
    if( ::rdma_seterrno( ::ibv_post_send( _cm_id->qp, &wr, &bad_wr )))
    {
        EQERROR << "ibv_post_send : "  << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_postMessage( const RDMAMessage &message )
{
    _fcredits--;
    EQASSERT( _fcredits >= 0L );

    if( ::rdma_post_send( _cm_id, (void *)&message, (void *)&message,
            offsetof( RDMAMessage, payload ) + message.length, _msgbuf.getMR( ),
            0 ))
    {
        EQERROR << "rdma_post_send : "  << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
}

void RDMAConnection::_recvMessage( const RDMAMessage &message )
{
    switch( message.opcode )
    {
        case FC:
            if( sizeof(struct RDMAFCPayload) == (size_t)message.length )
                _recvFC( message.payload.fc );
            else
                EQWARN << "Invalid flow control message received!" << std::endl;
            break;
        case SETUP:
            if( sizeof(struct RDMASetupPayload) == (size_t)message.length )
                _recvSetup( message.payload.setup );
            else
                EQWARN << "Invalid setup message received!" << std::endl;
            break;
        default:
            EQWARN << "Invalid message received: "
                << std::hex << (int)message.opcode << std::dec << std::endl;
    }
}

/* inline */
void RDMAConnection::_recvFC( const RDMAFCPayload &fc )
{
    // Analysis:
    //
    // Since we will only write a maximum of _sourceptr.available( ) bytes
    // to our source buffer, a malicious (presumably overflow) value here would
    // have no chance of causing us to write beyond our buffer as we have local
    // control over those ring pointers.  Worst case, we'd and up writing to
    // arbitrary regions of the remote buffer, since this ring pointer is
    // circular as well.
    _rptr.incrTail( fc.bytes_received );

    _wcredits += fc.writes_received;
    EQASSERTINFO( _wcredits <= _depth, _wcredits << " > " << _depth );

    _fcs++;
}

bool RDMAConnection::_postFC( const uint32_t bytes_taken )
{
    RDMAMessage &message =
        *reinterpret_cast< RDMAMessage * >( _msgbuf.getBuffer( ));

    message.opcode = FC;
    message.length = (uint8_t)sizeof(struct RDMAFCPayload);

    message.payload.fc.bytes_received = bytes_taken;
    message.payload.fc.writes_received = _writes;
    _writes -= message.payload.fc.writes_received;
    EQASSERT( _writes >= 0 );

    return _postMessage( message );
}

void RDMAConnection::_recvSetup( const RDMASetupPayload &setup )
{
    // Analysis:
    //
    // Malicious values here would only affect the receiver, we're willing
    // to RDMA write to anywhere specified!
    _rbase = setup.rbase;
    _rptr.clear( setup.rlen );
    _rkey = setup.rkey;

    EQVERB << "RDMA MR: " << std::showbase
        << std::dec << setup.rlen << " @ "
        << std::hex << setup.rbase << std::dec << std::endl;
}

bool RDMAConnection::_postSetup( )
{
    RDMAMessage &message =
        *reinterpret_cast< RDMAMessage * >( _msgbuf.getBuffer( ));

    message.opcode = SETUP;
    message.length = (uint8_t)sizeof(struct RDMASetupPayload);

    message.payload.setup.rbase = (uint64_t)(uintptr_t)_sinkbuf.getBase( );
    message.payload.setup.rlen = (uint64_t)_sinkbuf.getSize( );
    message.payload.setup.rkey = _sinkbuf.getMR( )->rkey;

    return _postMessage( message );
}

bool RDMAConnection::_waitRecvSetup( )
{
    co::base::Clock clock;
    const int64_t start = clock.getTime64( );
    const uint32_t timeout = Global::getTimeout( );
    eventset events;

retry:
    if( !_checkDisconnected( events ))
    {
        EQERROR << "Error while checking event state." << std::endl;
        goto err;
    }

    if( !_established )
    {
        EQERROR << "Disconnected while waiting for setup message." << std::endl;
        goto err;
    }

    if( !_checkCQ( true ))
    {
        EQERROR << "Error while polling receive completion queue." << std::endl;
        goto err;
    }

    if( 0ULL == _rkey )
    {
        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out waiting for setup message." << std::endl;
                goto err;
            }
        }

        co::base::Thread::yield( );
        goto retry;
    }
    
    return true;

err:
    return false;
}

////////////////////////////////////////////////////////////////////////////////

bool RDMAConnection::_createNotifier( )
{
#ifdef _WIN32
    _event->connect();
    _event->reset();
    return true;
#else
    EQASSERT( _notifier < 0 );

    _notifier = ::epoll_create( 1 );
    if( _notifier < 0 )
    {
        EQERROR << "epoll_create1 : " << co::base::sysError << std::endl;
        goto err;
    }

    return true;

err:
    return false;
#endif
}

bool RDMAConnection::_checkEvents( eventset &events )
{
    events.reset( );

#ifdef _WIN32
    co::base::ScopedFastWrite mutex( _eventFlagLock );
    if (( _eventFlag & MASK_BUF_EVENT ) != 0 )
        events.set( BUF_EVENT );
    if (( _eventFlag & MASK_CQ_EVENT ) != 0 )
        events.set( CQ_EVENT );
    if (( _eventFlag & MASK_CM_EVENT ) != 0 )
        events.set( CM_EVENT );
    
    _eventFlag = 0;

    return true;
#else
    struct epoll_event evts[3];

    int nfds = TEMP_FAILURE_RETRY( ::epoll_wait( _notifier, evts, 3, 0 ));
    if( nfds < 0 )
    {
        EQERROR << "epoll_wait : " << co::base::sysError << std::endl;
        goto err;
    }

    for( int i = 0; i < nfds; i++ )
    {
        const int fd = evts[i].data.fd;
        if(( _pipe_fd[0] >= 0 ) && ( fd == _pipe_fd[0] ))
            events.set( BUF_EVENT );
        else if( _cc && ( fd == _cc->fd ))
            events.set( CQ_EVENT );
        else if( _cm && ( fd == _cm->fd ))
            events.set( CM_EVENT );
        else
            EQUNREACHABLE;
    }

    return true;

err:
    return false;
#endif
}

bool RDMAConnection::_checkDisconnected( eventset &events )
{
    co::base::ScopedWrite poll_mutex( _poll_lock );

    if( !_checkEvents( events ))
    {
        EQERROR << "Error while checking event state." << std::endl;
        goto err;
    }

    if( events.test( CM_EVENT ))
    {
        if( !_doCMEvent( RDMA_CM_EVENT_DISCONNECTED ))
        {
            EQERROR << "Unexpected connection manager event." << std::endl;
            goto err;
        }

        EQASSERT( !_established );
    }

    return true;

err:
    return false;
}

bool RDMAConnection::_createBytesAvailableFD( )
{
#ifndef _WIN32
    if ( pipe( _pipe_fd ) == -1 )
    {
        EQERROR << "pipe: " << co::base::sysError << std::endl;
        return false;
    }

    EQASSERT( _pipe_fd[0] >= 0 && _pipe_fd[1] >= 0);

    // Use the event fd to signal Collage of bytes remaining.
    struct epoll_event evctl;
    ::memset( (void *)&evctl, 0, sizeof(struct epoll_event) );
    evctl.events = EPOLLIN;
    evctl.data.fd = _pipe_fd[0];
    if( ::epoll_ctl( _notifier, EPOLL_CTL_ADD, evctl.data.fd, &evctl ))
    {
        EQERROR << "epoll_ctl : " << co::base::sysError << std::endl;
        return false;
    }
#endif
    return true;
}

bool RDMAConnection::_incrAvailableBytes( const uint64_t b )
{
#ifdef _WIN32
    _availBytes += b;
    _event->set();
    co::base::ScopedFastWrite mutex( _eventFlagLock );
    _eventFlag |= MASK_BUF_EVENT;
#else
    if( ::write( _pipe_fd[1], (const void *)&b, sizeof(b) ) != sizeof(b) )
    {
        EQERROR << "write : " << co::base::sysError << std::endl;
        return false;
    }
#endif
    return true;
}

uint64_t RDMAConnection::_getAvailableBytes( )
{
    uint64_t available_bytes = 0;
#ifdef _WIN32
    available_bytes = static_cast<uint64_t>( _availBytes );
    _availBytes = 0;
    co::base::ScopedFastWrite mutex( _eventFlagLock );
    _eventFlag &= ~MASK_BUF_EVENT;
    return available_bytes;
#else
    uint64_t currBytes = 0;
    ssize_t count;
    pollfd pfd;
    pfd.fd = _pipe_fd[0];
    pfd.events = EPOLLIN;

    do 
    {
        count = ::read( _pipe_fd[0], (void *)&currBytes, sizeof( currBytes ));
        if ( count > 0 && count < (ssize_t)sizeof( currBytes ) )
            goto err;
        available_bytes += currBytes;
        pfd.revents = 0;
        if ( ::poll( &pfd, 1, 0 ) == -1 )
        {
            EQERROR << "poll : " << co::base::sysError << std::endl;
            goto err;
        }
    } while ( pfd.revents > 0 );
    
    if ( count == -1 )
    {
        EQERROR << "read : " << co::base::sysError << std::endl;
        goto err;
    }

    EQASSERT( available_bytes > 0ULL );
    return available_bytes;
err:
    return 0ULL;
#endif
}

bool RDMAConnection::_waitForCMEvent( enum rdma_cm_event_type expected )
{
    co::base::Clock clock;
    const int64_t start = clock.getTime64( );
    const uint32_t timeout = Global::getTimeout( );
    bool done = false;
    eventset events;

retry:
    if( !_checkEvents( events ))
    {
        EQERROR << "Error while checking event state." << std::endl;
        goto err;
    }

    if( events.test( CM_EVENT ))
    {
        done = _doCMEvent( expected );
        if( !done )
        {
            EQERROR << "Unexpected connection manager event." << std::endl;
            goto err;
        }
    }

    if( !done )
    {
        if( EQ_TIMEOUT_INDEFINITE != timeout )
        {
            if(( clock.getTime64( ) - start ) > timeout )
            {
                EQERROR << "Timed out waiting for setup message." << std::endl;
                goto err;
            }
        }

        co::base::Thread::yield( );
        goto retry;
    }
    
    return true;

err:
    return false;
}

bool RDMAConnection::_doCMEvent( enum rdma_cm_event_type expected )
{
    bool ok = false;
    struct rdma_cm_event *event;

    if( ::rdma_get_cm_event( _cm, &event ))
    {
        EQERROR << "rdma_get_cm_event : " << co::base::sysError << std::endl;
        goto out;
    }

    ok = ( event->event == expected );

#ifndef NDEBUG
    if( ok )
    {
        EQVERB << (void *)this
            << " (" << _addr << ":" << _serv << ")"
            << " event : " << ::rdma_event_str( event->event )
            << std::endl;
    }
    else
    {
        EQINFO << (void *)this
            << " (" << _addr << ":" << _serv << ")"
            << " event : " << ::rdma_event_str( event->event )
            << " expected: " << ::rdma_event_str( expected )
            << std::endl;
    }
#endif

    if( ok && ( RDMA_CM_EVENT_DISCONNECTED == event->event ))
        _established = false;

    if( ok && ( RDMA_CM_EVENT_ESTABLISHED == event->event ))
    {
        _established = true;

        struct rdma_conn_param *cp = &event->param.conn;

        ::memset( (void *)&_cpd, 0, sizeof(RDMAConnParamData) );
        // Note that the actual amount of data transferred to the remote side
        // is transport dependent and may be larger than that requested.
        if( cp->private_data_len >= sizeof(RDMAConnParamData) )
            _cpd = *reinterpret_cast< const RDMAConnParamData * >(
                cp->private_data );
    }

    if( ok && ( RDMA_CM_EVENT_CONNECT_REQUEST == event->event ))
    {
        _new_cm_id = event->id;

        struct rdma_conn_param *cp = &event->param.conn;

        ::memset( (void *)&_cpd, 0, sizeof(RDMAConnParamData) );
        // TODO : Not sure what happens when initiator sent ai_connect data
        // (assuming the underlying transport doesn't strip it)?
        if( cp->private_data_len >= sizeof(RDMAConnParamData) )
            _cpd = *reinterpret_cast< const RDMAConnParamData * >(
                cp->private_data );
    }

    if( RDMA_CM_EVENT_REJECTED == event->event )
        EQINFO << "Connection reject status : " << event->status << std::endl;

#ifdef _WIN32
    EnterCriticalSection( &_cm->channel.Lock );
    // reset event
    if (!_cm->channel.Head)
        ResetEvent( _cm->channel.Event );

    // rearm event callback
    UnregisterWait( _cmWaitObj );
    if ( !RegisterWaitForSingleObject( 
        &_cmWaitObj, 
        _cm->channel.Event, 
        WAITORTIMERCALLBACK( &_triggerNotifierCM ), 
        this, 
        INFINITE, 
        WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE ))
    {
        EQERROR << "RegisterWaitForSingleObject : " << co::base::sysError 
            << std::endl;
        return false;
    }
    {
        co::base::ScopedFastWrite mutex( _eventFlagLock );
        _eventFlag &= ~MASK_CM_EVENT;
    }
    LeaveCriticalSection ( &_cm->channel.Lock );
#endif

    if( ::rdma_ack_cm_event( event ))
        EQWARN << "rdma_ack_cm_event : "  << co::base::sysError << std::endl;

out:
    return ok;
}

bool RDMAConnection::_rearmCQ( )
{
    struct ibv_cq *ev_cq;
    void *ev_ctx;

    if( ::ibv_get_cq_event( _cc, &ev_cq, &ev_ctx ))
    {
        EQERROR << "ibv_get_cq_event : " << co::base::sysError << std::endl;
        goto err;
    }

    // http://lists.openfabrics.org/pipermail/general/2008-November/055237.html
    _completions++;
    if( std::numeric_limits< unsigned int >::max( ) == _completions )
    {
        ::ibv_ack_cq_events( _cq, _completions );
        _completions = 0U;
    }

    // Solicited only!
    if( ::rdma_seterrno( ::ibv_req_notify_cq( _cq, 1 )))
    {
        EQERROR << "ibv_req_notify_cq : " << co::base::sysError << std::endl;
        goto err;
    }

#ifdef _WIN32
    // reset event
    EnterCriticalSection( &_cc->comp_channel.Lock );
    if (!_cc->comp_channel.Head)
        ResetEvent( _cc->comp_channel.Event );

    // rearm event callback
    UnregisterWait( _ccWaitObj );
    if ( !RegisterWaitForSingleObject( 
        &_ccWaitObj, 
        _cc->comp_channel.Event, 
        WAITORTIMERCALLBACK( &_triggerNotifierCM ), 
        this, 
        INFINITE, 
        WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE ))
    {
        EQERROR << "RegisterWaitForSingleObject : " << co::base::sysError 
            << std::endl;
        return false;
    }
    {
        co::base::ScopedFastWrite mutex( _eventFlagLock );
        _eventFlag &= ~MASK_CQ_EVENT;
    }
    LeaveCriticalSection ( &_cc->comp_channel.Lock );
#endif

    return true;

err:
    return false;
}

bool RDMAConnection::_checkCQ( bool drain )
{
    uint32_t num_recvs;
    int count;

    co::base::ScopedWrite poll_mutex( _poll_lock );

    if( NULL == _cq )
        goto out;

repoll:
    /* CHECK RECEIVE COMPLETIONS */
    count = ::ibv_poll_cq( _cq, _depth, _wcs );
    if( count < 0 )
    {
        EQERROR << "ibv_poll_cq : " << co::base::sysError << std::endl;
        goto err;
    }

    num_recvs = 0UL;
    for( int i = 0; i != count ; i++ )
    {
        struct ibv_wc &wc = _wcs[i];

        if( IBV_WC_SUCCESS != wc.status )
        {
            // Non-fatal.
            if( IBV_WC_WR_FLUSH_ERR == wc.status )
                continue;

            EQWARN << (void *)this << " !IBV_WC_SUCCESS : " << std::showbase
                << std::hex << "wr_id = " << wc.wr_id
                << ", status = \"" << ::ibv_wc_status_str( wc.status ) << "\""
                << std::dec << " (" << (unsigned int)wc.status << ")"
                << std::hex << ", vendor_err = " << wc.vendor_err
                << std::dec << std::endl;

            // All others are fatal.
            goto err;
        }

        EQASSERT( IBV_WC_SUCCESS == wc.status );

#ifdef _WIN32 //----------------------------------------------------------------
        // WINDOWS IBV API WORKAROUND. 
        // TODO: remove this as soon as IBV API is fixed
        if ( wc.opcode == IBV_WC_RECV && wc.wc_flags == IBV_WC_WITH_IMM )
            wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
#endif //-----------------------------------------------------------------------

        if( IBV_WC_RECV_RDMA_WITH_IMM == wc.opcode )
            _recvRDMAWrite( wc.imm_data );
        else if( IBV_WC_RECV == wc.opcode )
            _recvMessage( *reinterpret_cast< RDMAMessage * >( wc.wr_id ));
        else if( IBV_WC_SEND == wc.opcode )
            _msgbuf.freeBuffer( (void *)(uintptr_t)wc.wr_id );
        else if( IBV_WC_RDMA_WRITE == wc.opcode )
            _sourceptr.incrTail( (uint32_t)wc.wr_id );
        else
            EQUNREACHABLE;

        if( IBV_WC_RECV & wc.opcode )
        {
            _msgbuf.freeBuffer( (void *)(uintptr_t)wc.wr_id );
            // All receive completions need to be reposted.
            num_recvs++;
        }
    }

    if(( num_recvs > 0UL ) && !_postReceives( num_recvs ))
        goto err;

    if( drain && ( count > 0 ))
        goto repoll;

out:
    return true;

err:
    return false;
}

/* inline */
uint32_t RDMAConnection::_drain( void *buffer, const uint32_t bytes )
{
    const uint32_t b = std::min( bytes, _sinkptr.available( ));
    ::memcpy( buffer, (const void *)((uintptr_t)_sinkbuf.getBase( ) +
        _sinkptr.tail( )), b );
    _sinkptr.incrTail( b );
    return b;
}

/* inline */
uint32_t RDMAConnection::_fill( const void *buffer, const uint32_t bytes )
{
    uint32_t b = std::min( bytes, std::min( _sourceptr.negAvailable( ),
        _rptr.negAvailable( )));
#ifndef WRAP
    b = std::min( b, (uint32_t)(_sourcebuf.getSize() - 
                                            _sourceptr.ptr( _sourceptr.HEAD )));
#endif
    ::memcpy( (void *)((uintptr_t)_sourcebuf.getBase( ) +
        _sourceptr.ptr( _sourceptr.HEAD )), buffer, b );
    _sourceptr.incrHead( b );
    return b;
}

Connection::Notifier RDMAConnection::getNotifier() const
{ 
#ifdef _WIN32
    return _event->getNotifier();
#else
	return _notifier;
#endif
}

#ifdef _WIN32
void RDMAConnection::_triggerNotifierCQ( RDMAConnection* conn )
{
    conn->_triggerNotifierWorker( CQ_EVENT );
}

void RDMAConnection::_triggerNotifierCM( RDMAConnection* conn )
{
    conn->_triggerNotifierWorker( CM_EVENT );
}

void RDMAConnection::_triggerNotifierWorker( Events which )
{
    EQASSERT( which != BUF_EVENT );

    if ( which == CM_EVENT )
    {
        co::base::ScopedFastWrite mutex ( _eventFlagLock );
        _eventFlag |= MASK_CM_EVENT;
    }
    if ( which == CQ_EVENT )
    {
        co::base::ScopedFastWrite mutex ( _eventFlagLock );
        _eventFlag |= MASK_CQ_EVENT;
    }
    _event->set();
}
#endif

//////////////////////////////////////////////////////////////////////////////

void RDMAConnection::_showStats( )
{
    EQVERB << std::dec
        << "reads = " << _stats.reads
        << ", buffer_empty = " << _stats.buffer_empty
        << ", no_credits_fc = " << _stats.no_credits_fc
        << ", writes = " << _stats.writes
        << ", buffer_full = " << _stats.buffer_full
        << ", no_credits_rdma = " << _stats.no_credits_rdma
        << std::endl;
}

//////////////////////////////////////////////////////////////////////////////

BufferPool::BufferPool( size_t buffer_size )
    : _buffer_size( buffer_size )
    , _num_bufs( 0 )
    , _buffer( NULL )
    , _mr( NULL )
    , _ring( 0 )
{
}

BufferPool::~BufferPool( )
{
    clear( );
}

void BufferPool::clear( )
{
    _num_bufs = 0;
    _ring.clear( _num_bufs );

    if(( NULL != _mr ) && ::rdma_dereg_mr( _mr ))
        EQWARN << "rdma_dereg_mr : " << co::base::sysError << std::endl;
    _mr = NULL;

    if( NULL != _buffer )
#ifdef _WIN32
        _aligned_free( _buffer );
#else
        ::free( _buffer );
#endif
    _buffer = NULL;
}

bool BufferPool::resize( ibv_pd *pd, uint32_t num_bufs )
{
    clear( );

    if( num_bufs )
    {
        _num_bufs = num_bufs;
        _ring.clear( _num_bufs );

#ifdef _WIN32
        _buffer = _aligned_malloc((size_t)( _num_bufs * _buffer_size ),
            boost::interprocess::mapped_region::get_page_size());
        if ( !_buffer )
        {
            EQERROR << "_aligned_malloc : Couldn't allocate aligned memory. " 
                    << co::base::sysError << std::endl;
            goto err;
        }
#else
        if( ::posix_memalign( &_buffer, (size_t)::getpagesize( ),
                (size_t)( _num_bufs * _buffer_size )))
        {
            EQERROR << "posix_memalign : " << co::base::sysError << std::endl;
            goto err;
        }
#endif
        ::memset( _buffer, 0xff, (size_t)( _num_bufs * _buffer_size ));
        _mr = ::ibv_reg_mr( pd, _buffer, (size_t)( _num_bufs * _buffer_size ),
            IBV_ACCESS_LOCAL_WRITE );
        if( NULL == _mr )
        {
            EQERROR << "ibv_reg_mr : " << co::base::sysError << std::endl;
            goto err;
        }

        for( uint32_t i = 0; i != _num_bufs; i++ )
            _ring.put( i );
    }

    return true;

err:
    return false;
}

//////////////////////////////////////////////////////////////////////////////

RingBuffer::RingBuffer( int access )
    : _access( access )
    , _size( 0 )
#ifdef _WIN32
    , _mapping( 0 )
    , _map( 0 )
#else
    , _map( MAP_FAILED )
#endif
    , _mr( 0 )
{
}

RingBuffer::~RingBuffer( )
{
    clear( );
}

void RingBuffer::clear( )
{
    if(( NULL != _mr ) && ::rdma_dereg_mr( _mr ))
        EQWARN << "rdma_dereg_mr : " << co::base::sysError << std::endl;
    _mr = NULL;

#ifdef _WIN32
    if ( _map )
    {
        UnmapViewOfFile( static_cast<char*>( _map ));
        UnmapViewOfFile( static_cast<char*>( _map ) + _size );
    }
    if ( _mapping )
        CloseHandle( _mapping );

    _map = 0;
    _mapping = 0;
#else
    if(( MAP_FAILED != _map ) && ::munmap( _map, _size << 1 ))
        EQWARN << "munmap : " << co::base::sysError << std::endl;
    _map = MAP_FAILED;
#endif
    _size = 0;
}

bool RingBuffer::resize( ibv_pd *pd, size_t size )
{
    bool ok = false;
    int fd = -1;

    clear( );

    if( size )
    {
#ifdef _WIN32
        uint32_t num_retries = RINGBUFFER_ALLOC_RETRIES;
        while (!_map && num_retries-- != 0)
        {
            void *target_addr = determineViableAddr( size * 2 );
            if (target_addr)
                allocAt( size, target_addr );
        }

        if ( !_map )
        {
            EQERROR << "Couldn't allocate desired RingBuffer memory after " 
                    << RINGBUFFER_ALLOC_RETRIES << " retries." << std::endl;
        }
        else
        {
            EQINFO << "Allocated Ringbuffer memory in " 
                    << ( RINGBUFFER_ALLOC_RETRIES - num_retries ) << " tries."
                    << std::endl;
            ok = true;
        }

#else
        void *addr1, *addr2;
        char path[] = "/dev/shm/co-rdma-buffer-XXXXXX";

        _size = size;

        fd = ::mkstemp( path );
        if( fd < 0 )
        {
            EQERROR << "mkstemp : " << co::base::sysError << std::endl;
            goto out;
        }

        if( ::unlink( path ))
        {
            EQERROR << "unlink : " << co::base::sysError << std::endl;
            goto out;
        }

        if( ::ftruncate( fd, _size ))
        {
            EQERROR << "ftruncate : " << co::base::sysError << std::endl;
            goto out;
        }

        _map = ::mmap( NULL, _size << 1,
            PROT_NONE,
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0 );
        if( MAP_FAILED == _map )
        {
            EQERROR << "mmap : " << co::base::sysError << std::endl;
            goto out;
        }

        addr1 = ::mmap( _map, _size,
            PROT_READ | PROT_WRITE,
            MAP_FIXED | MAP_SHARED, fd, 0 );
        if( MAP_FAILED == addr1 )
        {
            EQERROR << "mmap : " << co::base::sysError << std::endl;
            goto out;
        }

        addr2 = ::mmap( (void *)( (uintptr_t)_map + _size ), _size,
            PROT_READ | PROT_WRITE,
            MAP_FIXED | MAP_SHARED, fd, 0 );
        if( MAP_FAILED == addr2 )
        {
            EQERROR << "mmap : " << co::base::sysError << std::endl;
            goto out;
        }

        EQASSERT( addr1 == _map );
        EQASSERT( addr2 == (void *)( (uintptr_t)_map + _size ));

#endif
#ifdef WRAP
        _mr = ::ibv_reg_mr( pd, _map, _size << 1, _access );
#else
        _mr = ::ibv_reg_mr( pd, _map, _size, _access );
#endif

        if( NULL == _mr )
        {
            EQERROR << "ibv_reg_mr : " << co::base::sysError << std::endl;
            goto out;
        }

        ::memset( _map, 0, _size );
        *reinterpret_cast< uint8_t * >( _map ) = 0x45;
        EQASSERT( 0x45 ==
            *reinterpret_cast< uint8_t * >( (uintptr_t)_map + _size ));
    }

    ok = true;

out:
#ifndef _WIN32
    if(( fd >= 0 ) && TEMP_FAILURE_RETRY( ::close( fd )))
        EQWARN << "close : " << co::base::sysError << std::endl;
#endif
    return ok;
}

#ifdef _WIN32
void* RingBuffer::determineViableAddr( size_t size )
{
    void *ptr = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!ptr)
        return 0;

    VirtualFree(ptr, 0, MEM_RELEASE);
    return ptr;
}

void RingBuffer::allocAt( size_t size, void* desiredAddr )
{
    // if we already hold one allocation, refuse to make another.
    EQASSERT( !_map );
    EQASSERT( !_mapping );
    if ( _map || _mapping )
        return;

    // is ring_size a multiple of 64k? if not, this won't ever work!
    if (( size & 0xffff ) != 0 )
        return;

    // try to allocate and map our space
    size_t alloc_size = size * 2;
    _mapping = CreateFileMappingA(  INVALID_HANDLE_VALUE, 
                                    0, 
                                    PAGE_READWRITE, 
                                    (unsigned long long)alloc_size >> 32, 
                                    alloc_size & 0xffffffffu, 
                                    0 );

    if ( !_mapping )
    {
        EQERROR << "CreateFileMappingA failed" << std::endl;
        goto err;
    }

    _map = MapViewOfFileEx( _mapping, 
                            FILE_MAP_ALL_ACCESS, 
                            0, 0, 
                            size, 
                            desiredAddr );

    if ( !_map )
    {
        EQERROR << "First MapViewOfFileEx failed" << std::endl;
        goto err;
    }

    if (!MapViewOfFileEx(   _mapping, 
                            FILE_MAP_ALL_ACCESS, 
                            0, 0, 
                            size, 
                            (char *)desiredAddr + size))
    {
        EQERROR << "Second MapViewOfFileEx failed" << std::endl;
        goto err;
    }

    _size = size;
    return;
err:
    // something went wrong - clean up
    clear();
}
#endif


} // namespace co
