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

#include "crane_control.h"
#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "ccntr"
#define __LOG_LEVEL__ (LOG_LEVEL_crane_control & BASE_LOG_LEVEL)
#include "log.h"

typedef struct scmd_t
{
	am_addr_t ship_addr;
	uint8_t ship_cmd;
}scmd_t;

static scmd_t cmds[MAX_SHIPS];
uint32_t lastCraneEventTime = 0; // Kernel ticks
static crane_location_t cloc;

// Some initial tactics choices
bool Xfirst = true; // Which coordinate to use first, x is default
bool alwaysPlaceCargo = true; // Always send 'place cargo' command when crane is on top of a ship

static osMutexId_t cmdb_mutex, cloc_mutex;
static osMessageQueueId_t cmsg_qID, lmsg_qID, smsg_qID;
static osThreadId_t snd_task_id;

static comms_msg_t m_msg;
static comms_layer_t* cradio;
static am_addr_t my_address;
static am_addr_t crane_address = AM_BROADCAST_ADDR; // Use actual crane address if possible
static bool first_msg = true; // Used to get actual crane address once

static void craneMainLoop(void *args);
static void locationMsgHandler(void *args);
static void commandMsgHandler(void *args);
static void sendCommandMsg(void *args);

static uint8_t get_empty_slot();
static uint8_t goToDestination(uint8_t x, uint8_t y);
static uint8_t parrotShip(am_addr_t sID);
static uint8_t selectPopular();
static uint8_t selectCommand(uint8_t x, uint8_t y);
static uint8_t selectCommandXFirst(uint8_t x, uint8_t y);
static uint8_t selectCommandYFirst(uint8_t x, uint8_t y);
static void clear_cmds_buf();

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane_control(comms_layer_t* radio, am_addr_t addr)
{
	const osMutexAttr_t cloc_Mutex_attr = { .attr_bits = osMutexRecursive }; // Allow nesting of this mutex

	cmdb_mutex = osMutexNew(NULL);				// Protects ships' crane command database
	cloc_mutex = osMutexNew(&cloc_Mutex_attr);	// Protects current crane location values
	
	smsg_qID = osMessageQueueNew(9, sizeof(crane_command_msg_t), NULL); // Send queue
	cmsg_qID = osMessageQueueNew(9, sizeof(crane_command_msg_t), NULL); // Receive queue
	lmsg_qID = osMessageQueueNew(3, sizeof(crane_location_msg_t), NULL);
	
	// Initialise ships' commands buffer
	clear_cmds_buf();

	cradio = radio;
	my_address = addr;

	// Get crane start location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cloc.crane_y = 0;
	cloc.crane_x = 0;
	cloc.cargo_here = false;
	osMutexRelease(cloc_mutex);

    osThreadNew(commandMsgHandler, NULL, NULL);		// Handles received crane command messages
    osThreadNew(locationMsgHandler, NULL, NULL);	// Handles received crane command messages
    snd_task_id = osThreadNew(sendCommandMsg, NULL, NULL);		// Handles command message sending
	osThreadNew(craneMainLoop, NULL, NULL);			// Crane state changes
}

/**********************************************************************************************
 *	Crane status changes
 *********************************************************************************************/

static void craneMainLoop(void *args)
{
	uint8_t cmd = 7; // CM_NOTHING_TO_DO
	uint32_t time_left, ticks;
	loc_bundle_t loc;
	crane_command_msg_t packet;

	ticks = (uint32_t)(0.5 * osKernelGetTickFreq()); // Half a second
	for(;;)
	{
		osDelay(ticks);
		
		//TODO User code here: evaluate situation and choose tactics

		time_left = CRANE_UPDATE_INTERVAL * osKernelGetTickFreq() + lastCraneEventTime - osKernelGetTickCount();
		if(time_left < ticks)
		{
			Xfirst = false;	// This is a tactical choice and ship strategy module may want to change it
			alwaysPlaceCargo = true; // This is a tactical choice and ship strategy module may want to change it
			loc = get_ship_location(my_address);
			
			// This is a strategic chioce and ship strategy module may want to change it
			cmd = goToDestination(loc.x, loc.y);
			if(cmd != CM_NOTHING_TO_DO)
			{
				packet.messageID = CRANE_COMMAND_MSG;
				packet.senderAddr = my_address;
				packet.cmd = cmd;
				info1("Cmnd sel %u", cmd);
				osMessageQueuePut(smsg_qID, &packet, 0, 0);
			}
		}
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	am_addr_t crane_addr;
	
	if (pl_len == sizeof(crane_command_msg_t))
    {
        crane_command_msg_t * packet = (crane_command_msg_t*)comms_get_payload(comms, msg, sizeof(crane_command_msg_t));
        debug1("rcv-c");
        osStatus_t err = osMessageQueuePut(cmsg_qID, packet, 0, 0);
		if(err == osOK)info1("command rcvd");
		else warn1("cmsgq err");
    }
    else if (pl_len == sizeof(crane_location_msg_t))
	{
		if(first_msg)crane_addr = comms_am_get_source(comms, msg);

		crane_location_msg_t * packet = (crane_location_msg_t*)comms_get_payload(comms, msg, sizeof(crane_location_msg_t));
        debug1("rcv-l");

		if(packet->messageID == CRANE_LOCATION_MSG && packet->senderAddr == CRANE_ADDR && first_msg)
		{
			crane_address = crane_addr;
			first_msg = false;
		}

        osStatus_t err = osMessageQueuePut(lmsg_qID, packet, 0, 0);
		if(err == osOK)info1("crane location rcvd");
		else warn1("lmsgq err");
	}
	else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

static void locationMsgHandler(void *args)
{
	crane_location_msg_t packet;
	loc_bundle_t sloc;
	am_addr_t saddr;
	for(;;)
	{
		osMessageQueueGet(lmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_LOCATION_MSG && packet.senderAddr == CRANE_ADDR)
		{
			lastCraneEventTime = osKernelGetTickCount();
			info1("Crane mov %lu %lu %u", packet.x_coordinate, packet.y_coordinate, packet.cargoPlaced);

			while(osMutexAcquire(cloc_mutex, 1000) != osOK);
			cloc.crane_x = packet.x_coordinate;
			cloc.crane_y = packet.y_coordinate;
			cloc.cargo_here = packet.cargoPlaced;
			osMutexRelease(cloc_mutex);
			
			// If cargo was placed, check if we need to update our knowledge base
			if(packet.cargoPlaced)
			{
				sloc.x = packet.x_coordinate;
				sloc.y = packet.y_coordinate;
				saddr = get_ship_addr(sloc);
				if(saddr != 0)mark_cargo(saddr);
			}

			clear_cmds_buf(); // Clear contents of cmds buffer
		}
	}
}

// Listen to and store commands sent by other ships
static void commandMsgHandler(void *args)
{
	uint8_t i;
	crane_command_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(cmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_COMMAND_MSG)
		{
			while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
			for(i=0;i<MAX_SHIPS;i++)
			{
				if(cmds[i].ship_addr == packet.senderAddr)
				{
					cmds[i].ship_cmd = packet.cmd;
					break;
				}
			}
			if(i>=MAX_SHIPS) // Add ship and command if room
			{
				i = get_empty_slot();
				if(i<MAX_SHIPS)
				{
					cmds[i].ship_addr = packet.senderAddr;
					cmds[i].ship_cmd = packet.cmd;
				}
				else ; // Drop this ships command, cuz no room
			}
			osMutexRelease(cmdb_mutex);
		}
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radio_send_done(comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
    osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void sendCommandMsg(void *args)
{
	crane_command_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(smsg_qID, (crane_command_msg_t*) &packet, NULL, osWaitForever);
		
		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(cradio, &m_msg);
		crane_command_msg_t * cMsg = comms_get_payload(cradio, &m_msg, sizeof(crane_command_msg_t));
		if (cMsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}

		cMsg->messageID = packet.messageID;
		cMsg->senderAddr = packet.senderAddr;
		cMsg->cmd = packet.cmd;
			
		// Send data packet
	    comms_set_packet_type(cradio, &m_msg, AMID_CRANECOMMUNICATION);
	    comms_am_set_destination(cradio, &m_msg, crane_address);
	    //comms_am_set_source(cradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(cradio, &m_msg, sizeof(crane_command_msg_t));

	    comms_error_t result = comms_send(cradio, &m_msg, radio_send_done, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Crane command tactics
 **********************************************************************************************/

// Selects an appropriate command to get the crane to location (x; y)
// Takes into account 'Xfirst' and 'alwaysPlaceCargo' choices.
// This function can return CM_NOTHING_TO_DO in some cases
static uint8_t goToDestination(uint8_t x, uint8_t y)
{
	uint8_t cmd;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cmd = selectCommand(x, y);
	osMutexRelease(cloc_mutex);
	return cmd;
}

// Returns command sent by ship with sID.
// If no such ship or no command, returns CM_NOTHING_TO_DO.
static uint8_t parrotShip(am_addr_t sID)
{
	uint8_t cmd, i;

	cmd = CM_NOTHING_TO_DO;

	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(cmds[i].ship_addr == sID)
		{
			cmd = cmds[i].ship_cmd;
			break;
		}
	}
	osMutexRelease(cmdb_mutex);

	return cmd;
}

// Returns most popular command sent by all other ships this round.
// In case of tie favors the first most popular choice found.
// If no ship or commands, returns CM_NOTHING_TO_DO.
static uint8_t selectPopular()
{
	uint8_t i, n;
	uint8_t cmd[7];

	// Empty the buffer
	for(i=0;i<7;i++)cmd[i] = 0;

	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		switch(cmds[i].ship_cmd)
		{
			case CM_UP : // 1
			cmd[1]++;
			break;

			case CM_DOWN : // 2
			cmd[2]++;
			break;

			case CM_LEFT : // 3
			cmd[3]++;
			break;

			case CM_RIGHT : // 4
			cmd[4]++;
			break;

			case CM_PLACE_CARGO : // 5
			cmd[5]++;
			break;

			case CM_CURRENT_LOCATION : // 6
			cmd[6]++;
			break;

			default : // 0 && 7
			break;
		}
	}
	osMutexRelease(cmdb_mutex);

	// This favors the first most popular choice
	n=0;
	cmd[0] = CM_NOTHING_TO_DO;
	for(i=1;i<7;i++)if(n < cmd[i])
	{
		n = cmd[i];
		cmd[0] = i; // Using the 0 index memory area for this, because it is available
	}
	
	return cmd[0];
}

static uint8_t selectCommand(uint8_t x, uint8_t y)
{
	uint8_t len = 0, i;
	am_addr_t ships[MAX_SHIPS];
	loc_bundle_t sloc;

	// First check if cargo was placed in the last round, if not, maybe we need to
	if(!cloc.cargo_here)
	{
		// There is no cargo in this place, is there a ship here and do we need to place cargo?
		if(alwaysPlaceCargo)
		{
			len = get_all_ships_addr(ships, MAX_SHIPS);

			if(len > 0 && len <= MAX_SHIPS)
			{
				for(i=0;i<len;i++)
				{
					sloc = get_ship_location(ships[i]);
					// If there is a ship here, then only reasonable command is place cargo
					if(distToCrane(sloc.x, sloc.y) == 0)return CM_PLACE_CARGO;// Found ship in this location
				}
			}
			else ; // No more ships in game besides me

			// If I reach here, then there are no ships besides me in the game
			// or there are more ships but no one is at the current crane location.
			// Therefor just continue with the strategy.
		}
		else ; // Don't care about cargo placement unless it serves my strategy
	}
	else ; // Cargo was placed by the crane in the last round, so no need to place it again this round

	if(Xfirst)return selectCommandXFirst(x, y);
	else return selectCommandYFirst(x, y);
}

static uint8_t selectCommandXFirst(uint8_t x, uint8_t y)
{
	if(y > cloc.crane_y)return CM_UP;
	else if(y < cloc.crane_y)return CM_DOWN;
	else ;

	if(x > cloc.crane_x)return CM_RIGHT;
	else if(x < cloc.crane_x)return CM_LEFT;
	else ;

	// If we get here, then the crane is at the desired location (x; y).
	// Check if there is cargo here and issue place cargo, if there isn't, else return with 'do nothing'.
	// This ensures that we only place cargo to a ship only once.
	if(cloc.cargo_here)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

static uint8_t selectCommandYFirst(uint8_t x, uint8_t y)
{
	if(x > cloc.crane_x)return CM_RIGHT;
	else if(x < cloc.crane_x)return CM_LEFT;
	else ;

	if(y > cloc.crane_y)return CM_UP;
	else if(y < cloc.crane_y)return CM_DOWN;
	else ;

	// If we get here, then the crane is at the desired location (x; y).
	// Check if there is cargo here and issue place cargo, if there isn't, else return with 'do nothing'.
	// This ensures that we only place cargo to a ship only once.
	if(cloc.cargo_here)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns distance to crane, zero distance means crane is at location (x; y).
// If crane location data is unavailable for more than 1000 kernel ticks, returns 
// last available crane location. Availability here is related to cloc_mutex.
// This function can block for 1000 kernel ticks.
uint16_t distToCrane(uint8_t x, uint8_t y)
{
	static uint16_t dist;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	dist = abs(cloc.crane_x - x) + abs(cloc.crane_y - y);
	osMutexRelease(cloc_mutex);
	return dist;
}

static void clear_cmds_buf()
{
	uint8_t i;
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		cmds[i].ship_addr = 0;
		cmds[i].ship_cmd = 0;
	}
	osMutexRelease(cmdb_mutex);
}

//TODO mutex protection if more than commandMsgHandler is calling this function
// Mutex nesting is allowed, but 'release' must be called the same number of times as 'acquire'.
static uint8_t get_empty_slot() 
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(cmds[k].ship_addr == 0)break;
	return k;
}
