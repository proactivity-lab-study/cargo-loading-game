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
 * TODO m_msg max payload size for queue and comms_get_payload function
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

#define SS_DEFAULT_DELAY 2 // A delay, seconds

typedef enum
{
	SHIP_MSG_ID_NEXT_CMD 	    = 1,
	SHIP_MSG_ID_NEXT_SHIP		= 2
} ship_msg_id_t;

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t sfgl_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static uint32_t fglobal_val; // Read-write by multiple threads, needs mutex protection

static void thread_template(void *args); // A thread function
static void sendMsg(void *args); // Message sending thread

static void sendNextCommandMsg(crane_command_t cmd, am_addr_t dest); // Send 'next command' message
static void sendNextShipMsg(am_addr_t next_ship_addr, am_addr_t dest); // Send 'next ship' message

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, comms_get_payload_max_length(radio), NULL);
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
	
	osThreadNew(thread_template, NULL, NULL); 			// Template thread
	snd_task_id = osThreadNew(sendMsg, NULL, NULL); 	// Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void thread_template(void *args)
{
    bool send_command = true;
	uint32_t val;
	for(;;)
	{
		osDelay(SS_DEFAULT_DELAY*osKernelGetTickFreq());
		
		// Do some strategy evaluation here
		if(send_command)
        {
            sendNextCommandMsg(CM_NOTHING_TO_DO, AM_BROADCAST_ADDR);
            send_command = false;
        }
        else
        {
            sendNextShipMsg(my_address, AM_BROADCAST_ADDR);
            send_command = true;
        }
        
        // Select tactics
	    setXFirst(true);
	    setAlwaysPlaceCargo(true);
	    setCraneTactics(cc_to_address, my_address, getShipLocation(my_address));
		
		// Mutex usage example
		while(osMutexAcquire(sfgl_mutex, 1000) != osOK); // Protects fglobal_val
		val = fglobal_val;
		fglobal_val = val + 1;
		osMutexRelease(sfgl_mutex);
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ShipReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
    // Ship-to-ship messages are received here
    // NB! Don't forget to use ntoh() functions when receiving variables larger than one byte!

    uint8_t pl_len = comms_get_payload_length(comms, msg);
    uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
    am_addr_t sender, ship;
    uint8_t cmd;
    ship_next_cmd_msg_t *cpkt;
    ship_next_ship_msg_t *spkt;

    switch(rmsg[0])
    {
    
        // Next command message
        case SHIP_MSG_ID_NEXT_CMD :
            cpkt = (ship_next_cmd_msg_t*)comms_get_payload(comms, msg, sizeof(ship_next_cmd_msg_t));
            info1("Rcvd - nxt cmd msg");
            sender = ntoh16(cpkt->senderAddr);
            cmd = cpkt->cmd;

            // Do something with this info.
            
            break;
            
        // Next ship message
        case SHIP_MSG_ID_NEXT_SHIP :
            spkt = (ship_next_ship_msg_t*)comms_get_payload(comms, msg, sizeof(ship_next_ship_msg_t));
            info1("Rcvd - nxt ship msg");
            sender = ntoh16(spkt->senderAddr);
            ship = ntoh16(spkt->nextShip);
            
            // Do something with this info.
            
            break;
            
        default :
            info1("Rcvd - unk msg");
            break;
    }
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
    ship_next_cmd_msg_t *cmsg, *cpkt;
    ship_next_ship_msg_t *smsg, *spkt;
    uint8_t packet[comms_get_payload_max_length(sradio)];
    for(;;)
    {
        osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
        osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

        comms_init_message(sradio, &m_msg);		
        switch(packet[0])
        {
            // Next command message
            case SHIP_MSG_ID_NEXT_CMD :

            cmsg = (ship_next_cmd_msg_t *)comms_get_payload(sradio, &m_msg, sizeof(ship_next_cmd_msg_t));
            if (cmsg == NULL)
            {
                continue ;// Continue for(;;) loop
            }

            cpkt = (ship_next_cmd_msg_t*)packet;
            cmsg->messageID = cpkt->messageID;
            cmsg->cmd = cpkt->cmd;
            cmsg->senderAddr = hton16(my_address);

            comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
            comms_am_set_destination(sradio, &m_msg, cpkt->senderAddr); // Setting destination address
            comms_set_payload_length(sradio, &m_msg, sizeof(ship_next_cmd_msg_t));
            break;

            // Next ship message
            case SHIP_MSG_ID_NEXT_SHIP :
            
            smsg = (ship_next_ship_msg_t *)comms_get_payload(sradio, &m_msg, sizeof(ship_next_ship_msg_t));
            if (smsg == NULL)
            {
                continue ;// Continue for(;;) loop
            }

            spkt = (ship_next_ship_msg_t*)packet;
            smsg->messageID = spkt->messageID;
            smsg->nextShip = hton16(spkt->nextShip);
            smsg->senderAddr = hton16(my_address);
            comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
            comms_am_set_destination(sradio, &m_msg, spkt->senderAddr); // Setting destination address
            comms_set_payload_length(sradio, &m_msg, sizeof(ship_next_ship_msg_t));
            break;

            default :
                break ;
        }

        // Send data packet
        comms_error_t result = comms_send(sradio, &m_msg, radioSendDone, NULL);
        logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
    }
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

static void sendNextCommandMsg (crane_command_t cmd, am_addr_t dest)
{
	ship_next_cmd_msg_t packet;
	
	packet.messageID = SHIP_MSG_ID_NEXT_CMD;
	packet.senderAddr = dest; // Piggybacking destination address here
	packet.cmd = cmd;
	
	osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	info1("Send next cmd msg");
}

static void sendNextShipMsg (am_addr_t next_ship_addr, am_addr_t dest)
{
	ship_next_ship_msg_t packet;
	
	packet.messageID = SHIP_MSG_ID_NEXT_SHIP;
	packet.senderAddr = dest; // Piggybacking destination address here
	packet.nextShip = next_ship_addr;
	
	osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	info1("Send next ship msg");
}
