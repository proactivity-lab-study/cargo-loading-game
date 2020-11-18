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

#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"

#include "loglevels.h"
#define __MODUUL__ "gamestatus"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

typedef struct ship_data_t{ //TODO replace with sdb_t structure??
	bool shipInGame;
	am_addr_t shipAdd; 
	uint16_t departureT;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t isCargoLoaded;
} ship_data_t;

am_addr_t ship_addr[MAX_SHIPS]; //protected by asdb_mutex

ship_data_t ships[MAX_SHIPS]; //protected by sddb_mutex
ship_data_t my_data; //protected by sddb_mutex

uint32_t global_time_left; //protected by sddb_mutex

static osMutexId_t sddb_mutex, asdb_mutex, snd_mutex;
static osMessageQueueId_t snd_msg_qID;

static comms_msg_t m_msg;
static bool m_sending = false;
static comms_layer_t* sradio;
am_addr_t my_address;

bool get_all_ships_data_in_progress = false; //protected by asdb_mutex

void welcome_msg_loop(void *args);
void send_msg_loop(void *args);
void get_all_ships_data(void *args);
void get_all_ships_in_game(void *args);

osEventFlagsId_t evt_id;

static uint8_t get_empty_slot();
static uint8_t get_index(am_addr_t addr);
static void add_ship(queryResponseMsg* ship);
static void mark_cargo(am_addr_t addr);
static void add_ship_addr(am_addr_t addr);


/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_system_status(comms_layer_t* radio, am_addr_t addr)
{
	uint8_t i;

	sddb_mutex = osMutexNew(NULL);	//protects ships' crane command database
	asdb_mutex = osMutexNew(NULL);	//protects current crane location values
	snd_mutex = osMutexNew(NULL);	//protects against sending another message before hardware has handled previous message
	evt_id = osEventFlagsNew(NULL);	//tells get_all_ships_data to quiery the next ship

	snd_msg_qID = osMessageQueueNew(9, sizeof(queryMsg), NULL);
	
	sradio = radio;//this is the only write, so not going to protect it with mutex
	my_address = addr;//this is the only write, so not going to protect it with mutex

	//initialise ships' buffers
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ships[i].shipInGame = false;
		ships[i].shipAdd = 0;
		ships[i].departureT = 0;
		ships[i].x_coordinate = 0;
		ships[i].y_coordinate = 0;
		ships[i].isCargoLoaded = false;
	}
	my_data.shipInGame = false;
	my_data.shipAdd = 0;
	my_data.departureT = 0;
	my_data.x_coordinate = 0;
	my_data.y_coordinate = 0;
	my_data.isCargoLoaded = false;
	osMutexRelease(sddb_mutex);

	while(osMutexAcquire(asdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ship_addr[i] = 0;
	}
	osMutexRelease(asdb_mutex);

	osThreadNew(welcome_msg_loop, NULL, NULL);//sends welcome message
	osThreadNew(get_all_ships_data, NULL, NULL);
	osThreadNew(send_msg_loop, NULL, NULL);//sends quiery messages
	osThreadNew(get_all_ships_in_game, NULL, NULL);//sends AS_QMSG message	
}

/**********************************************************************************************
 *	 some threads
 *********************************************************************************************/

void get_all_ships_in_game(void *args)
{
	queryMsg packet;
	am_addr_t my_addr = my_address;
	
	for(;;)
	{
		osDelay(60*osKernelGetTickFreq()); //60 seconds
		packet.messageID = AS_QMSG;
		packet.senderAddr = my_addr;
		osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	}
}

void get_all_ships_data(void *args)
{
	uint8_t i;
	queryMsg packet;
	am_addr_t my_addr = my_address;

	for(;;)
	{
		osEventFlagsWait(evt_id, 0x00000001U, osFlagsWaitAny, osWaitForever);
		while(osMutexAcquire(asdb_mutex, 1000) != osOK);
		for(i=0;i<MAX_SHIPS;i++)
		{
			if(ship_addr[i] != 0)
			{
				packet.messageID = SHIP_QMSG;
				packet.senderAddr = my_addr;
				packet.shipAddr = ship_addr[i];
				ship_addr[i] = 0;
				osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
				osMutexRelease(asdb_mutex);
				break;
			}
		}
		if(i >= MAX_SHIPS)
		{
			get_all_ships_data_in_progress = false;//done
			osMutexRelease(asdb_mutex);
		}
	}
}

void welcome_msg_loop(void *args)
{
	queryMsg packet;
	for(;;)
	{
		while(osMutexAcquire(sddb_mutex, 1000) != osOK);
		if(my_data.shipInGame != true)
		{
			packet.messageID = WELCOME_MSG;
			packet.senderAddr = my_address;
			osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
		}
		else ;//TODO delete this thread, cuz no need anymore
		osMutexRelease(sddb_mutex);
		osDelay(10*osKernelGetTickFreq()); //10 seconds
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t nums, i;
	am_addr_t dest;
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	uint8_t * qmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
	queryResponseMsg * packet;
	//TODO maybe put this all in a separate task, to make this interrupt handler faster
	switch(qmsg[0])
	{
		//new ship, maybe do something? trigger reminder to ask for new ship?
		case WELCOME_MSG :
		break;

		//query messages, nothing to do
		case GTIME_QMSG :
		case SHIP_QMSG :
		case AS_QMSG :
		case ACARGO_QMSG :
			break;

		case GTIME_QRMSG :
			packet = (queryResponseMsg *) comms_get_payload(comms, msg, sizeof(queryResponseMsg));
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			global_time_left = packet->departureT;
			osMutexRelease(sddb_mutex);
			break;

		case WELCOME_RMSG :
		case SHIP_QRMSG :
			packet = (queryResponseMsg *) comms_get_payload(comms, msg, sizeof(queryResponseMsg));
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			add_ship(packet);
			osMutexRelease(sddb_mutex);
			while(osMutexAcquire(asdb_mutex, 1000) != osOK);//protects get_all_ships_data_in_progress
			dest = comms_am_get_destination(comms, msg);
			if(get_all_ships_data_in_progress && dest == my_address)osEventFlagsSet(evt_id, 0x00000001U);//trigger get_all_ships_data() thread 
			osMutexRelease(asdb_mutex);
			info1("command rcvd");
			break;

		case AS_QRMSG :
			if(qmsg[2] == my_address)//only if I made quiery
			{
				nums = qmsg[3];//num of ships in this received message
				while(osMutexAcquire(asdb_mutex, 1000) != osOK);
				for(i=0;i<nums;i++)
				{
					add_ship_addr(qmsg[4+i]);
				}
				get_all_ships_data_in_progress = true;
				osMutexRelease(asdb_mutex);
				osEventFlagsSet(evt_id, 0x00000001U);//trigger get_all_ships_data() thread 
			}
			break;

		case ACARGO_QRMSG :
			nums = qmsg[3];//num of ships in this received message
			while(osMutexAcquire(sddb_mutex, 1000) != osOK);
			for(i=0;i<nums;i++)
			{
				mark_cargo(qmsg[4+i]);
			}
			osMutexRelease(sddb_mutex);
			break;

		default:
			break;
	}
	//debug1("rcv-c");
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

void send_msg_loop(void *args)
{
	queryMsg packet;
	for(;;)
	{
		osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
		while(osMutexAcquire(snd_mutex, 1000) != osOK);
		if(!m_sending)
		{
			comms_init_message(sradio, &m_msg);
			queryMsg * qmsg = comms_get_payload(sradio, &m_msg, sizeof(queryMsg));
			if (qmsg == NULL)
			{
				return ;
			}
			qmsg->messageID = packet.messageID;
			qmsg->messageID = packet.senderAddr;
			qmsg->messageID = packet.shipAddr;

			// Send data packet
		    comms_set_packet_type(sradio, &m_msg, AMID_SYSTEMCOMMUNICATION);
		    comms_am_set_destination(sradio, &m_msg, SYSTEM_ID);//TODO resolv system ID value
		    //comms_am_set_source(sradio, &m_msg, radio_address); // No need, it will use the one set with radio_init
		    comms_set_payload_length(sradio, &m_msg, sizeof(queryMsg));

		    comms_error_t result = comms_send(sradio, &m_msg, radio_send_done, NULL);
		    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
		    if (COMMS_SUCCESS == result)
		    {
		        m_sending = true;
		    }
		}
		osMutexRelease(snd_mutex);
	}
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/
loc_bundle_t get_ship_location(am_addr_t ship_addr)
{
	//TODO add functionality to return location of ship with ship_addr address
	loc_bundle_t mloc;
	while(osMutexAcquire(sddb_mutex, 1000) != osOK);
	mloc.x = my_data.x_coordinate;
	mloc.y = my_data.y_coordinate;
	osMutexRelease(sddb_mutex);
	return mloc;
}

static uint8_t get_empty_slot()
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].shipInGame == false)break;
	}
	return k;
}

static uint8_t get_index(am_addr_t addr)
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)
	{
		if(ships[k].shipInGame == true && ships[k].shipAdd == addr)break;
	}
	return k;
}

static void add_ship_addr(am_addr_t addr)
{
	uint8_t i;

	for(i=0;i<MAX_SHIPS;i++)if(ship_addr[i] == addr)break;//check that we don't add double ships

	if(i>=MAX_SHIPS)
	{
		for(i=0;i<MAX_SHIPS;i++)if(ship_addr[i] == 0)
		{
			ship_addr[i] = addr;
			break;
		}
	}
	else ; //this ship is already in database
}

static void mark_cargo(am_addr_t addr)
{
	uint8_t i;
	for(i=0;i<MAX_SHIPS;i++)if(ships[i].shipAdd == addr && ships[i].shipInGame == true)
	{
		ships[i].isCargoLoaded = true;
		break;
	}
}

static void add_ship(queryResponseMsg* ship)
{
	uint8_t ndx;
	queryMsg packet;
	if(ship->shipAddr == my_address)
	{
		my_data.shipInGame = true;
		my_data.shipAdd = ship->shipAddr;
		my_data.departureT = ship->departureT;
		my_data.x_coordinate = ship->x_coordinate;
		my_data.y_coordinate = ship->y_coordinate;
		my_data.isCargoLoaded = ship->isCargoLoaded;
		
		//now ask for a list of all ships in game		
		packet.messageID = AS_QMSG;
		packet.senderAddr = my_address;
		osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	}
	else
	{
		ndx = get_index(ship->shipAddr);
		if(ndx >= MAX_SHIPS)
		{
			ndx = get_empty_slot();
			if(ndx < MAX_SHIPS)
			{
				ships[ndx].shipInGame = true;
				ships[ndx].shipAdd = ship->shipAddr;
				ships[ndx].departureT = ship->departureT;
				ships[ndx].x_coordinate = ship->x_coordinate;
				ships[ndx].y_coordinate = ship->y_coordinate;
				ships[ndx].isCargoLoaded = ship->isCargoLoaded;
			}
			else ; //no room
		}
		else //already got this ship, update only cargo status
		{
			ships[ndx].isCargoLoaded = ship->isCargoLoaded;
		}
	}
}
