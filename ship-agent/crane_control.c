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
#define __MODUUL__ "crane"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

typedef struct scmd_t
{
	am_addr_t ship_ID;
	uint8_t ship_cmd;
}scmd_t;


scmd_t cmds[MAX_SHIPS];
uint8_t ship_ID[MAX_SHIPS];
uint32_t lastCraneEventTime = 0;
crane_location_t cloc;

bool Xfirst = true;//which coordinate to use first, X is default
bool alwaysPlaceCargo = true; //always send 'place cargo' command when crane is on top of a ship

static osMutexId_t cmdb_mutex, cloc_mutex, snd_mutex;
static osMessageQueueId_t cmsg_qID, lmsg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* cradio;
am_addr_t my_address;

void crane_main_loop(void *args);
void craneLocationMsg_handler_loop(void *args);
void craneCommandMsg_handler_loop(void *args);

static uint8_t get_empty_slot();
static void sendCommand(uint8_t cmd, am_addr_t destination);
static uint8_t goToDestination(uint8_t x, uint8_t y);
static uint8_t parrotShip(am_addr_t sID);
static uint8_t selectPopular();
static uint8_t selectCommand(uint8_t x, uint8_t y);
static uint8_t selectCommandXFirst(uint8_t x, uint8_t y);
static uint8_t selectCommandYFirst(uint8_t x, uint8_t y);
static uint16_t distToCrane(uint8_t x, uint8_t y);
static void clear_cmds_buf();

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane_control(comms_layer_t* radio, am_addr_t addr)
{
	cmdb_mutex = osMutexNew(NULL);	//protects ships' crane command database
	cloc_mutex = osMutexNew(NULL);	//protects current crane location values
	snd_mutex = osMutexNew(NULL);	//protects against sending another message before hardware has handled previous message
	
	cmsg_qID = osMessageQueueNew(9, sizeof(craneCommandMsg), NULL);
	lmsg_qID = osMessageQueueNew(2, sizeof(craneLocationMsg), NULL);
	
	//initialise ships' commands buffer
	clear_cmds_buf();

	cradio = radio;
	my_address = addr;

	//get crane start location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cloc.craneY = 0;
	cloc.craneX = 0;
	cloc.cargoInCurrentLoc = false;
	osMutexRelease(cloc_mutex);

    osThreadNew(craneCommandMsg_handler_loop, NULL, NULL);	//handles received crane command messages
    osThreadNew(craneLocationMsg_handler_loop, NULL, NULL);	//handles received crane command messages
	osThreadNew(crane_main_loop, NULL, NULL);	//crane state changes
	
}

/**********************************************************************************************
 *	Crane status changes
 *********************************************************************************************/

void crane_main_loop(void *args)
{
	uint8_t cmd = 7;//CM_NOTHING_TO_DO
	uint32_t time_left, ticks;
	loc_bundle_t loc;

	ticks = (uint32_t)(0.5 * osKernelGetTickFreq()); //half of a second
	for(;;)
	{
		osDelay(ticks);
		
		//TODO evaluate situation and choose tactics

		time_left = CRANE_UPDATE_INTERVAL * osKernelGetTickFreq() + lastCraneEventTime - osKernelGetTickCount();
		if(time_left < ticks)
		{
			Xfirst = false;
			alwaysPlaceCargo = true;
			loc = get_ship_location(my_address);
			cmd = goToDestination(loc.x, loc.y);
			if(cmd != CM_NOTHING_TO_DO)sendCommand(cmd, CRANE_ID);
		}
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);

	if (pl_len == sizeof(craneCommandMsg))
    {
        craneCommandMsg * packet = (craneCommandMsg*)comms_get_payload(comms, msg, sizeof(craneCommandMsg));
        debug1("rcv-c");
        osStatus_t err = osMessageQueuePut(cmsg_qID, packet, 0, 0);
		if(err == osOK)info1("command rcvd");
		else warn1("cmsgq err");
    }
    else if (pl_len == sizeof(craneLocationMsg))
	{
		craneLocationMsg * packet = (craneLocationMsg*)comms_get_payload(comms, msg, sizeof(craneLocationMsg));
        debug1("rcv-l");
        osStatus_t err = osMessageQueuePut(lmsg_qID, packet, 0, 0);
		if(err == osOK)info1("crane location rcvd");
		else warn1("lmsgq err");
	}
	else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

void craneLocationMsg_handler_loop(void *args)
{
	craneLocationMsg packet;

	for(;;)
	{
		osMessageQueueGet(lmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_LOCATION_MSG && packet.senderAddr == CRANE_ID)
		{
			lastCraneEventTime = osKernelGetTickCount();

			while(osMutexAcquire(cloc_mutex, 1000) != osOK);
			cloc.craneX = packet.x_coordinate;
			cloc.craneY = packet.y_coordinate;
			cloc.cargoInCurrentLoc = packet.cargoPlaced;
			osMutexRelease(cloc_mutex);
			
			//if cargo was placed, check if we need to update our knowledge base
			if(packet.cargoPlaced)
			{
				//TODO notify Knowledge database
			}

			clear_cmds_buf();//clear contents of cmds buffer
		}
	}
}

void craneCommandMsg_handler_loop(void *args)
{
	uint8_t i;
	craneCommandMsg packet;

	for(;;)
	{
		osMessageQueueGet(cmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_COMMAND_MSG)
		{
			while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
			for(i=0;i<MAX_SHIPS;i++)
			{
				if(cmds[i].ship_ID == packet.senderAddr)
				{
					cmds[i].ship_cmd = packet.cmd;
					break;
				}
			}
			if(i>=MAX_SHIPS)//add ship and command if room
			{
				i = get_empty_slot();
				if(i<MAX_SHIPS)
				{
					cmds[i].ship_ID = packet.senderAddr;
					cmds[i].ship_cmd = packet.cmd;
				}
				else ; //drop this ships command, cuz no room
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
    while(osMutexAcquire(snd_mutex, 1000) != osOK);
    m_sending = false;
    osMutexRelease(snd_mutex);
}

static void sendCommand(uint8_t cmd, am_addr_t destination)
{
	while(osMutexAcquire(snd_mutex, 1000) != osOK);
	if(!m_sending)
	{
		comms_init_message(cradio, &m_msg);
		craneCommandMsg * cMsg = comms_get_payload(cradio, &m_msg, sizeof(craneCommandMsg));
		if (cMsg == NULL)
		{
			return ;
		}

		cMsg->messageID = CRANE_COMMAND_MSG;
		cMsg->senderAddr = my_address;
		cMsg->cmd = cmd;
				
		// Send data packet
        comms_set_packet_type(cradio, &m_msg, AMID_CRANECOMMUNICATION);
        comms_am_set_destination(cradio, &m_msg, destination);
        //comms_am_set_source(cradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
        comms_set_payload_length(cradio, &m_msg, sizeof(craneCommandMsg));

        comms_error_t result = comms_send(cradio, &m_msg, radio_send_done, NULL);
        logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
        if (COMMS_SUCCESS == result)
        {
            m_sending = true;
        }
	}
	osMutexRelease(snd_mutex);
}

/**********************************************************************************************
 *	Crane command tactics
 **********************************************************************************************/

static uint8_t goToDestination(uint8_t x, uint8_t y)
{
	uint8_t cmd;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cmd = selectCommand(x, y);
	osMutexRelease(cloc_mutex);
	return cmd;
}

static uint8_t parrotShip(am_addr_t sID)
{
	uint8_t cmd, i;

	cmd = 0;

	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(cmds[i].ship_ID == sID)cmd = cmds[i].ship_cmd;
	}
	osMutexRelease(cmdb_mutex);

	if(cmd == 0)cmd = CM_NOTHING_TO_DO;
	return cmd;
}

static uint8_t selectPopular()
{
	uint8_t i, n;
	uint8_t cmd[7];

	//empty the buffer
	for(i=1;i<7;i++)cmd[i] = 0;

	for(i=0;i<MAX_SHIPS;i++)
	{
		switch(cmds[i].ship_cmd)
		{
			case CM_UP ://1
			cmd[1]++;
			break;

			case CM_DOWN ://2
			cmd[2]++;
			break;

			case CM_LEFT ://3
			cmd[3]++;
			break;

			case CM_RIGHT ://4
			cmd[4]++;
			break;

			case CM_PLACE_CARGO ://5
			cmd[5]++;
			break;

			case CM_CURRENT_LOCATION ://6
			cmd[6]++;
			break;

			default ://0 && 7
			break;
		}
	}

	//this favors the first most popular choice
	n=0;
	cmd[0] = CM_NOTHING_TO_DO;
	for(i=1;i<7;i++)if(n < cmd[i])
	{
		n = cmd[i];
		cmd[0] = i;//using the 0 index memory area for this, because it is available anyway
	}
	
	if(cmd[0] == 0)cmd[0] = CM_NOTHING_TO_DO;
	return cmd[0];
}

static uint8_t selectCommand(uint8_t x, uint8_t y)
{
	uint8_t ships[MAX_SHIPS], len = 0, i;

	//first check if cargo was placed in the last round, if not, maybe we need to
	if(!cloc.cargoInCurrentLoc)
	{
		//there is no cargo in this place, is there a ship here and do we need to place cargo?
		if(alwaysPlaceCargo)
		{
			//TODO call KnowledgeLink.getShipsInGame(ships, &len);

			if(len > 0 && len <= MAX_SHIPS)
			{
				for(i=0;i<len;i++)
				{
					//TODO loc = call KnowledgeLink.getShipLocation(ships[i]);
					//if there is a ship here, then only reasonable command is place cargo
					//TODO if(distToCrane(loc.x_coordinate, loc.y_coordinate) == 0)return CM_PLACE_CARGO;//found ship in this location
				}
			}
			else ; //no more ships in game besides me

			//if I reach here, then there are no ships besides me in the game
			//or there are more ships but no one is at the current crane location
			//therefor just continue with the strategy
		}
		else ; //don't care about cargo placement unless it serves my strategy
	}
	else ; //cargo was placed by the crane in the last round, so no need to place it again this round

	if(Xfirst)return selectCommandXFirst(x, y);
	else return selectCommandYFirst(x, y);
}

static uint8_t selectCommandXFirst(uint8_t x, uint8_t y)
{
	if(y > cloc.craneY)return CM_UP;
	else if(y < cloc.craneY)return CM_DOWN;
	else ;

	if(x > cloc.craneX)return CM_RIGHT;
	else if(x < cloc.craneX)return CM_LEFT;
	else ;

	//if we get here, then the crane is at the desired location 'loc'
	//check if there isn't cargo here, then issue place cargo, else return with 'do nothing'
	//this ensures that we only place cargo to a ship only once
	if(cloc.cargoInCurrentLoc)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

static uint8_t selectCommandYFirst(uint8_t x, uint8_t y)
{
	if(x > cloc.craneX)return CM_RIGHT;
	else if(x < cloc.craneX)return CM_LEFT;
	else ;

	if(y > cloc.craneY)return CM_UP;
	else if(y < cloc.craneY)return CM_DOWN;
	else ;

	//if we get here, then the crane is at the desired location 'loc'
	//check if there isn't cargo here, then issue 'place cargo', else return with 'do nothing'
	//this ensures that we only place cargo to a ship only once
	if(cloc.cargoInCurrentLoc)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

static uint16_t distToCrane(uint8_t x, uint8_t y)
{
	return abs(cloc.craneX - x) + abs(cloc.craneY - y);
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

static void clear_cmds_buf()
{
	uint8_t i;
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		cmds[i].ship_ID = 0;
		cmds[i].ship_cmd = 0;
	}
	osMutexRelease(cmdb_mutex);
}

static uint8_t get_empty_slot()//TODO mutex protection if more than craneCommandMsg_handler_loop is calling this function
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(cmds[k].ship_ID == 0)break;
	return k;
}
