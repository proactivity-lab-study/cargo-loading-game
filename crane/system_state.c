/**
 * 
 * This is the system module of crane-agent. It is the main game engine.
 * It keeps track of game state, registers new ships to the game and 
 * responds to all sorts of quieries about game state and state of
 * ships currently in the game. 
 * 
 * The functionality of the system module is to:
 * 
 * - initilises game state variables and ships database
 * - handles all quiery messages (query_msg_t)
 * - registers new ships to the game
 * - keeps track of game state and ships in game
 * - responds to all quierys 
 * 		-global time left
 * 		-ship data
 * 		-ships in game
 * 		-all ships with cargo
 * 
 * The first radio message to arrive, triggers random number generator
 * initialisation (seed) and also starts the game (starts game time 
 * count). Ships enter the game by sending a welcome message. When a 
 * welcome message is received, the ship is registered and its 
 * location coordinates and cargo loading deadline is randomly chosen. 
 * The ship is then informed of registration with a welcome response 
 * message. All quiery messages trigger a response message.
 * 
 * See clg_comm.h about message structures and game_types.h about 
 * default initial values and message identifiers.
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

#include "system_state.h"
#include "crane_state.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "csys"
#define __LOG_LEVEL__ (LOG_LEVEL_system_state & BASE_LOG_LEVEL)
#include "log.h"

// Global cargo loading deadline expressed as Kernel tick count, i.e. game end time
static uint32_t global_load_deadline;

static sdb_t ship_db[MAX_SHIPS];
static bool first_msg = true;

static comms_msg_t msg;
static comms_layer_t* sradio;
static am_addr_t my_address;

static osMutexId_t sdb_mutex;
static osMessageQueueId_t rcv_msg_qID, snd_msg_qID, snd_buf_qID;
static osEventFlagsId_t snd_event_id;

static void incomingMsgHandler(void *arg);
static void sendResponseMsg(void *arg);
static void sendResponseBuf(void *arg);

static uint8_t registerNewShip(am_addr_t shipAddr);
static void genNewCoordinates(uint8_t index);
static void genLoadTime(uint8_t index);
static uint8_t getEmptySlot();
static uint8_t getAllShips(am_addr_t buf[], uint8_t len);
static uint8_t getAllCargo(am_addr_t buf[], uint8_t len);
static uint32_t randomNumber(uint32_t rndL, uint32_t rndH);

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_game()
{
	srand(osKernelGetTickCount()); // Initialise random number generator
	init_crane_loc(); // Crane location
	global_load_deadline = osKernelGetTickCount() + (DURATION_OF_GAME - 1) * osKernelGetTickFreq();
}

void init_system(comms_layer_t* radio, am_addr_t my_addr)
{
	uint8_t i=0;

	sdb_mutex = osMutexNew(NULL); // Protects registered ship database

	while(osMutexAcquire(sdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ship_db[i].shipInGame = false;
		ship_db[i].shipAddr = 0;
		ship_db[i].x_coordinate = DEFAULT_LOC;
		ship_db[i].y_coordinate = DEFAULT_LOC;
		ship_db[i].ltime = osKernelGetTickCount() + DEFAULT_TIME * osKernelGetTickFreq();
		ship_db[i].isCargoLoaded = false;
	}
	osMutexRelease(sdb_mutex);

	sradio = radio;
	my_address = my_addr;

	rcv_msg_qID = osMessageQueueNew(9, sizeof(query_msg_t), NULL);	// For received messages
	snd_msg_qID = osMessageQueueNew(9, sizeof(query_response_msg_t), NULL);	// For response messages
	snd_buf_qID = osMessageQueueNew(9, sizeof(query_response_buf_t), NULL);	// For response messages

	snd_event_id = osEventFlagsNew(NULL);  // Using one event flag for two send threads. Possible starvation??
	osEventFlagsSet(snd_event_id, 0x00000001U); // Sets send threads to ready-to-send state

	osThreadNew(incomingMsgHandler, NULL, NULL);	// Handles incoming messages and responses to
	osThreadNew(sendResponseMsg, NULL, NULL);	// Sends query_response_msg_t response messages
	osThreadNew(sendResponseBuf, NULL, NULL);	// Sends query_response_buf_t response messages
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	// First message that is received triggers random number generator init
	// and subsequently starts the game
	if(first_msg)
	{
		init_game();
		first_msg = false;
	}

	if (comms_get_payload_length(comms, msg) == sizeof(query_msg_t))
	{
	    query_msg_t * packet = (query_msg_t*)comms_get_payload(comms, msg, sizeof(query_msg_t));
		info1("Rcv qry");		
		osStatus_t err = osMessageQueuePut(rcv_msg_qID, packet, 0, 0);
		if(err == osOK)info1("rc query");
		else warn1("msgq err");
	}
	else warn1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

static void incomingMsgHandler(void *arg)
{
	uint8_t ndx;
	query_msg_t packet; 
	query_response_msg_t rpacket;
	query_response_buf_t bpacket;

	for(;;)
	{
		osMessageQueueGet(rcv_msg_qID, &packet, NULL, osWaitForever);
		switch(packet.messageID)
		{
			case WELCOME_MSG:
				while(osMutexAcquire(sdb_mutex, 1000) != osOK);
				ndx = registerNewShip(ntoh16(packet.senderAddr));
				if(ndx >= MAX_SHIPS)
				{
					info1("No room");
				}
				else 
				{
					info1("New ship %lu", ntoh16(packet.senderAddr));

					rpacket.messageID = WELCOME_RMSG;
					rpacket.senderAddr = ship_db[ndx].shipAddr; // Piggybacking destination address here
					rpacket.shipAddr = ship_db[ndx].shipAddr;
					rpacket.loadingDeadline = (uint16_t)((ship_db[ndx].ltime - osKernelGetTickCount()) / osKernelGetTickFreq());
					rpacket.x_coordinate = ship_db[ndx].x_coordinate;
					rpacket.y_coordinate = ship_db[ndx].y_coordinate;
					rpacket.isCargoLoaded = ship_db[ndx].isCargoLoaded;
					osMessageQueuePut(snd_msg_qID, &rpacket, 0, 0);
				}
				osMutexRelease(sdb_mutex);

			break;

			case GTIME_QMSG:
				rpacket.messageID = GTIME_QRMSG;
				rpacket.senderAddr = ntoh16(packet.senderAddr); // Piggybacking destination address here
				rpacket.shipAddr = ntoh16(packet.senderAddr);
				rpacket.loadingDeadline = (uint16_t)((global_load_deadline - osKernelGetTickCount()) / osKernelGetTickFreq());
				rpacket.x_coordinate = DEFAULT_LOC;
				rpacket.y_coordinate = DEFAULT_LOC;
				rpacket.isCargoLoaded = false;
				osMessageQueuePut(snd_msg_qID, &rpacket, 0, 0);

			break;

			case SHIP_QMSG:
				info1("Ship qry %lu %lu", ntoh16(packet.senderAddr), ntoh16(packet.shipAddr));
				while(osMutexAcquire(sdb_mutex, 1000) != osOK);
				ndx = getIndex(ntoh16(packet.senderAddr));
				rpacket.messageID = SHIP_QRMSG;
				rpacket.senderAddr = ntoh16(packet.senderAddr); // Piggybacking destination address here
				rpacket.shipAddr = ship_db[ndx].shipAddr;
				rpacket.loadingDeadline = (uint16_t)((ship_db[ndx].ltime - osKernelGetTickCount()) / osKernelGetTickFreq());
				rpacket.x_coordinate = ship_db[ndx].x_coordinate;
				rpacket.y_coordinate = ship_db[ndx].y_coordinate;
				rpacket.isCargoLoaded = ship_db[ndx].isCargoLoaded;
				osMessageQueuePut(snd_msg_qID, &rpacket, 0, 0);
				osMutexRelease(sdb_mutex);

			break;

			case AS_QMSG:
				info1("AShip qry %lu", ntoh16(packet.senderAddr));
				bpacket.messageID = AS_QRMSG;
				bpacket.senderAddr = SYSTEM_ADDR;
				bpacket.shipAddr = ntoh16(packet.senderAddr);
				while(osMutexAcquire(sdb_mutex, 1000) != osOK);
				bpacket.len = getAllShips(bpacket.ships, MAX_SHIPS);
				osMutexRelease(sdb_mutex);			
				
				osMessageQueuePut(snd_buf_qID, &bpacket, 0, 0);

			break;

			case ACARGO_QMSG:
				bpacket.messageID = ACARGO_QRMSG;
				bpacket.senderAddr = SYSTEM_ADDR;
				bpacket.shipAddr = ntoh16(packet.senderAddr);
				while(osMutexAcquire(sdb_mutex, 1000) != osOK);
				bpacket.len = getAllCargo(bpacket.ships, MAX_SHIPS);
				osMutexRelease(sdb_mutex);

				osMessageQueuePut(snd_buf_qID, &bpacket, 0, 0);

			break;

			default: 
			break; // Do nothing, except drop this quiery
		}
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/
static void radioSendDone(comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
	osEventFlagsSet(snd_event_id, 0x00000001U);
}

static void sendResponseMsg(void *arg)
{
	query_response_msg_t packet;
	am_addr_t dest;

	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);

		osEventFlagsWait(snd_event_id, 0x00000001U, osFlagsWaitAny, osWaitForever); // Flags automatically cleared

		comms_init_message(sradio, &msg);
		query_response_msg_t * qRMsg = comms_get_payload(sradio, &msg, sizeof(query_response_msg_t));
		if (qRMsg == NULL)
		{
			continue ; // Continue for(;;) loop
		}
		dest = packet.senderAddr;
		qRMsg->messageID = packet.messageID;
		qRMsg->senderAddr = hton16((uint16_t)SYSTEM_ADDR);
		qRMsg->shipAddr = hton16(packet.shipAddr);
		qRMsg->loadingDeadline = hton16(packet.loadingDeadline); // hton16() ensures correct endianness
		qRMsg->x_coordinate = packet.x_coordinate;
		qRMsg->y_coordinate = packet.y_coordinate;
		qRMsg->isCargoLoaded = packet.isCargoLoaded;
	
		// Send data packet
	    comms_set_packet_type(sradio, &msg, AMID_SYSTEMCOMMUNICATION);
	    comms_am_set_destination(sradio, &msg, dest);
	    //comms_am_set_source(sradio, &msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(sradio, &msg, sizeof(query_response_msg_t));

	    comms_error_t result = comms_send(sradio, &msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

static void sendResponseBuf(void *arg)
{
	uint8_t i=0;
	query_response_buf_t packet;
	for(;;)
	{
		osMessageQueueGet(snd_buf_qID, &packet, NULL, osWaitForever);

		osEventFlagsWait(snd_event_id, 0x00000001U, osFlagsWaitAny, osWaitForever); // Flags automatically cleared

		comms_init_message(sradio, &msg);
		query_response_buf_t * qRMsg = comms_get_payload(sradio, &msg, sizeof(query_response_buf_t));
		if (qRMsg == NULL)
		{
			continue ;//continue for(;;) loop
		}

		qRMsg->messageID = packet.messageID;
		qRMsg->senderAddr = hton16(packet.senderAddr);
		qRMsg->shipAddr = hton16(packet.shipAddr);
		qRMsg->len = packet.len;
		for(i=0;i<packet.len;i++)
		{
			qRMsg->ships[i]=hton16(packet.ships[i]);
		}

		// Send data packet
	    comms_set_packet_type(sradio, &msg, AMID_SYSTEMCOMMUNICATION);
	    comms_am_set_destination(sradio, &msg, packet.shipAddr);
	    //comms_am_set_source(radio, &msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(sradio, &msg, sizeof(query_response_buf_t));

	    comms_error_t result = comms_send(sradio, &msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "sndb %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns buffer index of ship with address 'id' or value MAX_SHIPS if no such ship.
uint8_t getIndex(am_addr_t id)
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(ship_db[k].shipInGame && ship_db[k].shipAddr == id)break;
	return k;
}

// Returns address of ship in location 'x', 'y' or 0 if no ship in this location.
// This function can block.
am_addr_t isShipHere(uint8_t x, uint8_t y)
{
	am_addr_t addr = 0;
	uint8_t i;

	while(osMutexAcquire(sdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(ship_db[i].x_coordinate == x && ship_db[i].y_coordinate == y && ship_db[i].shipInGame)	
		{
			addr = ship_db[i].shipAddr;
			break;
		}
	}
	osMutexRelease(sdb_mutex);
	return addr;
}

// Marks cargo status as true for ship with address 'addr', if such a ship is found.
// Use with care! There is no revers command to mark cargo status false.
// This function can block.
void markCargo(am_addr_t addr)
{
	uint8_t i;
	while(osMutexAcquire(sdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)if(ship_db[i].shipAddr == addr && ship_db[i].shipInGame)
	{
		ship_db[i].isCargoLoaded = true;
		break;
	}
	osMutexRelease(sdb_mutex);
}

static uint8_t registerNewShip(am_addr_t shipAddr)
{
	uint8_t index = getIndex(shipAddr);

	if(index >= MAX_SHIPS)
	{
		index = getEmptySlot();
	
		if(index < MAX_SHIPS)
		{
			genNewCoordinates(index);
			genLoadTime(index);
			ship_db[index].isCargoLoaded = false;
			ship_db[index].shipAddr = shipAddr;
			ship_db[index].shipInGame = true;
		}
	}
	else ; // Ship already registered
	return index;
}

static void genNewCoordinates(uint8_t index)
{
	uint8_t k;
	uint8_t xloc, yloc;

	for(;;)
	{
		xloc = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);
		yloc = randomNumber(GRID_LOWER_BOUND, GRID_UPPER_BOUND);

		// Check that no other ship is in this location
		for(k=0;k<MAX_SHIPS;k++)
		{
			if(ship_db[k].shipInGame)
			{
				if(ship_db[k].x_coordinate == xloc && ship_db[k].y_coordinate == yloc)
				{
					break ; // Found a matching ship, get new coordinates
				}
			}
		}

		if(k>=MAX_SHIPS)break; // Unique coordinates, break loop
		else ; // Another ship already in this location, do loop again
	}

	ship_db[index].x_coordinate = xloc;
	ship_db[index].y_coordinate = yloc;		
}

static void genLoadTime(uint8_t index)
{
	//TODO: MIN-MAX need to be set somehow in relation to ship-crane distance
	uint32_t ldkt;

	ldkt = randomNumber(MIN_LOADING_TIME, MAX_LOADING_TIME) * osKernelGetTickFreq();
	ldkt += osKernelGetTickCount();

	if(ldkt > global_load_deadline)ldkt = global_load_deadline; // Sry, your time is cut short
	ship_db[index].ltime = ldkt;
}

static uint8_t getEmptySlot()
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(!(ship_db[k].shipInGame))break;
	return k;
}

static uint8_t getAllShips(am_addr_t buf[], uint8_t len)
{
	uint8_t i, u=0;
	for(i=0;i<MAX_SHIPS;i++)if(ship_db[i].shipInGame)
	{
		if(u<len)buf[u++]=ship_db[i].shipAddr;
		else break;
	}
	return u;
}

static uint8_t getAllCargo(am_addr_t buf[], uint8_t len)
{
	uint8_t i, u=0;
	for(i=0;i<MAX_SHIPS;i++)if(ship_db[i].shipInGame && ship_db[i].isCargoLoaded)
	{
		if(u<len)buf[u++]=ship_db[i].shipAddr;
		else break;
	}
	return u;
}

// Random number between rndL and rndH (rndL <= rnd <=rndH)
// Only positive values
// User must provide correct arguments, such that 0 <= rndL < rndH

static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}
