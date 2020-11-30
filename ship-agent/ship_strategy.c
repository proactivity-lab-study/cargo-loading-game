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
#include "clg_comm.h"
#include "game_types.h"
#include "game_status.h"

#include "loglevels.h"
#define __MODUUL__ "sstrt"
#define __LOG_LEVEL__ (LOG_LEVEL_ship_strategy & BASE_LOG_LEVEL)
#include "log.h"

#define START_COOP_MSG_ID 130
#define ANS_COOP_MSG_ID 131

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;

bool no_coop = false;

static void start_coop(void *args);
static void send_msg(void *args);
am_addr_t get_nearest_n();
static uint16_t calcDistance(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
//osEventFlagsId_t evt_id;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_ship_strategy(comms_layer_t* radio, am_addr_t addr)
{
	loc_bundle_t loc;

	snd_msg_qID = osMessageQueueNew(9, sizeof(query_msg_t), NULL);
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	setXFirst(true);
	setAlwaysPlaceCargo(true);
	loc.x = loc.y = 0;
	setCraneTactics(cc_do_nothing, 0, loc);
	
	osThreadNew(start_coop, NULL, NULL); // Empty thread
	snd_task_id = osThreadNew(send_msg, NULL, NULL); // Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void start_coop(void *args)
{
	coop_msg_t cmsg;
	am_addr_t dest;
	
	for(;;)
	{
		osDelay(10*osKernelGetTickFreq()); //10 seconds
		if(!no_coop)
		{
			info1("start snd coop");
			cmsg.messageID = START_COOP_MSG_ID;
			cmsg.senderAddr = my_address;
			dest = get_nearest_n();
			cmsg.destinationAddr = dest;
			osMessageQueuePut(snd_msg_qID, &cmsg, 0, 0);
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
	am_addr_t nearest;
	coop_msg_ans_t amsg;

	switch(rmsg[0])
	{
		case START_COOP_MSG_ID :
			info1("rcvd start");
			coop_msg_t * start = (coop_msg_t *) comms_get_payload(comms, msg, pl_len);
			nearest = get_nearest_n();
			if(nearest == start->senderAddr)
			{
				amsg.agreement = true;
				info1("agree");
			}
			else
			{
				amsg.agreement = false;
				info1("no dice");
			}

			amsg.messageID = ANS_COOP_MSG_ID;
			amsg.senderAddr = my_address;
			amsg.destinationAddr = start->senderAddr;
			
			osMessageQueuePut(snd_msg_qID, &amsg, 0, 0);

			break;

		case ANS_COOP_MSG_ID :
			info1("rcvd ans");
			coop_msg_ans_t * ans = (coop_msg_ans_t *)comms_get_payload(comms, msg, pl_len);
			nearest = get_nearest_n();
			if(nearest == ans->senderAddr)
			{
				if(ans->agreement)
				{
					//good, start coop
					info1("coop OK!");
				}
			}
			break;

		default:
			break;
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt-ss %u", result);
    osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void send_msg(void *args)
{
	uint8_t packet[15], len;
	coop_msg_ans_t *acoop_p;
	coop_msg_t *coop_p;
	am_addr_t dest;

	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
	
		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(sradio, &m_msg);

		switch(packet[0])
		{
			case START_COOP_MSG_ID :
				info1("snd coop");
				coop_msg_t * smsg = comms_get_payload(sradio, &m_msg, sizeof(coop_msg_t));
				if (smsg == NULL)
				{
					continue ;// Continue for(;;) loop
				}
				coop_p = (coop_msg_t*)packet;
				smsg->messageID = coop_p->messageID;
				smsg->senderAddr = coop_p->senderAddr;
				smsg->destinationAddr = coop_p->destinationAddr;
				dest = coop_p->destinationAddr;
				len = sizeof(coop_msg_t);

				break;

			case ANS_COOP_MSG_ID : 
				info1("snd ans");
				coop_msg_ans_t * qmsg = comms_get_payload(sradio, &m_msg, sizeof(coop_msg_ans_t));
				if (qmsg == NULL)
				{
					continue ;// Continue for(;;) loop
				}
				acoop_p = (coop_msg_ans_t*)packet;

				qmsg->messageID = acoop_p->messageID;
				qmsg->senderAddr = acoop_p->senderAddr;
				qmsg->destinationAddr = acoop_p->destinationAddr;
				qmsg->agreement = acoop_p->agreement;
				dest = acoop_p->destinationAddr;
				len = sizeof(coop_msg_ans_t);

				break;

			default:
				break;
		}
			
		// Send data packet
	    comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
	    comms_am_set_destination(sradio, &m_msg, dest); //TODO Dont't forget to set destination
	    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(sradio, &m_msg, len);

	    comms_error_t result = comms_send(sradio, &m_msg, radio_send_done, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd-ss %u", result);
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










