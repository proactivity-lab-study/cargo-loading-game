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

#include "game_state.h"
#include "crane_state.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "crane"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

uint8_t cmd_buf[MAX_SHIPS];//buffer to store commands received
crane_location_t cloc;

static osMutexId_t cmdb_mutex, cloc_mutex, snd_mutex;
static osMessageQueueId_t msg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* cradio;

void message_handler_loop(void *args);
void crane_main_loop(void *args);
static uint8_t get_winning_cmd();
static uint8_t doCommand(uint8_t wcmd);
static void sendLoc(am_addr_t destination, uint8_t x, uint8_t y, bool isCargoLoaded);
static uint32_t randomNumber(uint32_t rndL, uint32_t rndH);

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane(comms_layer_t* radio)
{
	uint8_t i;

	cmdb_mutex = osMutexNew(NULL);	//protects received ship command database
	cloc_mutex = osMutexNew(NULL);	//protects current crane location values
	snd_mutex = osMutexNew(NULL);	//protects against sending another message before hardware has handled previous message
	
	msg_qID = osMessageQueueNew(6, sizeof(craneCommandMsg), NULL);
	
	//initialise buffer
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)cmd_buf[i] = 0;
	osMutexRelease(cmdb_mutex);

	cradio = radio;

	//get crane start location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cloc.craneY = 0;
	cloc.craneX = 0;
	cloc.cargoInCurrentLoc = false;
	osMutexRelease(cloc_mutex);

    osThreadNew(message_handler_loop, NULL, NULL);	//handles received messages and sends crane location info
	osThreadNew(crane_main_loop, NULL, NULL);	//crane state changes
	
}

void init_crane_loc()
{
	while(osMutexAcquire(cloc_mutex, 1000) != osOK)
	cloc.craneY = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
	cloc.craneX = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
	cloc.cargoInCurrentLoc = false;
	osMutexRelease(cloc_mutex);
}

/**********************************************************************************************
 *	Crane status changes
 *********************************************************************************************/

void crane_main_loop(void *args)
{
	uint8_t wcmd;
	crane_location_t sloc;
	const uint32_t delay_ticks = (uint32_t)(CRANE_UPDATE_INTERVAL * osKernelGetTickFreq());
	for(;;)
	{
		osDelay(delay_ticks);
		wcmd = get_winning_cmd();
		if(wcmd > 0)
		{
			while(osMutexAcquire(cloc_mutex, 1000) != osOK);
			if(!doCommand(wcmd))
			{
				sloc.craneY = cloc.craneY;
				sloc.craneX = cloc.craneX;
				sloc.cargoInCurrentLoc = cloc.cargoInCurrentLoc;
				sendLoc(AM_BROADCAST_ADDR, sloc.craneX, sloc.craneY, sloc.cargoInCurrentLoc);
				info1("new location x y");
			}
			osMutexRelease(cloc_mutex);
		}
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	if (comms_get_payload_length(comms, msg) >= sizeof(craneCommandMsg))
    {
        craneCommandMsg * packet = (craneCommandMsg*)comms_get_payload(comms, msg, sizeof(craneCommandMsg));
        debug1("rcv");
        osStatus_t err = osMessageQueuePut(msg_qID, packet, 0, 0);
		if(err == osOK)info1("rc query");
		else warn1("msgq err");
    }
    else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

void message_handler_loop(void *args)
{
	uint8_t index, cmd;
	crane_location_t sloc;
	craneCommandMsg packet;

	for(;;)
	{
		osMessageQueueGet(msg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_COMMAND_MSG)
		{
			cmd = packet.cmd;
			if(cmd == CM_CURRENT_LOCATION)
			{
				while(osMutexAcquire(cloc_mutex, 1000) != osOK);
				sloc.craneY = cloc.craneY;
				sloc.craneX = cloc.craneX;
				sloc.cargoInCurrentLoc = cloc.cargoInCurrentLoc;
				osMutexRelease(cloc_mutex);
				sendLoc(packet.senderAddr, sloc.craneX, sloc.craneY, sloc.cargoInCurrentLoc);
			}
			else if(cmd > 0 && cmd < CM_CURRENT_LOCATION)
			{
				index = getIndex(packet.senderAddr);
				if(index < MAX_SHIPS)
				{
					while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
					cmd_buf[index] = cmd;
					osMutexRelease(cmdb_mutex);
					info1("cmd rcv");
				}
				else ;//ship not in game, command dropped
			}
			else if(cmd == CM_NOTHING_TO_DO) ; //this command shouldn't be sent, but no harm done, just ignore
			else ;//invalid command, do nothing
		}
	}
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

static void sendLoc(am_addr_t destination, uint8_t x, uint8_t y, bool isCargoLoaded)
{
	while(osMutexAcquire(snd_mutex, 1000) != osOK);
	if(!m_sending)
	{
		comms_init_message(cradio, &m_msg);
		craneLocationMsg * cLMsg = comms_get_payload(cradio, &m_msg, sizeof(craneLocationMsg));
		if (cLMsg == NULL)
		{
			return ;
		}

		cLMsg->messageID = CRANE_LOCATION_MSG;
		cLMsg->senderAddr = SYSTEM_ID;
		cLMsg->x_coordinate = x;
		cLMsg->y_coordinate = y;
		cLMsg->cargoPlaced = isCargoLoaded;
				
		// Send data packet
        comms_set_packet_type(cradio, &m_msg, AMID_CRANECOMMUNICATION);
        comms_am_set_destination(cradio, &m_msg, destination);
        //comms_am_set_source(cradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
        comms_set_payload_length(cradio, &m_msg, sizeof(craneLocationMsg));

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
 *	Utility functions
 **********************************************************************************************/

static uint8_t get_winning_cmd()
{
	//change crane state
	uint8_t votes[MAX_SHIPS], i, rnd, mcount, max, wcmd;
	bool atLeastOne = false;

	for(i=0;i<6;i++)votes[i] = 0;

	//find most popular command
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

	//if no commands from ships don't move
	if(!atLeastOne)
	{
		return 0;
	}

	//get max
	max = votes[1];
	for(i=2;i<6;i++)
	{
		if(votes[i]>max)max = votes[i];
	}
	//check if there are more than one max and mark these buffer locations, clear other locations
	mcount = 0;
	for(i=1;i<6;i++)
	{
		if(votes[i] == max){votes[i] = 1;mcount++;}
		else votes[i] = 0;
	}
	if(mcount>1)
	{
		//get random modulo mcount
		rnd = randomNumber(1, mcount);

		for(i=1;i<6;i++)if(votes[i] == 1)
		{
			rnd--;
			if(rnd == 0){wcmd = i;break;}//winning command
		}
	}
	else for(i=1;i<6;i++)if(votes[i] == 1){wcmd = i;break;}

	return wcmd;
}

static uint8_t doCommand(uint8_t wcmd)
{
	cloc.cargoInCurrentLoc = false;
	switch(wcmd)
	{
		case CM_UP: if(cloc.craneY<GRID_UPPER_BOUND)cloc.craneY++;
		break;
		case CM_DOWN: if(cloc.craneY>GRID_LOWER_BOUND)cloc.craneY--;
		break;
		case CM_LEFT: if(cloc.craneX>GRID_LOWER_BOUND)cloc.craneX--;
		break;
		case CM_RIGHT: if(cloc.craneX<GRID_UPPER_BOUND)cloc.craneX++;
		break;
		case CM_PLACE_CARGO: cloc.cargoInCurrentLoc = true;
		break;
		default: return 1;//zero ends up here
		break;
	}
	if(cloc.cargoInCurrentLoc)info1("Cargo placed");
	return 0;
}

//random number between rndL and rndH (rndL <= rnd <=rndH)
//only positive values
//user must provide correct arguments, such that 0 <= rndL < rndH

static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}

