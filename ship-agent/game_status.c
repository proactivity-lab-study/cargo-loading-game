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
#include "endianness.h"

#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "gstat"
#define __LOG_LEVEL__ (LOG_LEVEL_game_status & BASE_LOG_LEVEL)
#include "log.h"

typedef struct ship_data_t{
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


static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static am_addr_t system_address = AM_BROADCAST_ADDR; // Use actual system address if possible
static bool first_msg = true; // Used to get actual system address once

static bool get_all_ships_data_in_progress = false; // Protected by asdb_mutex

static void welcome_msg_loop(void *args);
static void send_msg_loop(void *args);
static void get_all_ships_data(void *args);
static void get_all_ships_in_game(void *args);

static osEventFlagsId_t evt_id;

static uint8_t get_empty_slot();
static uint8_t get_index(am_addr_t addr);
static void add_ship(query_response_msg_t* ship);
static void add_ship_addr(am_addr_t addr);


/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_system_status(comms_layer_t* radio, am_addr_t addr)
{
	uint8_t i;
	const osMutexAttr_t sddb_Mutex_attr = { .attr_bits = osMutexRecursive }; // Allow nesting of this mutex

	sddb_mutex = osMutexNew(&sddb_Mutex_attr);	// Protects ships' crane command database
	asdb_mutex = osMutexNew(NULL);				// Protects current crane location values

	evt_id = osEventFlagsNew(NULL);	// Tells 'get_all_ships_data' task to quiery for the next ship

	snd_msg_qID = osMessageQueueNew(9, sizeof(query_msg_t), NULL);
	
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

	wmsg_thread = osThreadNew(welcome_msg_loop, NULL, NULL); // Sends welcome message and then stops
	snd_task_id = osThreadNew(send_msg_loop, NULL, NULL); // Sends quiery messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
	osThreadNew(get_all_ships_data, NULL, NULL);
	osThreadNew(get_all_ships_in_game, NULL, NULL); // Sends AS_QMSG message	
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void get_all_ships_in_game(void *args)
{
	query_msg_t packet;
	
	for(;;)
	{
		osDelay(60*osKernelGetTickFreq()); // 60 seconds
		packet.messageID = AS_QMSG;
		packet.senderAddr = my_address;
		osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	}
}

static void get_all_ships_data(void *args)
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
				osMutexRelease(asdb_mutex);
				osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
				break;
			}
		}
		if(i >= MAX_SHIPS)
		{
			get_all_ships_data_in_progress = false; // Done
			osMutexRelease(asdb_mutex);
		}
	}
}

static void welcome_msg_loop(void *args)
{
	query_msg_t packet;
	for(;;)
	{
		while(osMutexAcquire(sddb_mutex, 1000) != osOK);
		if(get_index(my_address) >= MAX_SHIPS)
		{
			osMutexRelease(sddb_mutex);
			packet.messageID = WELCOME_MSG;
			packet.senderAddr = my_address;
			info1("Snd wlcme");
			osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
		}
		else 
		{
			osMutexRelease(sddb_mutex);
			// Terminate task, cuz no need for it anymore
			osThreadTerminate(wmsg_thread);
		}
		
		osDelay(10*osKernelGetTickFreq()); // 10 seconds
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t i;
	am_addr_t dest;
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	query_response_msg_t * packet;
	query_response_buf_t * bpacket;
	query_msg_t packet2;
	
	//TODO maybe put this all in a separate task, to make this interrupt handler faster
	switch(rmsg[0])
	{
		// New ship, maybe do something? trigger reminder to ask for new ship?
		case WELCOME_MSG :
		break;

		// Query messages, nothing to do
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
			info1("Rcv wlcme");
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			add_ship(packet);
			osMutexRelease(sddb_mutex);
			
			dest = comms_am_get_destination(comms, msg);
			if(dest == my_address)
			{
				info1("My loc %u %u", packet->x_coordinate, packet->y_coordinate);
				packet2.messageID = AS_QMSG;
				packet2.senderAddr = my_address;
				osMessageQueuePut(snd_msg_qID, &packet2, 0, 1000);
			}

			case SHIP_QRMSG :
			packet = (query_response_msg_t *) comms_get_payload(comms, msg, sizeof(query_response_msg_t));
			
			info1("Rcv ship");
			if(ntoh16(packet->shipAddr) == my_address)info1("My loc %u %u", packet->x_coordinate, packet->y_coordinate);
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			add_ship(packet);
			osMutexRelease(sddb_mutex);

			// Trigger get_all_ships_data() task 
			dest = comms_am_get_destination(comms, msg);
			while(osMutexAcquire(asdb_mutex, 1000) != osOK);
			if(get_all_ships_data_in_progress && dest == my_address)osEventFlagsSet(evt_id, 0x00000001U);
			osMutexRelease(asdb_mutex);

			break;

		case AS_QRMSG :

			bpacket = (query_response_buf_t *) comms_get_payload(comms, msg, sizeof(query_response_buf_t));
			if(ntoh16(bpacket->shipAddr) == my_address) // Only if I made quiery
			{
				while(osMutexAcquire(asdb_mutex, 1000) != osOK);
				for(i=0;i<bpacket->len;i++)
				{
					add_ship_addr(ntoh16(bpacket->ships[i]));
				}
				get_all_ships_data_in_progress = true;
				osMutexRelease(asdb_mutex);
				osEventFlagsSet(evt_id, 0x00000001U); // Trigger get_all_ships_data() task 
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

static void radio_send_done (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt-gs %u", result);
	osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void send_msg_loop(void *args)
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
	    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
	    comms_set_payload_length(sradio, &m_msg, sizeof(query_msg_t));

	    comms_error_t result = comms_send(sradio, &m_msg, radio_send_done, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd-gs %u", result);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns location of ship, if no such ship, returns 0 for both coordinates.
// This function can block.
loc_bundle_t getShipLocation(am_addr_t ship_addr)
{
	loc_bundle_t sloc;
	uint8_t ndx;
	
	sloc.x = sloc.y = 0;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	ndx = get_index(ship_addr);
	if(ndx < MAX_SHIPS)
	{
		sloc.x = ships[ndx].x_coordinate;
		sloc.y = ships[ndx].y_coordinate;
	}
	osMutexRelease(sddb_mutex);
	return sloc;
}

// Returns address of ship in location 'sloc' or 0 if no ship in this location.
// This function can block.
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
// Returns number of ships added to buffer 'saddr'.
// This function can block.
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
// This function can block.
void markCargo(am_addr_t addr)
{
	uint8_t i;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)if(ships[i].ship_addr == addr && ships[i].ship_in_game)
	{
		ships[i].is_cargo_loaded = true;
		break;
	}
	osMutexRelease(sddb_mutex);
}

// Returns cargo status of ship 'ship_addr'. Possible return values:
// 0 - cargo has been received, cargo present
// 1 - cargo has not been received, cargo not present
// 2 - ship not in database, unknown ship
// This function can block.
uint8_t getCargoStatus(am_addr_t ship_addr)
{
	uint8_t i, stat = 2;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)if(ships[i].ship_addr == ship_addr && ships[i].ship_in_game)
	{
		if(ships[i].is_cargo_loaded)stat = 0;
		else stat = 1;
	}
	osMutexRelease(sddb_mutex);
	return stat;
}

static uint8_t get_empty_slot()
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].ship_in_game == false)break;
	}
	return k;
}

static uint8_t get_index(am_addr_t addr)
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].ship_in_game && ships[k].ship_addr == addr)break;
	}
	return k;
}

static void add_ship_addr(am_addr_t addr)
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
static void add_ship(query_response_msg_t* ship)
{
	uint8_t ndx;
	
	ndx = get_index(ntoh16(ship->shipAddr));
	if(ndx >= MAX_SHIPS)
	{
		ndx = get_empty_slot();
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
