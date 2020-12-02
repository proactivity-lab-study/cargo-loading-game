/**
 *
 * TODO reminder to use hton and ntoh functions to assign variable values
 * 		larger than a byte in network messages!! 
 *
 * 
 * 
 *
 * 
 * 
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#include "cmsis_os2.h"

#include <stdlib.h>

#include "mist_comm_am.h"
#include "radio.h"
#include "endianness.h"

#include "ship_strategy.h"
#include "crane_control.h"
#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"
#include "game_status.h"

#include "loglevels.h"
#define __MODUUL__ "sstrt"
#define __LOG_LEVEL__ (LOG_LEVEL_ship_strategy & BASE_LOG_LEVEL)
#include "log.h"

enum {
	START_COOP_MSG_ID = 130,
	ANS_COOP_MSG_ID,
	SEL_COOP_MSG_ID,
	CONFIRM_MSG_ID
};

enum {
	COOP_SEARCHING,
	COOP_PENDING,
	COOP_ACTIVE
};

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t coop_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;

static am_addr_t coop_partner, coop_destination; // Protected by coop_mutex
uint8_t coop_status; // Protected by coop_mutex

static void startCoop(void *args);
static void manageCoop(void *args);
static void sendMsg(void *args);
am_addr_t get_nearest_n();
static uint16_t calcDistance(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
//osEventFlagsId_t evt_id;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
	coop_mutex = osMutexNew(NULL); // Protects cooperation parameter values
	snd_msg_qID = osMessageQueueNew(9, sizeof(coop_msg_t), NULL); 
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	setXFirst(true);
	setAlwaysPlaceCargo(true);
	setCraneTactics(cc_do_nothing, 0, getShipLocation(0));

	while(osMutexAcquire(coop_mutex, 1000) != osOK);
	coop_partner = my_address;
	coop_destination = my_address;
	coop_status = COOP_SEARCHING; // This is just a place-holder
	osMutexRelease(coop_mutex);
	
	osThreadNew(startCoop, NULL, NULL); // Send start cooperation thread
	osThreadNew(manageCoop, NULL, NULL); // Manages active cooperation
	snd_task_id = osThreadNew(sendMsg, NULL, NULL); // Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void startCoop(void *args)
{
	coop_msg_t cmsg;
	am_addr_t dest;
	
	for(;;)
	{
		osDelay(10*osKernelGetTickFreq()); // 10 seconds
		dest = get_nearest_n();

		while(osMutexAcquire(coop_mutex, 1000) != osOK);
		coop_status = COOP_SEARCHING;
		coop_partner = my_address;
		coop_destination = my_address;
		if(coop_partner != dest) // If nearest neighbour has changed, send start cooperation message
		{
			info1("start snd coop");
			cmsg.messageID = START_COOP_MSG_ID;
			cmsg.senderAddr = dest; // Piggy-backing destination address here
			cmsg.coopAddr = dest;
			cmsg.agreement = true;
			if(dest != my_address)osMessageQueuePut(snd_msg_qID, &cmsg, 0, 0);
		}
		osMutexRelease(coop_mutex);
	}
}

static void manageCoop(void *args)
{
	uint8_t stat;
	
	for(;;)
	{
		osDelay(2*osKernelGetTickFreq()); // 2 seconds

		while(osMutexAcquire(coop_mutex, 1000) != osOK);
		if(coop_status == COOP_ACTIVE)
		{
			stat = getCargoStatus(coop_destination);
			if(stat == 0)
			{
				if(coop_destination == my_address)
				{
					stat = getCargoStatus(coop_partner);
					if(stat != 0)
					{
						coop_destination = coop_partner;
						setXFirst(true);
						setAlwaysPlaceCargo(true);
						setCraneTactics(cc_to_address, coop_destination, getShipLocation(coop_destination));
					}
					else 
					{
						setCraneTactics(cc_do_nothing, 0, getShipLocation(0)); // All done!
						// coop_status = COOP_SEARCHING; // In case new ships enter game, we can partner up!
					}
				}
				else
				{
					stat = getCargoStatus(coop_destination);
					if(stat != 0)
					{
						coop_destination = my_address;
						setXFirst(true);
						setAlwaysPlaceCargo(true);
						setCraneTactics(cc_to_address, coop_destination, getShipLocation(coop_destination));
					}
					else 
					{
						setCraneTactics(cc_do_nothing, 0, getShipLocation(0)); // All done!
						// coop_status = COOP_SEARCHING; // In case new ships enter game, we can partner up!
					}
				}
			} // Cooperation destination has not received cargo yet
		} // No active cooperation
		osMutexRelease(coop_mutex);
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ShipReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	uint16_t m_dist, n_dist;
	uint8_t m_stat, n_stat;
	am_addr_t nearest;
	coop_msg_t amsg;
	coop_msg_t *smsg;

	switch(rmsg[0])
	{
		case START_COOP_MSG_ID :
			info1("rcvd start");
			smsg = (coop_msg_t *) comms_get_payload(comms, msg, pl_len);
			nearest = get_nearest_n();
			
			if(nearest == smsg->senderAddr)
			{
				info1("agree");
				amsg.coopAddr = 0;
				amsg.agreement = true;

				m_dist = distToCrane(getShipLocation(my_address));
				n_dist = distToCrane(getShipLocation(smsg->senderAddr));
				m_stat = getCargoStatus(my_address);
				n_stat = getCargoStatus(smsg->senderAddr);

				if(n_dist < m_dist)
				{
					if(n_stat != 0)amsg.coopAddr = smsg->senderAddr;
					else 
					{
						if(m_stat != 0)amsg.coopAddr = my_address;
						else amsg.agreement = false; // Both have cargo
					}
				}
				else 
				{
					if(m_stat != 0)amsg.coopAddr = my_address;
					else 
					{
						if(n_stat != 0)amsg.coopAddr = smsg->senderAddr;
						else amsg.agreement = false; // Both have cargo
					}
				}

				if(amsg.coopAddr != 0)
				{
					while(osMutexAcquire(coop_mutex, 1000) != osOK);
					coop_partner = smsg->senderAddr;
					coop_status = COOP_PENDING;
					coop_destination = amsg.coopAddr;
					osMutexRelease(coop_mutex);
				}
			}
			else
			{
				amsg.coopAddr = my_address; // Otherwise it is left floating
				amsg.agreement = false;
				info1("no dice");
			}

			amsg.messageID = ANS_COOP_MSG_ID;
			amsg.senderAddr = smsg->senderAddr; // Piggy-backing destination address here
			osMessageQueuePut(snd_msg_qID, &amsg, 0, 0);
			break;

		case ANS_COOP_MSG_ID :
			info1("rcvd ans");
			smsg = (coop_msg_t *)comms_get_payload(comms, msg, pl_len);
			nearest = get_nearest_n();
			if(nearest == smsg->senderAddr)
			{
				if(smsg->agreement)
				{
					amsg.agreement = true;
					m_dist = distToCrane(getShipLocation(my_address));
					n_dist = distToCrane(getShipLocation(smsg->senderAddr));
					m_stat = getCargoStatus(my_address);
					n_stat = getCargoStatus(smsg->senderAddr);
 
					if(n_dist < m_dist)
					{
						if(smsg->coopAddr == smsg->senderAddr)
						{
							if(n_stat != 0)amsg.coopAddr = smsg->senderAddr;
							else amsg.agreement = false; // Already has cargo
						}
						else amsg.agreement = false; // No Agreement
					}
					else
					{
						if(smsg->coopAddr == my_address)
						{
							if(m_stat != 0)amsg.coopAddr = my_address;
							else amsg.agreement = false; // I already have cargo
						}
						else amsg.agreement = false; // No Agreement
					}
				}
				else ; // Nearest neigbour does not agree, nothing to do, try again later
			}
			else 
			{
				// Not my nearest neighbour, respond with disagreement and wait for startCoop 
				// thread to send new start cooperation message to nearest neighbour.
				amsg.agreement = false;
			}

			if(amsg.agreement)
			{
				while(osMutexAcquire(coop_mutex, 1000) != osOK);
				coop_status = COOP_ACTIVE;
				coop_partner = smsg->senderAddr;
				coop_destination = amsg.coopAddr;
				setCraneTactics(cc_to_address, coop_destination, getShipLocation(coop_destination));
				osMutexRelease(coop_mutex);
				setXFirst(true);
				setAlwaysPlaceCargo(true);			
				info1("coop active!");
			}
			else amsg.coopAddr = my_address; // Otherwise it's left floating
			
			amsg.messageID = CONFIRM_MSG_ID;
			amsg.senderAddr = smsg->senderAddr; // Piggy-backing destination address here
			osMessageQueuePut(snd_msg_qID, &amsg, 0, 0);
			break;

		case CONFIRM_MSG_ID : 
			info1("rcvd conf");
			coop_msg_t * smsg = (coop_msg_t *)comms_get_payload(comms, msg, pl_len);
			
			while(osMutexAcquire(coop_mutex, 1000) != osOK);
			if(smsg->coopAddr == coop_partner)
			{
				if(smsg->agreement)
				{
					coop_status = COOP_ACTIVE;
					setXFirst(true);
					setAlwaysPlaceCargo(true);
					setCraneTactics(cc_to_address, coop_destination, getShipLocation(coop_destination));
				}
				else 
				{
					coop_status = COOP_SEARCHING; // try again later
					coop_partner = my_address;
					coop_destination = my_address;
				}
			}
			else ; 	// Someone confirming me unexpectedly. Drop this message and wait for partner
					// or startCoop thread to clear COOP_PENDING flag. 
			osMutexRelease(coop_mutex);
		default:
			break;
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radioSendDone(comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt-ss %u", result);
    osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void sendMsg(void *args)
{
	coop_msg_t packet;
	am_addr_t dest;

	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
	
		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(sradio, &m_msg);
		coop_msg_t * smsg = comms_get_payload(sradio, &m_msg, sizeof(coop_msg_t));
		if (smsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}
		smsg->messageID = packet.messageID;
		smsg->senderAddr = my_address; // Set true sender address
		dest = packet.senderAddr; // This is actually the destination
		smsg->coopAddr = packet.coopAddr;
		smsg->agreement = packet.agreement;



		// Send data packet
	    comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
	    comms_am_set_destination(sradio, &m_msg, dest);
	    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(sradio, &m_msg, sizeof(coop_msg_t));

	    comms_error_t result = comms_send(sradio, &m_msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Selects ship closest to me
// In case of tie, selects first one found
// If no ships returns own address
// TODO ship_in_game must be used
am_addr_t get_nearest_n()
{
	am_addr_t ship_addresses[MAX_SHIPS], saddr;
	uint8_t num_ships, i;
	uint16_t dist, smallest_dist;

	loc_bundle_t sloc, my_loc;

	num_ships = getAllShipsAddr(ship_addresses, MAX_SHIPS);
	
	saddr = my_address;

	if(num_ships > 0)
	{
		my_loc = getShipLocation(my_address);

		sloc = getShipLocation(ship_addresses[0]);
		smallest_dist = calcDistance(sloc.x, sloc.y, my_loc.x, my_loc.y);
		saddr = ship_addresses[0];

		for(i=1;i<num_ships;i++)
		{
			sloc = getShipLocation(ship_addresses[i]);
			dist = calcDistance(sloc.x, sloc.y, my_loc.x, my_loc.y);
			if(dist < smallest_dist)
			{
				smallest_dist = dist;
				saddr = ship_addresses[i];
			}
		}
	}
	else ;//no ships beside me return my address
	
	return saddr;
}

static uint16_t calcDistance(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
	return abs(x2 - x1) + abs(y2 - y1);
}










