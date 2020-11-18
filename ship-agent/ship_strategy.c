/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#include "cmsis_os2.h"

#include <stdlib.h>

#include "mist_comm_am.h"
#include "radio.h"

#include "ship_strategy.h"
#include "clg_comm.h"
#include "game_types.h"
#include "game_status.h"

#include "loglevels.h"
#define __MODUUL__ "shipstrategy"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

#define START_COOP_MSG_ID 130

static osMutexId_t snd_mutex;
static osMessageQueueId_t snd_msg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* sradio;
am_addr_t my_address;

bool no_coop = false;

void start_coop(void *args);
void send_msg(void *args);
am_addr_t get_nearest_n();
static uint16_t calcDistance(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
//osEventFlagsId_t evt_id;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_ship_strategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_mutex = osMutexNew(NULL);	//protects against sending another message before hardware has handled previous message
	//evt_id = osEventFlagsNew(NULL);	//tells get_all_ships_data to quiery the next ship

	snd_msg_qID = osMessageQueueNew(9, sizeof(queryMsg), NULL);
	
	sradio = radio;//this is the only write, so not going to protect it with mutex
	my_address = addr;//this is the only write, so not going to protect it with mutex

	osThreadNew(start_coop, NULL, NULL);//sends welcome message
}

/**********************************************************************************************
 *	 some threads
 *********************************************************************************************/

void start_coop(void *args)
{
	coop_msg_t cmsg;
	am_addr_t dest;
	
	for(;;)
	{
		osDelay(10*osKernelGetTickFreq()); //10 seconds
		if(!no_coop)
		{
			cmsg.messageID = START_COOP_MSG_ID;
			cmsg.senderAddr = my_address;
			dest = get_nearest_n();
			cmsg.destinationAddr = dest;

			//TODO put msg to queue
		}
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ship_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	
	info1("command rcvd");
	debug1("rcv-c");
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
    while(osMutexAcquire(snd_mutex, 1000) != osOK);
    m_sending = false;
    osMutexRelease(snd_mutex);
}

void send_msg(void *args)
{
	queryMsg packet;
	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
		while(osMutexAcquire(snd_mutex, 1000) != osOK);
		if(!m_sending)
		{
			comms_init_message(sradio, &m_msg);
			queryMsg * qmsg = comms_get_payload(sradio, &m_msg, sizeof(queryMsg));
			if (qmsg == NULL)
			{
				return ;
			}
			qmsg->messageID = packet.messageID;
			qmsg->messageID = packet.senderAddr;
			qmsg->messageID = packet.shipAddr;

			// Send data packet
		    comms_set_packet_type(sradio, &m_msg, AMID_SYSTEMCOMMUNICATION);
		    comms_am_set_destination(sradio, &m_msg, SYSTEM_ID);//TODO resolv system ID value
		    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
		    comms_set_payload_length(sradio, &m_msg, sizeof(queryMsg));

		    comms_error_t result = comms_send(sradio, &m_msg, radio_send_done, NULL);
		    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
		    if (COMMS_SUCCESS == result)
		    {
		        m_sending = true;
		    }
		}
		osMutexRelease(snd_mutex);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

am_addr_t get_nearest_n()
{
	am_addr_t ship_addresses[MAX_SHIPS], saddr;
	uint8_t num_ships, i;
	uint16_t dist, smallest_dist;

	loc_bundle_t sloc, my_loc;

	num_ships = get_all_ship_addr(ship_addresses);
	
	saddr = my_address;

	if(num_ships > 0)
	{
		my_loc = get_ship_location(my_address);

		sloc = get_ship_location(ship_addresses[0]);
		smallest_dist = calcDistance(sloc.x, sloc.y, my_loc.x, my_loc.y);
		saddr = ship_addresses[0];

		for(i=1;i<num_ships;i++)
		{
			sloc = get_ship_location(ship_addresses[i]);
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










