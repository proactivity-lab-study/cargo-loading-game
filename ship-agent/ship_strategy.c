/**
 * 
 * This is the ship strategy module of ship-agent. It's purpose is to communicate with 
 * other ships and establish some kind of cooperation. It is also responsible for 
 * setting the tactics and goals for crane control module.
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 * TODO reminder to use hton and ntoh functions to assign variable values
 * 		larger than a byte in network messages!! 
 *
 * TODO Mechanism to leave the game.
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

#include "loglevels.h"
#define __MODUUL__ "sstrt"
#define __LOG_LEVEL__ (LOG_LEVEL_ship_strategy & BASE_LOG_LEVEL)
#include "log.h"

#define SS_DEFAULT_DELAY 60 // A delay, seconds

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t sfgl_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static uint32_t fglobal_val; // Read-write by multiple threads, needs mutex protection

static void thread_template(void *args); // A thread function
static void shipMsgLoop(void *args); // Creates a message to send
static void sendMsg(void *args); // Message sending thread

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, sizeof(ship_msg_t), NULL);
	sfgl_mutex = osMutexNew(NULL);	// Protects fglobal_val
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	while(osMutexAcquire(sfgl_mutex, 1000) != osOK); // Protects fglobal_val
	fglobal_val = 0;
	osMutexRelease(sfgl_mutex);

	// Default tactics choices
	setXFirst(true);
	setAlwaysPlaceCargo(true);
	setCraneTactics(cc_to_address, my_address, getShipLocation(my_address));
	
	osThreadNew(thread_template, NULL, NULL); 			// Empty thread
	osThreadNew(shipMsgLoop, NULL, NULL); 				// Creates messages
	snd_task_id = osThreadNew(sendMsg, NULL, NULL); 	// Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void thread_template(void *args)
{
	uint32_t val;
	for(;;)
	{
		osDelay(SS_DEFAULT_DELAY*osKernelGetTickFreq());
		// Do some strategy evaluation here
		
		while(osMutexAcquire(sfgl_mutex, 1000) != osOK); // Protects fglobal_val
		val = fglobal_val;
		fglobal_val = val + 1;
		osMutexRelease(sfgl_mutex);
	}
}

static void shipMsgLoop(void *args)
{
	ship_msg_t packet;
	for(;;)
	{
		packet.messageID = 127;
		packet.senderAddr = AM_BROADCAST_ADDR; // Piggybacking destination address here
		packet.val8 = 0xAB;
		packet.val16 = hton16(0xABCD);
		packet.valf = htonf(1.1);
		
		while(osMutexAcquire(sfgl_mutex, 1000) != osOK);// Protects fglobal_val
		packet.val32 = hton32(fglobal_val);
		osMutexRelease(sfgl_mutex);
		
		osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
		info1("Send demo msg");
		osDelay(SS_DEFAULT_DELAY*osKernelGetTickFreq());
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ShipReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	// Ship-to-ship messages are received here
	// NB! Don't forget to use ntoh() functions when receiving variables larger than one byte!

	uint8_t pl_len;
	ship_msg_t * rmsg;
	am_addr_t sender;
	uint8_t val8, msgID;
	uint16_t val16;
	uint32_t val32;
	float valf;

	pl_len = comms_get_payload_length(comms, msg);
	rmsg = (ship_msg_t *) comms_get_payload(comms, msg, pl_len);

	if(pl_len == sizeof(ship_msg_t))
	{
		msgID = rmsg->messageID;
		sender = ntoh16(rmsg->senderAddr);
		val8 = rmsg->val8;
		val16 = ntoh16(rmsg->val16);
		val32 = ntoh32(rmsg->val32);
		valf = ntohf(rmsg->valf);

		// Float and double can't be used with log messages
		info1("Rcvd %u, %u, %u, %x, %lu, %lu", msgID, sender, val8, val16, val32, valf); 
	}
	else info1("Rcvd - wrong length");
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radioSendDone(comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_INFO1: LOG_WARN1, "snt %u", result);
    osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void sendMsg(void *args)
{
	// NB! Don't forget to use hton() functions when sending variables larger than one byte!
	ship_msg_t packet;
	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
	
		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(sradio, &m_msg);
		ship_msg_t * qmsg = comms_get_payload(sradio, &m_msg, sizeof(ship_msg_t));
		if (qmsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}
		qmsg->messageID = packet.messageID;
		qmsg->senderAddr = hton16(my_address); 
		qmsg->val8 = packet.val8;
		qmsg->val16 = packet.val16;
		qmsg->val32 = packet.val32;
		qmsg->valf = packet.valf;

		// Send data packet
	    comms_set_packet_type(sradio, &m_msg, AMID_SYSTEMCOMMUNICATION);
	    comms_am_set_destination(sradio, &m_msg, packet.senderAddr); // Setting destination address
	    comms_set_payload_length(sradio, &m_msg, sizeof(ship_msg_t));

	    comms_error_t result = comms_send(sradio, &m_msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/


