/**
 * 
 * This is the game status module of ship-agent. It is responsible for keeping track
 * of game status. Game status includes game time and information about ships 
 * (including self) and their status (address, location, cargo status, departure time). 
 * 
 * The main functionality of this module is to
 * 
 * - send WELCOME_MSG every GS_WELCOME_MSG_RETRY_INTERVAL seconds until a WELCOME_RMSG
 *   is received 
 * - perform game status update every GS_UPDATE_INTERVAL seconds, including
 * 		- info about new ships in game
 * 		- info about ship status
 * - send different game state query messages to crane-agent
 * - receive different game state messages from crane-agent
 * - provide utility functions for crane control module and ship strategy module to
 *   get and set game status information (see game_status.h).
 * 
 * Game status module is mostly a database of information about ships active in the
 * current game. Different actions are taken to keep this database as up to date as 
 * possible. In good radio transmission conditions, where messages are seldom lost, 
 * this database should be up to date to well under a second. The database can hold
 * MAX_SHIPS (see game_types.h) number of ships. 
 * 
 * Note:
 * 		There is currently no mechanism for a ship to publicly announce leaving the 
 * 		game or becoming inactive. 
 * 
 * Note:
 * 		Cargo status of ships is set to false after initialisation and there is no
 * 		mechanism to reset it to false again during the game. So take care when 
 * 		cargo status of a ship is set to true, there is no going back after this 
 * 		action.
 * 
 * TODO Regular cargo status updates.
 * 
 * TODO Mechanism to leave the game.
 * 
 * TODO CRANE_ADDR and SYSYEM_ADDR are still used to identify crane-agent. This
 * 		however does not solve crane-agent identity theft and impersonation problem
 * 		so in this regard it is redundant. Suggested for removal.
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

#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "gstat"
#define __LOG_LEVEL__ (LOG_LEVEL_game_status & BASE_LOG_LEVEL)
#include "log.h"

#define GS_UPDATE_INTERVAL 60 				// Update game state, seconds
#define GS_WELCOME_MSG_RETRY_INTERVAL 10 	// Retry welcome message, seconds

typedef struct {
	bool ship_in_game;
	am_addr_t ship_addr; 
	uint16_t ship_deadline;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t is_cargo_loaded;
} ship_data_t;

static am_addr_t ship_addr[MAX_SHIPS]; // Protected by asdb_mutex

static ship_data_t ships[MAX_SHIPS]; // Protected by sddb_mutex

uint32_t global_time_left; // Protected by sddb_mutex

static osMutexId_t sddb_mutex, asdb_mutex;
static osMessageQueueId_t snd_msg_qID;
static osThreadId_t wmsg_thread, snd_task_id;
static osEventFlagsId_t evt_id;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static am_addr_t system_address = AM_BROADCAST_ADDR; // Use actual system address if possible
static bool first_msg = true; // Used to get actual system address once

static void welcomeMsgLoop(void *args);
static void sendMsgLoop(void *args);
static void getAllShipsData(void *args);
static void getAllShipsIngame(void *args);

static uint8_t getEmptySlot();
static uint8_t getIndex(am_addr_t addr);
static void addShip(query_response_msg_t* ship);
static void addShipAddr(am_addr_t addr);


/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initSystemStatus(comms_layer_t* radio, am_addr_t addr)
{
	uint8_t i;
	const osMutexAttr_t sddb_Mutex_attr = { .attr_bits = osMutexRecursive }; // Allow nesting of this mutex

	sddb_mutex = osMutexNew(&sddb_Mutex_attr);	// Protects ships' crane command database
	asdb_mutex = osMutexNew(NULL);				// Protects current crane location values

	evt_id = osEventFlagsNew(NULL);	// Tells 'getAllShipsData' task to quiery for the next ship

	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, sizeof(query_msg_t), NULL);
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	// Initialise ships' buffers
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ships[i].ship_in_game = false;
		ships[i].ship_addr = 0;
		ships[i].ship_deadline = 0;
		ships[i].x_coordinate = 0;
		ships[i].y_coordinate = 0;
		ships[i].is_cargo_loaded = false;
	}
	osMutexRelease(sddb_mutex);

	while(osMutexAcquire(asdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ship_addr[i] = 0;
	}
	osMutexRelease(asdb_mutex);

	wmsg_thread = osThreadNew(welcomeMsgLoop, NULL, NULL); // Sends welcome message and then stops
	snd_task_id = osThreadNew(sendMsgLoop, NULL, NULL); // Sends quiery messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
	osThreadNew(getAllShipsData, NULL, NULL);
	osThreadNew(getAllShipsIngame, NULL, NULL); // Sends AS_QMSG message	
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void getAllShipsIngame(void *args)
{
	query_msg_t packet;
	
	for(;;)
	{
		osDelay(GS_UPDATE_INTERVAL*osKernelGetTickFreq());
		packet.messageID = AS_QMSG;
		packet.senderAddr = my_address;
		osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	}
}

static void getAllShipsData(void *args)
{
	uint8_t i;
	query_msg_t packet;

	for(;;)
	{
		osEventFlagsWait(evt_id, 0x00000001U, osFlagsWaitAny, osWaitForever);
		while(osMutexAcquire(asdb_mutex, 1000) != osOK);
		for(i=0;i<MAX_SHIPS;i++)
		{
			if(ship_addr[i] != 0)// TODO maybe also see if we have data for this ship
			{
				packet.messageID = SHIP_QMSG;
				packet.senderAddr = my_address;
				packet.shipAddr = ship_addr[i];
				ship_addr[i] = 0;
				osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
			}
		}
		osMutexRelease(asdb_mutex);
	}
}

static void welcomeMsgLoop(void *args)
{
	query_msg_t packet;
	for(;;)
	{
		while(osMutexAcquire(sddb_mutex, 1000) != osOK);
		if(getIndex(my_address) >= MAX_SHIPS)
		{
			osMutexRelease(sddb_mutex);
			packet.messageID = WELCOME_MSG;
			packet.senderAddr = my_address;
			info1("Send welcome");
			osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
		}
		else 
		{
			osMutexRelease(sddb_mutex);
			// Terminate task, cuz no need for it anymore
			osThreadTerminate(wmsg_thread);
		}
		
		osDelay(GS_WELCOME_MSG_RETRY_INTERVAL*osKernelGetTickFreq());
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void systemReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t i;
	am_addr_t dest;
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	query_response_msg_t * packet;
	query_response_buf_t * bpacket;
	query_msg_t packet2;
	
	switch(rmsg[0])
	{
		// New ship, trigger make ship quiery
		case WELCOME_MSG :
			packet2.messageID = SHIP_QMSG;
			packet2.senderAddr = my_address;
			packet2.shipAddr = comms_am_get_source(comms, msg);
			osMessageQueuePut(snd_msg_qID, &packet2, 0, osWaitForever);
			break;

		// Query messages, nothing to do, we should not get these
		case GTIME_QMSG :
		case SHIP_QMSG :
		case AS_QMSG :
		case ACARGO_QMSG :
			break;

		case GTIME_QRMSG :
			packet = (query_response_msg_t *) comms_get_payload(comms, msg, sizeof(query_response_msg_t));
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			global_time_left = ntoh16(packet->loadingDeadline);
			osMutexRelease(sddb_mutex);
			break;

		case WELCOME_RMSG :
			packet = (query_response_msg_t *) comms_get_payload(comms, msg, sizeof(query_response_msg_t));

			// WELCOME_RMSG should be one of the first ones we receive, so lets put this here
			if(ntoh16(packet->senderAddr) == SYSTEM_ADDR && first_msg)
			{
				system_address = comms_am_get_source(comms, msg);
				first_msg = false;
			}

			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			addShip(packet);
			osMutexRelease(sddb_mutex);
			
			dest = comms_am_get_destination(comms, msg);
			if(dest == my_address)
			{
				info1("Rcv wlcm my loc %u %u", packet->x_coordinate, packet->y_coordinate);
				packet2.messageID = AS_QMSG;
				packet2.senderAddr = my_address;
				osMessageQueuePut(snd_msg_qID, &packet2, 0, 1000);
			}
			break;

			case SHIP_QRMSG :
			packet = (query_response_msg_t *) comms_get_payload(comms, msg, sizeof(query_response_msg_t));
			
			info1("Rcv ship %u loc %u %u", ntoh16(packet->shipAddr), packet->x_coordinate, packet->y_coordinate);
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			addShip(packet);
			osMutexRelease(sddb_mutex);

			break;

		case AS_QRMSG :

			bpacket = (query_response_buf_t *) comms_get_payload(comms, msg, sizeof(query_response_buf_t));
			if(ntoh16(bpacket->shipAddr) == my_address) // Only if I made quiery
			{
				while(osMutexAcquire(asdb_mutex, 1000) != osOK);
				for(i=0;i<bpacket->len;i++)
				{
					addShipAddr(ntoh16(bpacket->ships[i]));
				}
				osMutexRelease(asdb_mutex);
				osEventFlagsSet(evt_id, 0x00000001U); // Trigger getAllShipsData() task 
			}
			break;

		case ACARGO_QRMSG :

			bpacket = (query_response_buf_t *) comms_get_payload(comms, msg, sizeof(query_response_buf_t));
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			for(i=0;i<bpacket->len;i++)
			{
				markCargo(ntoh16(bpacket->ships[i]));
			}
			osMutexRelease(sddb_mutex);
			break;

		default:
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

static void sendMsgLoop(void *args)
{
	query_msg_t packet;
	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);

		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(sradio, &m_msg);
		query_msg_t * qmsg = comms_get_payload(sradio, &m_msg, sizeof(query_msg_t));
		if (qmsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}
		qmsg->messageID = packet.messageID;
		qmsg->senderAddr = hton16(packet.senderAddr);
		qmsg->shipAddr = hton16(packet.shipAddr);

		// Send data packet
	    comms_set_packet_type(sradio, &m_msg, AMID_SYSTEMCOMMUNICATION);
	    comms_am_set_destination(sradio, &m_msg, system_address);
	    comms_set_payload_length(sradio, &m_msg, sizeof(query_msg_t));

	    comms_error_t result = comms_send(sradio, &m_msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns location of ship, if no such ship, returns 0 for both coordinates.
loc_bundle_t getShipLocation(am_addr_t ship_addr)
{
	loc_bundle_t sloc;
	uint8_t ndx;
	
	sloc.x = sloc.y = 0;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	ndx = getIndex(ship_addr);
	if(ndx < MAX_SHIPS)
	{
		sloc.x = ships[ndx].x_coordinate;
		sloc.y = ships[ndx].y_coordinate;
	}
	osMutexRelease(sddb_mutex);
	return sloc;
}

// Returns address of ship in location 'sloc' or 0 if no ship in this location.
am_addr_t getShipAddr(loc_bundle_t sloc)
{
	am_addr_t addr = 0;
	uint8_t i;

	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(ships[i].x_coordinate == sloc.x && ships[i].y_coordinate == sloc.y && ships[i].ship_in_game)	
		{
			addr = ships[i].ship_addr;
			break;
		}
	}
	osMutexRelease(sddb_mutex);
	return addr;
}

// Fills buffer pointed to by 'saddr' with addresses of all ships currently known.
// Own address is included.
// Returns number of ships added to buffer 'saddr'.
uint8_t getAllShipsAddr(am_addr_t saddr[], uint8_t mlen)
{
	uint8_t i, len = 0;

	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(ships[i].ship_in_game && len < mlen)saddr[len++] = ships[i].ship_addr;
	}
	osMutexRelease(sddb_mutex);
	return len;
}

// Marks cargo status as true for ship with address 'addr', if such a ship is found.
// Use with care! There is no revers command to mark cargo status false.
void markCargo(am_addr_t addr)
{
	uint8_t i;
	static am_addr_t saddr;
	saddr = addr;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)if(ships[i].ship_addr == addr && ships[i].ship_in_game)
	{
		ships[i].is_cargo_loaded = true;
		break;
	}
	osMutexRelease(sddb_mutex);
	info1("Cargo placed %lu", saddr);
}

// Returns cargo status of ship 'ship_addr'. Possible return values:
// cs_cargo_received - cargo has been received, cargo present
// cs_cargo_not_received - cargo has not been received, cargo not present
// cs_unknown_ship_addr - ship not in database, unknown ship
cargo_status_t getCargoStatus(am_addr_t ship_addr)
{
	uint8_t i;
	cargo_status_t stat = cs_unknown_ship_addr;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)if(ships[i].ship_addr == ship_addr && ships[i].ship_in_game)
	{
		if(ships[i].is_cargo_loaded)stat = cs_cargo_received;
		else stat = cs_cargo_not_received;
	}
	osMutexRelease(sddb_mutex);
	return stat;
}

static uint8_t getEmptySlot()
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].ship_in_game == false)break;
	}
	return k;
}

static uint8_t getIndex(am_addr_t addr)
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].ship_in_game && ships[k].ship_addr == addr)break;
	}
	return k;
}

static void addShipAddr(am_addr_t addr)
{
	uint8_t i;

	for(i=0;i<MAX_SHIPS;i++)if(ship_addr[i] == addr)break; // Check that we don't add double ships

	if(i>=MAX_SHIPS)
	{
		for(i=0;i<MAX_SHIPS;i++)if(ship_addr[i] == 0)
		{
			ship_addr[i] = addr;
			break;
		}
	}
	else ; // This ship is already in database
}

// Input argument is network packet, so use ntoh functions to read values
static void addShip(query_response_msg_t* ship)
{
	uint8_t ndx;
	
	ndx = getIndex(ntoh16(ship->shipAddr));
	if(ndx >= MAX_SHIPS)
	{
		ndx = getEmptySlot();
		if(ndx < MAX_SHIPS)
		{
			ships[ndx].ship_in_game = true;
			ships[ndx].ship_addr = ntoh16(ship->shipAddr);
			ships[ndx].ship_deadline = ntoh16(ship->loadingDeadline);
			ships[ndx].x_coordinate = ship->x_coordinate;
			ships[ndx].y_coordinate = ship->y_coordinate;
			ships[ndx].is_cargo_loaded = ship->isCargoLoaded;
		}
		else ; // No room
	}
	else // Already got this ship, update only cargo status
	{
		ships[ndx].is_cargo_loaded = ship->isCargoLoaded;
	}
}
