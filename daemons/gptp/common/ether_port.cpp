/******************************************************************************

  Copyright (c) 2009-2012, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   3. Neither the name of the Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/

#include <ieee1588.hpp>

#include <ether_port.hpp>
#include <avbts_message.hpp>
#include <avbts_clock.hpp>

#include <avbts_oslock.hpp>
#include <avbts_osnet.hpp>
#include <avbts_oscondition.hpp>
#include <ether_tstamper.hpp>

#include <gptp_log.hpp>

#include <stdio.h>

#include <math.h>

#include <stdlib.h>

LinkLayerAddress EtherPort::other_multicast(OTHER_MULTICAST);
LinkLayerAddress EtherPort::pdelay_multicast(PDELAY_MULTICAST);
LinkLayerAddress EtherPort::test_status_multicast
( TEST_STATUS_MULTICAST );

OSThreadExitCode watchNetLinkWrapper(void *arg)
{
	EtherPort *port;

	port = (EtherPort *) arg;
	if (port->watchNetLink() == NULL)
		return osthread_ok;
	else
		return osthread_error;
}

OSThreadExitCode openPortWrapper(void *arg)
{
	EtherPort *port;

	port = (EtherPort *) arg;
	if (port->openPort(port) == NULL)
		return osthread_ok;
	else
		return osthread_error;
}

EtherPort::~EtherPort()
{
	delete port_ready_condition;
}

EtherPort::EtherPort( PortInit_t *portInit ) :
	CommonPort( portInit )
{
	linkUp = portInit->linkUp;

	pdelay_sequence_id = 0;

	pdelay_started = false;
	pdelay_halted = false;
	sync_rate_interval_timer_started = false;

	duplicate_resp_counter = 0;
	last_invalid_seqid = 0;

	initialLogPdelayReqInterval = portInit->initialLogPdelayReqInterval;
	operLogPdelayReqInterval = portInit->operLogPdelayReqInterval;
	operLogSyncInterval = portInit->operLogSyncInterval;

	if( negotiateAutomotiveSyncRateEnabled() ) {
		if (getInitSyncInterval() == LOG2_INTERVAL_INVALID)
			setInitSyncInterval( -5 );     // 31.25 ms
	} else {
		if ( getInitSyncInterval() == LOG2_INTERVAL_INVALID )
			setInitSyncInterval( -3 );       // 125 ms
	}

	if (initialLogPdelayReqInterval == LOG2_INTERVAL_INVALID)
		initialLogPdelayReqInterval = 0;   // 1 second
	if (operLogPdelayReqInterval == LOG2_INTERVAL_INVALID)
		operLogPdelayReqInterval = 0;      // 1 second
	if (operLogSyncInterval == LOG2_INTERVAL_INVALID)
		operLogSyncInterval = 0;           // 1 second

	/*TODO: Add intervals below to a config interface*/
	log_min_mean_pdelay_req_interval = initialLogPdelayReqInterval;

	last_sync = NULL;
	last_pdelay_req = NULL;
	last_pdelay_resp = NULL;
	last_pdelay_resp_fwup = NULL;

	setPdelayCount(0);
	setSyncCount(0);

	// TODO: Investigate if getPortState() can be used instead so that the
	// stationStates feature isn't dependent on using externalPortConfiguration.
	if (automotiveStationStatesEnabled()) {
		if (externalPortConfigurationEnabled() && getStaticPortState() == PTP_MASTER) {
			avbSyncState = 1;
		}
		else {
			avbSyncState = 2;
		}
		if (testModeEnabled())
		{
			linkUpCount = 1;  // TODO : really should check the current linkup status http://stackoverflow.com/questions/15723061/how-to-check-if-interface-is-up
			linkDownCount = 0;
		}
		setStationState(STATION_STATE_RESERVED);
	}
}

bool EtherPort::_init_port( void )
{
	last_pdelay_lock = lock_factory->createLock(oslock_recursive);
	port_tx_lock = lock_factory->createLock(oslock_recursive);

	pDelayIntervalTimerLock = lock_factory->createLock(oslock_recursive);

	port_ready_condition = condition_factory->createCondition();

	return true;
}

void EtherPort::startPDelay()
{
	if(!pdelayHalted()) {
		if (forceAsCapableEnabled()) {
			if (log_min_mean_pdelay_req_interval != PTPMessageSignalling::sigMsgInterval_NoSend) {
				long long unsigned int waitTime;
				waitTime = ((long long) (pow((double)2, log_min_mean_pdelay_req_interval) * 1000000000.0));
				waitTime = waitTime > EVENT_TIMER_GRANULARITY ? waitTime : EVENT_TIMER_GRANULARITY;
				pdelay_started = true;
				startPDelayIntervalTimer(waitTime);
			}
		}
		else {
			pdelay_started = true;
			reinitializeAsCapable();
			startPDelayIntervalTimer(32000000);
		}
	}
}

void EtherPort::stopPDelay()
{
	haltPdelay(true);
	pdelay_started = false;
	clock->deleteEventTimerLocked( this, PDELAY_INTERVAL_TIMEOUT_EXPIRES);
}

void EtherPort::startSyncRateIntervalTimer()
{
	if (negotiateAutomotiveSyncRateEnabled()) {
		sync_rate_interval_timer_started = true;
		if (getPortState() == PTP_MASTER) {
			// GM will wait up to 8  seconds for signaling rate
			// TODO: This isn't according to spec but set because it is believed that some slave devices aren't signalling
			//  to reduce the rate
			clock->addEventTimerLocked( this, SYNC_RATE_INTERVAL_TIMEOUT_EXPIRED, 8000000000 );
		}
		else {
			// Slave will time out after 4 seconds
			clock->addEventTimerLocked( this, SYNC_RATE_INTERVAL_TIMEOUT_EXPIRED, 4000000000 );
		}
	}
}

void EtherPort::processMessage
( char *buf, int length, LinkLayerAddress *remote, uint32_t link_speed )
{
	GPTP_LOG_VERBOSE("Processing network buffer");

	PTPMessageCommon *msg =
		buildPTPMessage( buf, (int)length, remote, this );

	if (msg == NULL)
	{
		GPTP_LOG_ERROR("Discarding invalid message");
		return;
	}
	GPTP_LOG_VERBOSE("Processing message");

	if( msg->isEvent() )
	{
		Timestamp rx_timestamp = msg->getTimestamp();
		Timestamp phy_compensation = getRxPhyDelay( link_speed );
		GPTP_LOG_DEBUG( "RX PHY compensation: %s sec",
			 phy_compensation.toString().c_str() );
		phy_compensation._version = rx_timestamp._version;
		rx_timestamp = rx_timestamp - phy_compensation;
		msg->setTimestamp( rx_timestamp );
	}

	msg->processMessage(this);
	if (msg->garbage())
		delete msg;
}

void *EtherPort::openPort( EtherPort *port )
{
	port_ready_condition->signal();

	while (1) {
		uint8_t buf[128];
		LinkLayerAddress remote;
		net_result rrecv;
		size_t length = sizeof(buf);
		uint32_t link_speed;

		if ( ( rrecv = recv( &remote, buf, length, link_speed ))
		     == net_succeed )
		{
			processMessage
				((char *)buf, (int)length, &remote, link_speed );
		} else if (rrecv == net_fatal) {
			GPTP_LOG_ERROR("read from network interface failed");
			this->processEvent(FAULT_DETECTED);
		}
	}

	return NULL;
}

net_result EtherPort::port_send
( uint16_t etherType, uint8_t *buf, int size, MulticastType mcast_type,
  PortIdentity *destIdentity, bool timestamp )
{
	LinkLayerAddress dest;

	if (mcast_type != MCAST_NONE) {
		if (mcast_type == MCAST_PDELAY) {
			dest = pdelay_multicast;
		}
		else if (mcast_type == MCAST_TEST_STATUS) {
			dest = test_status_multicast;
		}
		else {
			dest = other_multicast;
		}
	} else {
		mapSocketAddr(destIdentity, &dest);
	}

	return send(&dest, etherType, (uint8_t *) buf, size, timestamp);
}

void EtherPort::sendEventPort
( uint16_t etherType, uint8_t *buf, int size, MulticastType mcast_type,
  PortIdentity *destIdentity, uint32_t *link_speed )
{
	net_result rtx = port_send
		( etherType, buf, size, mcast_type, destIdentity, true );
	if( rtx != net_succeed )
	{
		GPTP_LOG_ERROR("sendEventPort(): failure");
		return;
	}

	*link_speed = this->getLinkSpeed();

	return;
}

void EtherPort::sendGeneralPort
( uint16_t etherType, uint8_t *buf, int size, MulticastType mcast_type,
  PortIdentity * destIdentity )
{
	net_result rtx = port_send(etherType, buf, size, mcast_type, destIdentity, false);
	if (rtx != net_succeed) {
		GPTP_LOG_ERROR("sendGeneralPort(): failure");
	}

	return;
}

bool EtherPort::_processEvent( Event e )
{
	bool ret;

	switch (e) {
	case POWERUP:
	case INITIALIZE:
		if( getLinkUpState() ) {
			GPTP_LOG_STATUS("Starting PDelay");
			startPDelay();
		}

		port_ready_condition->wait_prelock();

		if( !linkWatch(watchNetLinkWrapper, (void *)this) ) {
			GPTP_LOG_ERROR("Error creating port link thread");
			ret = false;
			break;
		}

		if( !linkOpen(openPortWrapper, (void *)this) ) {
			GPTP_LOG_ERROR("Error creating port thread");
			ret = false;
			break;
		}

		port_ready_condition->wait();

		if( automotiveStationStatesEnabled() ) {
			setStationState(STATION_STATE_ETHERNET_READY);
		}

		if (testModeEnabled()) {
			APMessageTestStatus *testStatusMsg = new APMessageTestStatus(this);
			if (testStatusMsg) {
				testStatusMsg->sendPort(this);
				delete testStatusMsg;
			}
		}

		// TODO: Test if the regular port state can be used instead of the static
		// port state. If it's truly static, then it should already be set. Also,
		// that would make it possible to use negotiateAutomotiveSyncRate without
		// externalPortConfiguration enabled.
		if( negotiateAutomotiveSyncRateEnabled() &&
		    externalPortConfigurationEnabled() &&
			 getStaticPortState() == PTP_SLAVE ) {
			// Send an initial signalling message
			PTPMessageSignalling *sigMsg = new PTPMessageSignalling(this);
			if (sigMsg) {
				sigMsg->setintervals(PTPMessageSignalling::sigMsgInterval_NoSend, getSyncInterval(), PTPMessageSignalling::sigMsgInterval_NoSend);
				sigMsg->sendPort(this, NULL);
				delete sigMsg;
			}

			startSyncReceiptTimer((unsigned long long)
				 (SYNC_RECEIPT_TIMEOUT_MULTIPLIER *
				  ((double) pow((double)2, getSyncInterval()) *
					1000000000.0)));
		}

		ret = true;
		break;
	case STATE_CHANGE_EVENT:
		// If externalPortConfiguration is enabled, handle the event by
		// doing nothing and returning true, preventing the default
		// action from executing
		if( externalPortConfigurationEnabled() )
			ret = true;
		else
			ret = false;

		break;
	case LINKUP:
		stopPDelay();
		haltPdelay(false);
		startPDelay();
		GPTP_LOG_STATUS("LINKUP");

		if( clock->getPriority1() == 255 || getPortState() == PTP_SLAVE ) {
			becomeSlave( true );
		} else if( getPortState() == PTP_MASTER ) {
			becomeMaster( true );
		} else {
			clock->addEventTimerLocked(this, ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES,
				ANNOUNCE_RECEIPT_TIMEOUT_MULTIPLIER * pow(2.0, getAnnounceInterval()) * 1000000000.0);
		}

		if( automotiveStationStatesEnabled() ) {
			setStationState(STATION_STATE_ETHERNET_READY);
			// Start AVB SYNC at 2. It will decrement after each sync. When it reaches 0 the Test Status message
			// can be sent
			if ( getPortState() == PTP_MASTER ) {
				avbSyncState = 1;
			}
			else {
				avbSyncState = 2;
			}
		}

		if (testModeEnabled())
		{
			APMessageTestStatus *testStatusMsg = new APMessageTestStatus(this);
			if (testStatusMsg) {
				testStatusMsg->sendPort(this);
				delete testStatusMsg;
			}
		}

		// Reset send intervals to initial values
		resetInitSyncInterval();
		setAnnounceInterval( 0 );
		log_min_mean_pdelay_req_interval = initialLogPdelayReqInterval;

		if( negotiateAutomotiveSyncRateEnabled() && getPortState() == PTP_SLAVE ) {
			// Send an initial signaling message
			PTPMessageSignalling *sigMsg = new PTPMessageSignalling(this);
			if (sigMsg) {
				sigMsg->setintervals(PTPMessageSignalling::sigMsgInterval_NoSend, getSyncInterval(), PTPMessageSignalling::sigMsgInterval_NoSend);
				sigMsg->sendPort(this, NULL);
				delete sigMsg;
			}

			startSyncReceiptTimer((unsigned long long)
				 (SYNC_RECEIPT_TIMEOUT_MULTIPLIER *
				  ((double) pow((double)2, getSyncInterval()) *
					1000000000.0)));
		}

		// Reset Sync count and pdelay count
		setPdelayCount(0);
		setSyncCount(0);

		if (testModeEnabled())
		{
			linkUpCount++;
		}
		this->timestamper_reset();

		ret = true;
		break;
	case LINKDOWN:
		stopPDelay();
		GPTP_LOG_STATUS("LINK DOWN");

		if( !forceAsCapableEnabled() ) {
			setAsCapable(false);
		}
		if (testModeEnabled())
		{
			linkDownCount++;
		}

		ret = true;
		break;
	case ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES:
	case SYNC_RECEIPT_TIMEOUT_EXPIRES:
		if( !externalPortConfigurationEnabled() )
		{
			ret = false;
			break;
		} else {
			// If externalPortConfiguration is enabled, just reset the sync
			// receipt timer.
			// TODO: Investigate not even starting this time if
			// externalPortConfiguration is enabled so gptp doesn't have to ignore
			// and reset it constantly.
			if (e == SYNC_RECEIPT_TIMEOUT_EXPIRES) {
				GPTP_LOG_EXCEPTION("SYNC receipt timeout");

				startSyncReceiptTimer((unsigned long long)
								(SYNC_RECEIPT_TIMEOUT_MULTIPLIER *
								 ((double) pow((double)2, getSyncInterval()) *
							1000000000.0)));
			}
			ret = true;
			break;
		}
	case PDELAY_INTERVAL_TIMEOUT_EXPIRES:
		GPTP_LOG_DEBUG("PDELAY_INTERVAL_TIMEOUT_EXPIRES occured");
		{
			Timestamp req_timestamp;

			if (getLastPDelayLock() != true) {
				GPTP_LOG_ERROR("Failed to get last PDelay lock before sending a PDelayReq");
				break;
			}

			PTPMessagePathDelayReq *pdelay_req =
			    new PTPMessagePathDelayReq(this);
			PortIdentity dest_id;
			getPortIdentity(dest_id);
			pdelay_req->setPortIdentity(&dest_id);

			{
				Timestamp pending =
				    PDELAY_PENDING_TIMESTAMP;
				pdelay_req->setTimestamp(pending);
			}

			if (last_pdelay_req != NULL) {
				delete last_pdelay_req;
			}
			setLastPDelayReq(pdelay_req);

			getTxLock();
			pdelay_req->sendPort(this, NULL);
			GPTP_LOG_DEBUG("*** Sent PDelay Request message");
			putTxLock();

			{
				long long timeout;
				long long interval;

				timeout = PDELAY_RESP_RECEIPT_TIMEOUT_MULTIPLIER *
					((long long)
					 (pow((double)2,getPDelayInterval())*1000000000.0));

				timeout = timeout > EVENT_TIMER_GRANULARITY ?
					timeout : EVENT_TIMER_GRANULARITY;
				clock->addEventTimerLocked
					(this, PDELAY_RESP_RECEIPT_TIMEOUT_EXPIRES, timeout );
				GPTP_LOG_DEBUG("Schedule PDELAY_RESP_RECEIPT_TIMEOUT_EXPIRES, "
					"PDelay interval %d, timeout %lld",
					getPDelayInterval(), timeout);

				interval =
					((long long)
					 (pow((double)2,getPDelayInterval())*1000000000.0));
				interval = interval > EVENT_TIMER_GRANULARITY ?
					interval : EVENT_TIMER_GRANULARITY;
				startPDelayIntervalTimer(interval);
			}

			putLastPDelayLock();
		}
		break;
	case SYNC_INTERVAL_TIMEOUT_EXPIRES:
		{
			/* Set offset from master to zero, update device vs
			   system time offset */

			// Send a sync message and then a followup to broadcast
			PTPMessageSync *sync = new PTPMessageSync(this);
			PortIdentity dest_id;
			bool tx_succeed;
			getPortIdentity(dest_id);
			sync->setPortIdentity(&dest_id);
			getTxLock();
			tx_succeed = sync->sendPort(this, NULL);
			GPTP_LOG_DEBUG("Sent SYNC message");

			if ( automotiveStationStatesEnabled() &&
			     getPortState() == PTP_MASTER )
			{
				if (avbSyncState > 0) {
					avbSyncState--;
					if (avbSyncState == 0) {
						// Send Avnu Automotive Profile status message
						setStationState(STATION_STATE_AVB_SYNC);
						if (testModeEnabled()) {
							APMessageTestStatus *testStatusMsg = new APMessageTestStatus(this);
							if (testStatusMsg) {
								testStatusMsg->sendPort(this);
								delete testStatusMsg;
							}
						}
					}
				}
			}
			putTxLock();

			if ( tx_succeed )
			{
				Timestamp sync_timestamp = sync->getTimestamp();

				GPTP_LOG_VERBOSE("Successful Sync timestamp");
				GPTP_LOG_VERBOSE("Seconds: %u",
						 sync_timestamp.seconds_ls);
				GPTP_LOG_VERBOSE("Nanoseconds: %u",
						 sync_timestamp.nanoseconds);

				PTPMessageFollowUp *follow_up = new PTPMessageFollowUp(this);
				PortIdentity dest_id;
				getPortIdentity(dest_id);

				follow_up->setClockSourceTime(getClock()->getFUPInfo());
				follow_up->setPortIdentity(&dest_id);
				follow_up->setSequenceId(sync->getSequenceId());
				follow_up->setPreciseOriginTimestamp
					(sync_timestamp);
				follow_up->sendPort(this, NULL);
				delete follow_up;
			} else {
				GPTP_LOG_ERROR
					("*** Unsuccessful Sync timestamp");
			}
			delete sync;
		}
		break;
	case FAULT_DETECTED:
		GPTP_LOG_ERROR("Received FAULT_DETECTED event");
		if (!forceAsCapableEnabled()) {
			setAsCapable(false);
		}
		break;
	case PDELAY_DEFERRED_PROCESSING:
		GPTP_LOG_DEBUG("PDELAY_DEFERRED_PROCESSING occured");
		if (getLastPDelayLock() != true) {
			GPTP_LOG_ERROR("Failed to get last PDelay lock before processing a deferred PDelay Follow Up");
			break;
		}
		if (last_pdelay_resp_fwup == NULL) {
			GPTP_LOG_ERROR("PDelay Response Followup is NULL!");
			abort();
		}
		last_pdelay_resp_fwup->processMessage(this);
		if (last_pdelay_resp_fwup->garbage()) {
			delete last_pdelay_resp_fwup;
			this->setLastPDelayRespFollowUp(NULL);
		}
		putLastPDelayLock();
		break;
	case PDELAY_RESP_RECEIPT_TIMEOUT_EXPIRES:
		if (!forceAsCapableEnabled()) {
			GPTP_LOG_DEBUG("PDelay Response Receipt Timeout");
			if ( getAsCapable() || !getAsCapableEvaluated() ) {
				GPTP_LOG_STATUS("Did not receive a valid PDelay Response before the timeout. Not AsCapable");
			}
			setAsCapable(false);
		}
		setPdelayCount( 0 );
		break;

	case PDELAY_RESP_PEER_MISBEHAVING_TIMEOUT_EXPIRES:
		GPTP_LOG_EXCEPTION("PDelay Resp Peer Misbehaving timeout expired! Restarting PDelay");

		haltPdelay(false);
		if( getPortState() != PTP_SLAVE &&
		    getPortState() != PTP_MASTER )
		{
			GPTP_LOG_STATUS("Starting PDelay" );
			startPDelay();
		}
		break;
	case SYNC_RATE_INTERVAL_TIMEOUT_EXPIRED:
		{
			GPTP_LOG_INFO("SYNC_RATE_INTERVAL_TIMEOUT_EXPIRED occured");

			sync_rate_interval_timer_started = false;

			bool intervalUpdated = false;
			if ( getSyncInterval() != operLogSyncInterval )
			{
				setSyncInterval( operLogSyncInterval );
				intervalUpdated = true;
			}

			if (log_min_mean_pdelay_req_interval != operLogPdelayReqInterval) {
				log_min_mean_pdelay_req_interval = operLogPdelayReqInterval;
				intervalUpdated = true;
			}

			if (intervalUpdated && getPortState() == PTP_SLAVE) {
				// Send operational signalling message
				PTPMessageSignalling *sigMsg = new PTPMessageSignalling(this);
				if (sigMsg) {
					if (negotiateAutomotiveSyncRateEnabled()) {
						sigMsg->setintervals(PTPMessageSignalling::sigMsgInterval_NoChange, getSyncInterval(), PTPMessageSignalling::sigMsgInterval_NoChange);
					} else {
						sigMsg->setintervals(log_min_mean_pdelay_req_interval, getSyncInterval(), PTPMessageSignalling::sigMsgInterval_NoChange);
					}
					sigMsg->sendPort(this, NULL);
					delete sigMsg;
				}

				startSyncReceiptTimer((unsigned long long)
					(SYNC_RECEIPT_TIMEOUT_MULTIPLIER *
					 ((double) pow((double)2, getSyncInterval()) *
					  1000000000.0)));
				}
			}

		break;
	default:
		GPTP_LOG_ERROR
		  ( "Unhandled event type in "
		    "EtherPort::processEvent(), %d", e );
		ret = false;
		break;
	}

	return ret;
}

void EtherPort::recoverPort( void )
{
	return;
}

void EtherPort::becomeMaster( bool annc ) {
	setPortState( PTP_MASTER );
	// Stop announce receipt timeout timer
	if( transmitAnnounceEnabled() ) {
		clock->deleteEventTimerLocked( this, ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES );
	}

	// Stop sync receipt timeout timer
	stopSyncReceiptTimer();

	if( externalPortConfigurationEnabled() &&
		 getStaticPortState() == PTP_MASTER ) {
		// Set grandmaster info to myself
		ClockIdentity clock_identity;
		unsigned char priority1;
		unsigned char priority2;
		ClockQuality clock_quality;

		clock_identity = getClock()->getClockIdentity();
		getClock()->setGrandmasterClockIdentity(clock_identity);
		priority1 = getClock()->getPriority1();
		getClock()->setGrandmasterPriority1(priority1);
		priority2 = getClock()->getPriority2();
		getClock()->setGrandmasterPriority2(priority2);
		clock_quality = getClock()->getClockQuality();
		getClock()->setGrandmasterClockQuality(clock_quality);
	}

	// TOOD: Is the annc parameter needed anymore?
	if( annc && transmitAnnounceEnabled()) {
		startAnnounce();
	}
	startSyncIntervalTimer(16000000);
	GPTP_LOG_STATUS("Switching to Master" );

	clock->updateFUPInfo();

	return;
}

void EtherPort::becomeSlave( bool restart_syntonization ) {
	clock->deleteEventTimerLocked( this, ANNOUNCE_INTERVAL_TIMEOUT_EXPIRES );
	clock->deleteEventTimerLocked( this, SYNC_INTERVAL_TIMEOUT_EXPIRES );

	setPortState( PTP_SLAVE );

	if( !externalPortConfigurationEnabled() ) {
		clock->addEventTimerLocked
		  (this, ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES,
		   (ANNOUNCE_RECEIPT_TIMEOUT_MULTIPLIER*
			(unsigned long long)
			(pow((double)2,getAnnounceInterval())*1000000000.0)));
	} else {
		// When externalPortConfiguration is enabled as a slave, we might receive
		// grandmaster info in the future from an Announce message, but that will
		// not occur if the grandmaster does not transmit Announce
		// (e.g. transmitAnnounce=false). Therefore, we need to initialize the
		// parentDS.grandmaster members as follows:
		//- If there a value for "unknown", use that, otherwise
		//  - If the member indicates whether this is the best GM,
		//      use a value that represents best GM.
		//  - If the member represents quality,
		//      use the worst value that is conformant, since we don't know.
		ClockIdentity clock_identity;
		ClockQuality clock_quality;

		// The default constructor for ClockIdentity uses all-zero.
		// For 1588 and 802.1AS, all-zero is a special value that is
		// invalid/unknown. The only other value we could use is all-one,
		// but since that can also mean "all clocks", it is not appropriate.
		getClock()->setGrandmasterClockIdentity(clock_identity);
		// Using zero for the priorities indicates that the remote GM is the
		// best, which is true since we are externally configuring it to be GM.
		getClock()->setGrandmasterPriority1(0);
		getClock()->setGrandmasterPriority2(0);
		// 802.1AS-2011 8.6.2.2, value for unknown
		clock_quality.cq_class = 248;
		// 802.1AS-2011 8.6.2.3, value for unknown
		clock_quality.clockAccuracy = 0xFE;
		// 802.1AS-2011 8.6.2.3, value for unknown, and also worst conformant
		clock_quality.offsetScaledLogVariance = 0x4100;
		getClock()->setGrandmasterClockQuality(clock_quality);
	}

	GPTP_LOG_STATUS("Switching to Slave" );
	if( restart_syntonization ) clock->newSyntonizationSetPoint();

	getClock()->updateFUPInfo();

	return;
}

void EtherPort::mapSocketAddr
( PortIdentity *destIdentity, LinkLayerAddress *remote )
{
	*remote = identity_map[*destIdentity];
	return;
}

void EtherPort::addSockAddrMap
( PortIdentity *destIdentity, LinkLayerAddress *remote )
{
	identity_map[*destIdentity] = *remote;
	return;
}

int EtherPort::getTxTimestamp
( PTPMessageCommon *msg, Timestamp &timestamp, unsigned &counter_value,
  bool last )
{
	PortIdentity identity;
	msg->getPortIdentity(&identity);
	return getTxTimestamp
		(&identity, msg->getMessageId(), timestamp, counter_value, last);
}

int EtherPort::getRxTimestamp
( PTPMessageCommon * msg, Timestamp & timestamp, unsigned &counter_value,
  bool last )
{
	PortIdentity identity;
	msg->getPortIdentity(&identity);
	return getRxTimestamp
		(&identity, msg->getMessageId(), timestamp, counter_value, last);
}

int EtherPort::getTxTimestamp
(PortIdentity *sourcePortIdentity, PTPMessageId messageId,
 Timestamp &timestamp, unsigned &counter_value, bool last )
{
	EtherTimestamper *timestamper =
		dynamic_cast<EtherTimestamper *>(_hw_timestamper);
	if (timestamper)
	{
		return timestamper->HWTimestamper_txtimestamp
			( sourcePortIdentity, messageId, timestamp,
			  counter_value, last );
	}
	timestamp = clock->getSystemTime();
	return 0;
}

int EtherPort::getRxTimestamp
( PortIdentity * sourcePortIdentity, PTPMessageId messageId,
  Timestamp &timestamp, unsigned &counter_value, bool last )
{
	EtherTimestamper *timestamper =
		dynamic_cast<EtherTimestamper *>(_hw_timestamper);
	if (timestamper)
	{
		return timestamper->HWTimestamper_rxtimestamp
		    (sourcePortIdentity, messageId, timestamp, counter_value,
		     last);
	}
	timestamp = clock->getSystemTime();
	return 0;
}

void EtherPort::startPDelayIntervalTimer
( long long unsigned int waitTime )
{
	pDelayIntervalTimerLock->lock();
	clock->deleteEventTimerLocked(this, PDELAY_INTERVAL_TIMEOUT_EXPIRES);
	clock->addEventTimerLocked(this, PDELAY_INTERVAL_TIMEOUT_EXPIRES, waitTime);
	pDelayIntervalTimerLock->unlock();
}

void EtherPort::syncDone() {
	GPTP_LOG_VERBOSE("Sync complete");

	if (automotiveStationStatesEnabled() && getPortState() == PTP_SLAVE) {
		if (avbSyncState > 0) {
			avbSyncState--;
			if (avbSyncState == 0) {
				setStationState(STATION_STATE_AVB_SYNC);
				if (testModeEnabled()) {
					APMessageTestStatus *testStatusMsg =
						new APMessageTestStatus(this);
					if (testStatusMsg) {
						testStatusMsg->sendPort(this);
						delete testStatusMsg;
					}
				}
			}
		}
	}

	if (negotiateAutomotiveSyncRateEnabled()) {
		if (!sync_rate_interval_timer_started) {
			if ( getSyncInterval() != operLogSyncInterval )
			{
				startSyncRateIntervalTimer();
			}
		}
	}

	if( !pdelay_started && getLinkUpState() ) {
		startPDelay();
	}
}