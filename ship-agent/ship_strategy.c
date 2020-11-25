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

#include "ship_strategy.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "sstrt"
#define __LOG_LEVEL__ (LOG_LEVEL_ship_strategy & BASE_LOG_LEVEL)
#include "log.h"

static osMutexId_t snd_mutex;
static osMessageQueueId_t snd_msg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* sradio;
static am_addr_t my_address;

static void not_much(void *args);
static void send_msg(void *args);

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_ship_strategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_mutex = osMutexNew(NULL); // Protects against sending message before hardware has handled previous message
	
	snd_msg_qID = osMessageQueueNew(9, sizeof(query_msg_t), NULL);
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	//This is the only write, so not going to protect it with mutex

	osThreadNew(not_much, NULL, NULL); // Sends welcome message
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void not_much(void *args)
{
	
	for(;;)
	{
		osDelay(60*osKernelGetTickFreq()); //60 seconds
		// Do some strategy evaluation here
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ship_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	
	info1("rcvd");
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

static void send_msg(void *args)
{
	query_msg_t packet;
	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
		while(osMutexAcquire(snd_mutex, 1000) != osOK);
		if(!m_sending)
		{
			comms_init_message(sradio, &m_msg);
			query_msg_t * qmsg = comms_get_payload(sradio, &m_msg, sizeof(query_msg_t));
			if (qmsg == NULL)
			{
				osMutexRelease(snd_mutex);
				continue ;// Continue for(;;) loop
			}
			qmsg->messageID = packet.messageID;
			qmsg->messageID = packet.senderAddr;
			qmsg->messageID = packet.shipAddr;

			// Send data packet
		    comms_set_packet_type(sradio, &m_msg, AMID_SYSTEMCOMMUNICATION);
		    comms_am_set_destination(sradio, &m_msg, packet.shipAddr); //TODO Dont't forget to set destination
		    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
		    comms_set_payload_length(sradio, &m_msg, sizeof(query_msg_t));

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


