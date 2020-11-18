/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

//#include <stdio.h>

#include "cmsis_os2.h"

#include <stdlib.h>

#include "mist_comm_am.h"
#include "radio.h"

#include "game_state.h"
#include "crane_state.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "game"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

uint32_t global_load_deadline = DEFAULT_TIME;

sdb_t ship_db[MAX_SHIPS];
bool first_msg = true;

static comms_msg_t msg;
static bool m_sending = false;
static comms_layer_t* sradio;

static osMutexId_t snd_mutex, sdb_mutex;
static osMessageQueueId_t rcv_msg_qID;

void incoming_request_loop(void *arg);
static uint8_t registerNewShip(uint8_t shipAddr);
static void getNewCoordinates(uint8_t index);
static void makeDTime(uint8_t index);
static uint8_t getEmptySlot();
static void getAllShips(uint8_t buf[], uint8_t *len);
static void getAllCargo(uint8_t buf[], uint8_t *len);
static void sendRes(uint8_t msgID, uint8_t destination, uint8_t x, uint8_t y, uint8_t dt, bool isCargoLoaded);
static void sendResBuf(uint8_t msgID, uint8_t destination, uint8_t ships[], uint8_t l);
static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user);
static uint32_t randomNumber(uint32_t rndL, uint32_t rndH);

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_game()
{
	//srand(osKernelGetSysTimerCount()); //initialise random number generator
	srand(osKernelGetTickCount()); //initialise random number generator
	init_crane_loc(); //crane location
	global_load_deadline = osKernelGetTickCount() + (DURATION_OF_GAME - 1) * osKernelGetTickFreq();
}

void init_system(comms_layer_t* radio)
{
	uint8_t i=0;
	for(i=0;i<MAX_SHIPS;i++)
	{
		ship_db[i].shipInGame = false;
		ship_db[i].shipAddr = 0;
		ship_db[i].x_coordinate = DEFAULT_LOC;
		ship_db[i].y_coordinate = DEFAULT_LOC;
		ship_db[i].ltime = DEFAULT_TIME;
		ship_db[i].isCargoLoaded = false;
	}

	sradio = radio;

	rcv_msg_qID = osMessageQueueNew(3, sizeof(queryMsg), NULL);	//received messages
	snd_mutex = osMutexNew(NULL);	//protects against sending another message before hardware has handled previous message
	sdb_mutex = osMutexNew(NULL);	//protects registered ship database

	osThreadNew(incoming_request_loop, NULL, NULL);	//handles incoming messages and responses to
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	//first message that is received triggers random number generator init
	//and subsequently starts game
	if(first_msg)
	{
		init_game();
		first_msg = false;
	}

	if (comms_get_payload_length(comms, msg) == sizeof(queryMsg))
	{
	    queryMsg * packet = (queryMsg*)comms_get_payload(comms, msg, sizeof(queryMsg));
		osStatus_t err = osMessageQueuePut(rcv_msg_qID, packet, 0, 0);
		if(err == osOK)info1("rc query");
		else warn1("msgq err");
	}
	else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

void incoming_request_loop(void *arg)
{
	uint8_t ndx;
	uint8_t ships[MAX_SHIPS], len;
	uint8_t err;
	uint32_t dTime;
	queryMsg packet;
	for(;;)
	{
		osMessageQueueGet(rcv_msg_qID, &packet, NULL, osWaitForever);
		switch(packet.messageID)
		{
			case WELCOME_MSG:
				err = registerNewShip(packet.senderAddr);
				if(err >= MAX_SHIPS)
				{
					warn1("no room");
				}
				else 
				{
					info1("new ship: DATA_HERE");
					dTime = global_load_deadline;
					ndx = getIndex(packet.senderAddr);
					sendRes(WELCOME_MSG, packet.senderAddr, ship_db[ndx].x_coordinate, ship_db[ndx].y_coordinate, global_load_deadline, ship_db[ndx].isCargoLoaded);
				}
			break;

			case GTIME_QMSG:
				sendRes(GTIME_QRMSG, packet.senderAddr, DEFAULT_LOC, DEFAULT_LOC, global_load_deadline, false);
			break;

			case SHIP_QMSG:
				//TODO this isn't finished???
				ndx = getIndex(packet.shipAddr);
				sendRes(WELCOME_MSG, packet.senderAddr, ship_db[ndx].x_coordinate, ship_db[ndx].y_coordinate, global_load_deadline, ship_db[ndx].isCargoLoaded);
			break;

			case AS_QMSG:
				getAllShips(ships, &len);
				sendResBuf(AS_QRMSG, packet.senderAddr, ships, len);
			break;

			case ACARGO_QMSG:
				getAllCargo(ships, &len);
				sendResBuf(ACARGO_QRMSG, packet.senderAddr, ships, len);
			break;

			default: break;//do nothing, except drop this response
		}
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

void sendRes(uint8_t msgID, uint8_t destination, uint8_t x, uint8_t y, uint8_t dt, bool isCargoLoaded)
{
	while(osMutexAcquire(snd_mutex, 1000) != osOK);
	if(!m_sending)
	{
		comms_init_message(sradio, &msg);
		queryResponseMsg * qRMsg = comms_get_payload(sradio, &msg, sizeof(queryResponseMsg));
		if (qRMsg == NULL)
		{
			return ;
		}

		switch(msgID)
		{
			case WELCOME_RMSG:

			qRMsg->messageID = WELCOME_RMSG;
			qRMsg->senderAddr = SYSTEM_ID;
			qRMsg->shipAddr = destination;
			//TODO:qRMsg->departureT = ; //NB! use hton16
			qRMsg->x_coordinate = x;
			qRMsg->y_coordinate = y;
			qRMsg->isCargoLoaded = isCargoLoaded;
			break;

			case GTIME_QRMSG:

			qRMsg->messageID = GTIME_QRMSG;
			qRMsg->senderAddr = SYSTEM_ID;
			qRMsg->shipAddr = destination;
			//TODO:qRMsg->departureT = ; //NB! use hton16
			qRMsg->x_coordinate = DEFAULT_LOC;
			qRMsg->y_coordinate = DEFAULT_LOC;
			qRMsg->isCargoLoaded = false;
			break;

			case SHIP_QRMSG:
			
			qRMsg->messageID = SHIP_QRMSG;
			qRMsg->senderAddr = SYSTEM_ID;
			qRMsg->shipAddr = destination;
			//TODO:qRMsg->departureT = dt; //NB! use hton16
			qRMsg->x_coordinate = x;
			qRMsg->y_coordinate = y;
			qRMsg->isCargoLoaded = isCargoLoaded;
			break;

			default:
			break;
		}
		
		// Send data packet
        comms_set_packet_type(sradio, &msg, AMID_SYSTEMCOMMUNICATION);
        comms_am_set_destination(sradio, &msg, destination);
        //comms_am_set_source(sradio, &msg, radio_address); // No need, it will use the one set with radio_init
        comms_set_payload_length(sradio, &msg, sizeof(queryResponseMsg));

        comms_error_t result = comms_send(sradio, &msg, radio_send_done, NULL);
        logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
        if (COMMS_SUCCESS == result)
        {
            m_sending = true;
        }
	}
	osMutexRelease(snd_mutex);
}

void sendResBuf(uint8_t msgID, uint8_t destination, uint8_t ships[], uint8_t l)
{
	uint8_t i=0;
	while(osMutexAcquire(snd_mutex, 1000) != osOK);
	if(!m_sending)
	{
		comms_init_message(sradio, &msg);
		uint8_t * qRMsg = comms_get_payload(sradio, &msg, l+4);
		if (qRMsg == NULL)
		{
			return ;
		}

		qRMsg[0] = msgID;
		qRMsg[1] = SYSTEM_ID;
		qRMsg[2] = destination;
		qRMsg[3] = l;

		for(i=0;i<l;i++)qRMsg[4+i]=ships[i];

		// Send data packet
        comms_set_packet_type(sradio, &msg, AMID_SYSTEMCOMMUNICATION);
        comms_am_set_destination(sradio, &msg, AM_BROADCAST_ADDR);
        //comms_am_set_source(radio, &msg, radio_address); // No need, it will use the one set with radio_init
        comms_set_payload_length(sradio, &msg, l+4);

        comms_error_t result = comms_send(sradio, &msg, radio_send_done, NULL);
        logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
        if (COMMS_SUCCESS == result)
        {
            m_sending = true;
        }
	}
	osMutexRelease(snd_mutex);
}

static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
    while(osMutexAcquire(snd_mutex, 1000) != osOK);
    m_sending = false;
    osMutexRelease(snd_mutex);
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

uint8_t getIndex(uint8_t id)
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(ship_db[k].shipInGame && ship_db[k].shipAddr == id)break;
	return k;
}

static uint8_t registerNewShip(uint8_t shipAddr)
{
	uint8_t index = getIndex(shipAddr);

	if(index >= MAX_SHIPS)index = getEmptySlot();
	
	if(index < MAX_SHIPS)
	{
		getNewCoordinates(index);
		makeDTime(index);
		ship_db[index].isCargoLoaded = false;
		ship_db[index].shipAddr = shipAddr;
		ship_db[index].shipInGame = true;
	}

	return index;
}

static void getNewCoordinates(uint8_t index)
{
	uint8_t k;
	uint8_t xloc, yloc;

	for(;;)
	{
		xloc = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
		yloc = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);

		//check that no other ship is in this location
		for(k=0;k<MAX_SHIPS;k++)
		{
			if(ship_db[k].shipInGame)
			{
				if(ship_db[k].x_coordinate == xloc && ship_db[k].y_coordinate == yloc)
				{
					continue;//found a matching ship, get new coordinates
				}
			}
		}
		break ; //break loop if no matchin ship was found
	}
	ship_db[index].x_coordinate = xloc;
	ship_db[index].y_coordinate = yloc;		
}

static void makeDTime(uint8_t index)
{
	uint32_t tfrq = osKernelGetTickFreq();
	//TODO: these need to be set somehow in relation to ship location
	//TODO: check that deadline does not exceed global deadline
	ship_db[index].ltime = osKernelGetTickCount() + randomNumber(210*tfrq, 360*tfrq);
}

static uint8_t getEmptySlot()
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(!(ship_db[k].shipInGame))break;
	return k;
}

static void getAllShips(uint8_t buf[], uint8_t *len)
{
	uint8_t i, u=0;
	for(i=0;i<MAX_SHIPS;i++)if(ship_db[i].shipInGame)
	{
		buf[u]=ship_db[i].shipAddr;
		u++;
	}
	*len = u;
	return ;
}

static void getAllCargo(uint8_t buf[], uint8_t *len)
{
	uint8_t i, u=0;
	for(i=0;i<MAX_SHIPS;i++)if(ship_db[i].shipInGame)if(ship_db[i].isCargoLoaded)
	{
		buf[u]=ship_db[i].shipAddr;
		u++;
	}
	*len = u;
	return ;
}

//random number between rndL and rndH (rndL <= rnd <=rndH)
//only positive values
//user must provide correct arguments, such that 0 <= rndL < rndH

static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}
