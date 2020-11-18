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

#include "loglevels.h"
#define __MODUUL__ "shipstrategy"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

static osMutexId_t snd_mutex;
static osMessageQueueId_t snd_msg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* sradio;
am_addr_t my_address;

void not_much(void *args);
void send_msg(void *args);
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

	osThreadNew(not_much, NULL, NULL);//sends welcome message
}

/**********************************************************************************************
 *	 some threads
 *********************************************************************************************/

void not_much(void *args)
{
	
	for(;;)
	{
		osDelay(60*osKernelGetTickFreq()); //60 seconds
		//do some strategy evaluation here
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


