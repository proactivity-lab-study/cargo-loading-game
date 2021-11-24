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
    SHIP_MSG_ID_NEXT_CMD        = 1,
    SHIP_MSG_ID_NEXT_SHIP       = 2
} ship_msg_id_t;

typedef enum
{
    STRATS_I_AM_ROOT 	        = 1,
    STRATS_FOLLOW_NEIGHBOUR     = 2,
    STRATS_HOOD_DONE            = 3
} strategy_state_t;

typedef struct
{
    am_addr_t* root; // Root node of neighbourhood.
    am_addr_t node_1;
    am_addr_t node_2;
    am_addr_t node_3;
} neighbourhood_t;

uint16_t num_bnodes; // Total number of nodes in branches from me.
static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t my_hood_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static neighbourhood_t my_hood;
//static strategy_state_t strategy;

static void ship_strategy_thread(void *args);
static void sendMsg(void *args); // Message sending thread

static void sendNextCommandMsg(crane_command_t cmd, am_addr_t dest); // Send 'next command' message
static void sendNextShipMsg(am_addr_t next_ship_addr, am_addr_t dest); // Send 'next ship' message
static void find_my_neighb (am_addr_t* n1, am_addr_t* n2); // Returns two nearest neighbours
static uint32_t distance (am_addr_t ship1, am_addr_t ship2); // Calculates distance between two ships
static void rank_neighbourhood (am_addr_t n1, am_addr_t n2);
static void set_root ();

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, comms_get_payload_max_length(radio), NULL);
	my_hood_mutex = osMutexNew(NULL);	// Protects my_hood variable.
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
	my_hood.root = 0;
	my_hood.node_1 = 0;
	my_hood.node_2 = 0;
	num_bnodes = 0;
	osMutexRelease(my_hood_mutex);

	// Default tactics choices
	setXFirst(true);
	setAlwaysPlaceCargo(true);
	setCraneTactics(cc_to_address, my_address, getShipLocation(my_address));
	
	osThreadNew(ship_strategy_thread, NULL, NULL);
	snd_task_id = osThreadNew(sendMsg, NULL, NULL); // Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

/**
 * Check for new neighbours or changes in neighbourhood.
 * Change strategy if necessary.
 */
static void ship_strategy_thread(void *args)
{
    am_addr_t n1, n2, n3, *cur_root;

    for(;;)
    {
        osDelay(SS_DEFAULT_DELAY*osKernelGetTickFreq());

        find_my_neighb(&n1, &n2); // Periodic check for new neighbours or changes in neighbourhood.

        if (n1 == 0 && n2 == 0) // No neighbours
        {

            while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
            my_hood.node_1 = my_address;
            my_hood.node_2 = 0;
            my_hood.node_3 = 0;
            my_hood.root = &(my_hood.node_1);
            num_bnodes = 0;
            osMutexRelease(my_hood_mutex);
        }
        else // At least one neighbour
        {

            rank_neighbourhood(n1, n2);
        }

        set_root();

        while(osMutexAcquire(my_hood_mutex, 1000) != osOK);
        n1 = my_hood.node_1;
        n2 = my_hood.node_2;
        n3 = my_hood.node_3;
        cur_root = my_hood.root;
        osMutexRelease(my_hood_mutex);

        if (my_address == *cur_root)info4("I-AM-gROOT");
        info4("%u: %u %u %u", my_address, n1, n2, n3);
        
        if (NULL != cur_root)
        {
            setXFirst(true);
            setAlwaysPlaceCargo(true);
            if (my_address == *cur_root)
            {
                setCraneTactics(cc_to_address, *cur_root, getShipLocation(*cur_root));
            }
            else setCraneTactics(cc_parrot_ship, *cur_root, getShipLocation(*cur_root));
        }
        else 
        {
            // TODO Exit game.
            setXFirst(true);
            setAlwaysPlaceCargo(true);
            setCraneTactics(cc_do_nothing, 0, getShipLocation(0));
            info4("Done");
        }
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

static void find_my_neighb (am_addr_t* n1, am_addr_t* n2)
{
    am_addr_t saddr[MAX_SHIPS], ship1, ship2;
    uint8_t num_ships, i;
    uint32_t dist1, dist2, dist;
    
    num_ships = getAllShipsAddr(saddr, MAX_SHIPS);
    //info4("nums %u", num_ships);
    if (num_ships < 2) // Less than two neighbours
    {
        if (num_ships == 1)
        {
            ship1 = saddr[0];
            ship2 = 0;
        }
        else
        {
            ship1 = 0;
            ship2 = 0;
        }
    }
    else // At least two neighbours exist
    {
        ship1 = saddr[0];
        dist1 = distance(ship1, my_address);
        
        ship2 = saddr[1];
        dist2 = distance(ship2, my_address);
        
        if(dist1 <= dist2)
        {
            ship1 = saddr[0];
            ship2 = saddr[1];
        }
        else 
        {
            ship1 = saddr[1];
            ship2 = saddr[0];
            dist = dist1;
            dist1 = dist2;
            dist2 = dist;
        }

        for (i = 2; i < num_ships; i++)
        {
            dist = distance(saddr[i], my_address);
            
            if (dist <= dist1)
            {
                dist2 = dist1;
                dist1 = dist;
                ship2 = ship1;
                ship1 = saddr[i];
            }
            else if (dist < dist2) // Must not use <= here!
            {
                dist2 = dist;
                ship2 = saddr[i];
            }
            else ; // Distance to ship i is greater than that of ship1 and ship2
        }
    }
    *n1 = ship1;
    *n2 = ship2;
}

static uint32_t distance (am_addr_t ship1, am_addr_t ship2)
{
    loc_bundle_t dist1, dist2;
    
    dist1 = getShipLocation(ship1);
    dist2 = getShipLocation(ship2);
    
    return abs(dist1.x - dist2.x) + abs(dist1.y - dist2.y);
}

static void rank_neighbourhood(am_addr_t n1, am_addr_t n2)
{
    uint32_t dc_my, dc_s1, dc_s2;
	
	dc_my = distToCrane(getShipLocation(my_address));
	
	if (n1 != 0) dc_s1 = distToCrane(getShipLocation(n1));
	else dc_s1 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1; // Impossibly long distance.
	
	if (n2 != 0) dc_s2 = distToCrane(getShipLocation(n2));
	else dc_s2 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1; // Impossibly long distance.
	
	while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
	if (dc_my < dc_s1)
	{
	    if (dc_my < dc_s2) // me < s1, s2
	    {
	        my_hood.node_1 = my_address;
	        if (n1 != 0) dc_s1 = distance(my_hood.node_1, n1);
	        else dc_s1 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1;
	        if (n2 != 0) dc_s2 = distance(my_hood.node_1, n2);
	        else dc_s2 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1;
	        
	        if (dc_s1 < dc_s2)
	        {
	            my_hood.node_2 = n1;
	            my_hood.node_3 = n2;
	        }
	        else
	        {
	            my_hood.node_2 = n2;
	            my_hood.node_3 = n1;
	        }
	    }
	    else // s2 <= me < s1
	    {
	        my_hood.node_1 = n2;
	        dc_s1 = distance(my_hood.node_1, my_address);
	        if (n1 != 0) dc_s2 = distance(my_hood.node_1, n1);
	        else dc_s2 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1;
	        
	        if (dc_s1 < dc_s2)
	        {
	            my_hood.node_2 = my_address;
	            my_hood.node_3 = n1;
	        }
	        else
	        {
	            my_hood.node_2 = n1;
	            my_hood.node_3 = my_address;
	        }
	    }
	}
	else // s1 <= me
	{
	    if (dc_s1 < dc_s2) // s1 <= me < s2
	    {
	        my_hood.node_1 = n1;
	        dc_s1 = distance(my_hood.node_1, my_address);
	        if (n2 != 0) dc_s2 = distance(my_hood.node_1, n2);
	        else dc_s2 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1;
	        
	        if (dc_s1 < dc_s2)
	        {
	            my_hood.node_2 = my_address;
	            my_hood.node_3 = n2;
	        }
	        else
	        {
	            my_hood.node_2 = n2;
	            my_hood.node_3 = my_address;
	        }
	    }
	    else // s2 < s1 < me
	    {
	        my_hood.node_1 = n2;
	        dc_s1 = distance(my_hood.node_1, my_address);
	        if (n1 != 0) dc_s2 = distance(my_hood.node_1, n1);
	        else dc_s2 = GRID_UPPER_BOUND + GRID_UPPER_BOUND + 1;
	        
	        if (dc_s1 < dc_s2)
	        {
	            my_hood.node_2 = my_address;
	            my_hood.node_3 = n1;
	        }
	        else
	        {
	            my_hood.node_2 = n1;
	            my_hood.node_3 = my_address;
	        }
	    }
	}
	osMutexRelease(my_hood_mutex);
}

static void set_root()
{
    cargo_status_t cstat;
    
    while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
    
    cstat = getCargoStatus(my_hood.node_1);
    if (cs_cargo_not_received == cstat) my_hood.root = &(my_hood.node_1);
    else
    {
        if (cs_cargo_received == cstat)
        {
            cstat = getCargoStatus(my_hood.node_2);
            if (cs_cargo_not_received == cstat) my_hood.root = &(my_hood.node_2);
            else
            {
                if (cs_cargo_received == cstat)
                {
                    cstat = getCargoStatus(my_hood.node_3);
                    if (cs_cargo_not_received == cstat) my_hood.root = &(my_hood.node_3);
                    else
                    {
                        if (cs_cargo_received == cstat)
                        {
                            // TODO Neighbourhood done.
                            my_hood.root = NULL;
                        }
                        else ; // TODO Unknown ship
                    }
                }
                else ; // TODO Unknown ship
            }
        }
        else ; // TODO Unknown ship
    }
    osMutexRelease(my_hood_mutex);
}
