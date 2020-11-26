/**
 * 
 * This is the crane module of crane-agent. It is responsible for keeping and
 * updating crane state (i.e. crane location). Crane state is updated every 
 * CRANE_UPDATE_INTERVAL seconds. During this interval ships can send their 
 * movement commands to the crane. At the end of the interval a winning 
 * command is chosen from all received commands and crane location is updated
 * according to the winning command.
 * 
 * The main functionality of this module is to:
 * - keep track of and update crane state
 * - receive movement command messages from ships
 * - store movement commands per ship until the end of update interval
 * - receive crane location request messages from ships
 * - send out crane location messages
 * 
 * 
 * During crane update interval all ships can send their movement commands
 * to the crane. A correct movement command is one of CM_UP, CM_DOWN, CM_LEFT 
 * CM_RIGHT, CM_PLACE_CARGO, CM_CURRENT_LOCATION. Each will request the crane
 * to move one step up, down, left, right or place cargo in current location 
 * respectively. CM_CURRENT_LOCATION asks the crane to send its current location
 * and whether there is cargo in this location. This messages is sent immediately.
 * 
 * Crane stores one command per ship until the end of the update interval. Each
 * ship can send as many commands as it wants to (including changing the command)
 * but only the last received command is stored and later processed. 
 * 
 * When the update event occures, crane processes all received commands and
 * selects a winning command. The winning command is always the most popular 
 * choice, i.e. the command that was requested the most during this round. In
 * case of a tie the winning command is randomly chosen from the two (or several)
 * most popular requests. If no movement commands where received, no winning 
 * command is chosen. Then all received commands are erased in preparation for
 * the next update interval.
 * 
 * After the winning command is chosen crane changes its state (changes location)
 * according to the command. If the winning command was to place cargo, then 
 * crane places cargo in the current location and the new state reflects the 
 * current location but now with cargo placed in this location. The new state 
 * (new location and cargo placement status) is then broadcasted to everybody 
 * and receivement of movement commands commences.
 * 
 * If the crane is asked to exit the game area (see GRID_LOWER_BOUND and
 * GRID_UPPER_BOUND in game_types.h) then crane location is not changed but
 * a new state messages is still broadcast with the current (unchanged) 
 * location.
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

#include "system_state.h"
#include "crane_state.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "crane"
#define __LOG_LEVEL__ (LOG_LEVEL_crane_state & BASE_LOG_LEVEL)
#include "log.h"

static uint8_t cmd_buf[MAX_SHIPS]; // Buffer to store received commands
static crane_location_t cloc;

static osMutexId_t cmdb_mutex, cloc_mutex;
static osMessageQueueId_t smsg_qID, rmsg_qID;
static osThreadId_t snd_task_id;

static comms_msg_t m_msg;
static comms_layer_t* cradio;
static am_addr_t my_address;

static void incomingMsgHandler(void *args);
static void craneMainLoop(void *args);
static void sendLocationMsg(void *args);

static uint8_t getWinningCmd();
static uint8_t doCommand(uint8_t wcmd);
static uint32_t randomNumber(uint32_t rndL, uint32_t rndH);

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane(comms_layer_t* radio, am_addr_t my_addr)
{
	uint8_t i;

	cmdb_mutex = osMutexNew(NULL); // Protects received ship command database
	cloc_mutex = osMutexNew(NULL); // Protects current crane location values
		
	smsg_qID = osMessageQueueNew(9, sizeof(crane_location_msg_t), NULL);
	rmsg_qID = osMessageQueueNew(9, sizeof(crane_command_msg_t), NULL);
	
	// Initialise buffer
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)cmd_buf[i] = 0;
	osMutexRelease(cmdb_mutex);

	cradio = radio;
	my_address = my_addr;

	// Crane default location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cloc.crane_y = 0;
	cloc.crane_x = 0;
	cloc.cargo_here = false;
	osMutexRelease(cloc_mutex);

    osThreadNew(incomingMsgHandler, NULL, NULL);	// Handles received messages 
	osThreadNew(craneMainLoop, NULL, NULL);		// Crane state changes
	snd_task_id = osThreadNew(sendLocationMsg, NULL, NULL);	// Sends crane location info
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to read-to-send state
}

void init_crane_loc()
{
	// Get crane start location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK)
	cloc.crane_y = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
	cloc.crane_x = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
	cloc.cargo_here = false;
	osMutexRelease(cloc_mutex);
}

/**********************************************************************************************
 *	Crane status changes
 *********************************************************************************************/

static void craneMainLoop(void *args)
{
	uint8_t wcmd;
	static crane_location_msg_t sloc;
	const uint32_t delay_ticks = (uint32_t)CRANE_UPDATE_INTERVAL * osKernelGetTickFreq();
	for(;;)
	{
		osDelay(delay_ticks);
		wcmd = getWinningCmd();
		if(wcmd > 0)
		{
			while(osMutexAcquire(cloc_mutex, 1000) != osOK);
			if(!doCommand(wcmd))
			{
				sloc.messageID = CRANE_LOCATION_MSG;
				sloc.senderAddr = my_address;
				sloc.x_coordinate = cloc.crane_x;
				sloc.y_coordinate = cloc.crane_y;
				sloc.cargoPlaced = cloc.cargo_here;
				osMessageQueuePut(smsg_qID, &sloc, 0, 0);
			}
			osMutexRelease(cloc_mutex);
			info1("New loc %u %u %u", sloc.x_coordinate, sloc.y_coordinate, sloc.cargoPlaced);
		}
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	if (comms_get_payload_length(comms, msg) >= sizeof(crane_command_msg_t))
    {
        crane_command_msg_t * packet = (crane_command_msg_t*)comms_get_payload(comms, msg, sizeof(crane_command_msg_t));
        info1("Rcv cmnd");
        osStatus_t err = osMessageQueuePut(rmsg_qID, packet, 0, 0);
		if(err == osOK)info1("rc query");
		else warn1("msgq err");
    }
    else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

static void incomingMsgHandler(void *args)
{
	uint8_t index, cmd;
	crane_location_msg_t sloc;
	crane_command_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(rmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_COMMAND_MSG)
		{
			info("Rcvd %u %u", ntoh16(packet.senderAddr), packet.cmd);
			cmd = packet.cmd;
			if(cmd == CM_CURRENT_LOCATION)
			{
				while(osMutexAcquire(cloc_mutex, 1000) != osOK);
				sloc.messageID = CRANE_LOCATION_MSG;
				sloc.senderAddr = CRANE_ADDR;
				sloc.x_coordinate = cloc.crane_x;
				sloc.y_coordinate = cloc.crane_y;
				sloc.cargoPlaced = cloc.cargo_here;
				osMutexRelease(cloc_mutex);
				osMessageQueuePut(smsg_qID, &sloc, 0, 0);
			}
			else if(cmd > 0 && cmd < CM_CURRENT_LOCATION)
			{
				// Each ship has a designated memory area in the buffer
				// because if a ship sends multiple commands during a
				// crane update interval, only the last must be used.
				index = getIndex(ntoh16(packet.senderAddr));
				if(index < MAX_SHIPS)
				{
					while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
					cmd_buf[index] = cmd;
					osMutexRelease(cmdb_mutex);
					info1("cmd rcv");
				}
				else ;// Ship not in game, command dropped
			}
			else if(cmd == CM_NOTHING_TO_DO) ; // This command shouldn't be sent, but no harm done, just ignore
			else ; // Invalid command, do nothing
		}
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
	osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void sendLocationMsg(void *args)
{
	crane_location_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(smsg_qID, &packet, NULL, osWaitForever);

		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(cradio, &m_msg);
		crane_location_msg_t * cLMsg = comms_get_payload(cradio, &m_msg, sizeof(crane_location_msg_t));
		if (cLMsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}

		cLMsg->messageID = CRANE_LOCATION_MSG;
		cLMsg->senderAddr = hton16((uint16_t)CRANE_ADDR);
		cLMsg->x_coordinate = packet.x_coordinate;
		cLMsg->y_coordinate = packet.y_coordinate;
		cLMsg->cargoPlaced = packet.cargoPlaced;
			
		// Send data packet
	    comms_set_packet_type(cradio, &m_msg, AMID_CRANECOMMUNICATION);
	    comms_am_set_destination(cradio, &m_msg, AM_BROADCAST_ADDR);
	    //comms_am_set_source(cradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(cradio, &m_msg, sizeof(crane_location_msg_t));

	    comms_error_t result = comms_send(cradio, &m_msg, radio_send_done, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

static uint8_t getWinningCmd()
{
	uint8_t votes[MAX_SHIPS], i, rnd, mcount, max, wcmd;
	bool atLeastOne = false;

	for(i=0;i<6;i++)votes[i] = 0;

	// Find most popular command
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<6;i++)
	{
		if(cmd_buf[i] != 0)
		{
			votes[cmd_buf[i]]++;
			atLeastOne = true;
		}
	}
	osMutexRelease(cmdb_mutex);

	// If no commands from ships don't move
	if(!atLeastOne)
	{
		return 0;
	}

	// Get max
	max = votes[1];
	for(i=2;i<6;i++)
	{
		if(votes[i]>max)max = votes[i];
	}
	// Check if there are more than one max and mark these buffer locations, clear other locations
	mcount = 0;
	for(i=1;i<6;i++)
	{
		if(votes[i] == max){votes[i] = 1;mcount++;}
		else votes[i] = 0;
	}
	if(mcount>1)
	{
		// Get random modulo mcount
		rnd = randomNumber(1, mcount);

		for(i=1;i<6;i++)if(votes[i] == 1)
		{
			rnd--;
			if(rnd == 0){wcmd = i;break;} // Winning command
		}
	}
	else for(i=1;i<6;i++)if(votes[i] == 1){wcmd = i;break;}

	return wcmd;
}

static uint8_t doCommand(uint8_t wcmd)
{
	cloc.cargo_here = false;
	switch(wcmd)
	{
		case CM_UP: if(cloc.crane_y<GRID_UPPER_BOUND)cloc.crane_y++;
		break;
		case CM_DOWN: if(cloc.crane_y>GRID_LOWER_BOUND)cloc.crane_y--;
		break;
		case CM_LEFT: if(cloc.crane_x>GRID_LOWER_BOUND)cloc.crane_x--;
		break;
		case CM_RIGHT: if(cloc.crane_x<GRID_UPPER_BOUND)cloc.crane_x++;
		break;
		case CM_PLACE_CARGO: cloc.cargo_here = true;
		break;
		default: return 1; // Zero ends up here
		break;
	}
	if(cloc.cargo_here)info1("Cargo placed");
	return 0;
}

// Random number between rndL and rndH (rndL <= rnd <=rndH)
// only positive values
// user must provide correct arguments, such that 0 <= rndL < rndH

static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}

