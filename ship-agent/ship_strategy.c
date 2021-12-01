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
#define SS_B_MSG_DELAY 2 // Send I-am-branch-msg interval (delay)
#define SS_R_MSG_DELAY 10 // Send root msg interval

typedef enum
{
    SHIP_MSG_ID_NEXT_CMD        = 1,
    SHIP_MSG_ID_NEXT_SHIP       = 2,
    SHIP_BRANCH_MSG_ID          = 3,
    SHIP_ROOT_MSG_ID            = 4
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

typedef struct
{
    am_addr_t ship_addr; // Address of ship
    uint16_t num_bnodes; // Total number of nodes in branches from this ship.
    uint32_t validity_time;
} ship_branches_t;

static uint8_t game_root;

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t my_hood_mutex, b_node_mutex, game_root_mutex;

static comms_layer_t* sradio;
static am_addr_t my_address;
static neighbourhood_t my_hood;
static ship_branches_t ship_branches[MAX_SHIPS];
//static strategy_state_t strategy;

static void ship_strategy_thread(void *args);
static void send_branch_msg (void *args);
static void send_root_msg (void *args);
static void sendMsg(void *args); // Message sending thread

static void find_my_neighb (am_addr_t* n1, am_addr_t* n2); // Returns two nearest neighbours
static uint32_t distance (am_addr_t ship1, am_addr_t ship2); // Calculates distance between two ships
static void rank_neighbourhood (am_addr_t n1, am_addr_t n2);
static void set_root ();
static void add_branch_info(am_addr_t sAddr, uint8_t numBNodes);
static uint8_t calc_bnodes();
static void update_bnode_info();

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
    uint8_t i;
	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, comms_get_payload_max_length(radio), NULL);
	my_hood_mutex = osMutexNew(NULL);	// Protects my_hood variable.
	b_node_mutex = osMutexNew(NULL);	// Protects ship_branches array.
	game_root_mutex = osMutexNew(NULL);	// Protects game_root variable.
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex

	while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
	my_hood.root = 0;
	my_hood.node_1 = 0;
	my_hood.node_2 = 0;
    my_hood.node_3 = 0;
	osMutexRelease(my_hood_mutex);

    while(osMutexAcquire(game_root_mutex, 1000) != osOK);
    game_root = 0;
    osMutexRelease(game_root_mutex);
    
	while(osMutexAcquire(b_node_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		ship_branches[i].ship_addr = 0;
		ship_branches[i].num_bnodes = 0;
		ship_branches[i].validity_time = 0;
	}
	osMutexRelease(b_node_mutex);

	// Default tactics choices
	setXFirst(true);
	setAlwaysPlaceCargo(true);
	setCraneTactics(cc_to_address, my_address, getShipLocation(my_address));
	
	osThreadNew(ship_strategy_thread, NULL, NULL);
	osThreadNew(send_branch_msg, NULL, NULL);
	osThreadNew(send_root_msg, NULL, NULL);
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
    uint8_t *rmsg = (uint8_t *) comms_get_payload(comms, msg, pl_len);
    uint8_t new_root;
    
    branch_msg_t *bpkt;
    root_msg_t *rpkt;
    
    switch(rmsg[0])
    {
        case SHIP_BRANCH_MSG_ID :
            bpkt = (branch_msg_t*)comms_get_payload(comms, msg, sizeof(branch_msg_t));
            info1("Rcvd - branch msg");
            add_branch_info(ntoh16(bpkt->senderAddr), bpkt->num_branch_nodes);
            break;
            
        case SHIP_ROOT_MSG_ID :
            rpkt = (root_msg_t*)comms_get_payload(comms, msg, sizeof(root_msg_t));
            info1("Rcvd - root msg");
            
            if (*(my_hood.root) == my_address)
            {
                new_root = rpkt->num_root_nodes;
                
                while(osMutexAcquire(game_root_mutex, 1000) != osOK);
                
                if (new_root > game_root)
                {
                    game_root = new_root;
                }
                else if (new_root == game_root)
                {
                    // who is closer to crane
                }
                else ; // Root has less ships than game_root
                
                osMutexRelease(game_root_mutex);
            }
            else ; // I am not root, do nothing
            
            
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
    static comms_msg_t m_msg;
    branch_msg_t *bmsg, *bpkt;
    root_msg_t *rmsg, *rpkt;
    uint8_t packet[comms_get_payload_max_length(sradio)];
    
    for(;;)
    {
        osMessageQueueGet(snd_msg_qID, &packet, NULL, osWaitForever);
        osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

        comms_init_message(sradio, &m_msg);		
        
        switch(packet[0])
        {
            case SHIP_BRANCH_MSG_ID :
            
            bmsg = (branch_msg_t *)comms_get_payload(sradio, &m_msg, sizeof(branch_msg_t));
            if (bmsg == NULL)
            {
                continue ;// Continue for(;;) loop
            }
            bpkt = (branch_msg_t*)packet;
            bmsg->messageID = bpkt->messageID;
            bmsg->num_branch_nodes = bpkt->num_branch_nodes;
            bmsg->senderAddr = hton16(my_address);

            comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
            comms_am_set_destination(sradio, &m_msg, bpkt->senderAddr); // Setting destination address
            comms_set_payload_length(sradio, &m_msg, sizeof(branch_msg_t));
            break;
            
            case SHIP_ROOT_MSG_ID :
            
            bmsg = (root_msg_t *)comms_get_payload(sradio, &m_msg, sizeof(root_msg_t));
            if (bmsg == NULL)
            {
                continue ; // Continue for(;;) loop
            }
            
            rpkt = (root_msg_t*)packet;
            rmsg->messageID = rpkt->messageID;
            rmsg->num_root_nodes = rpkt->num_root_nodes;
            rmsg->senderAddr = hton16(my_address);

            comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
            comms_am_set_destination(sradio, &m_msg, rpkt->senderAddr); // Setting destination address
            comms_set_payload_length(sradio, &m_msg, sizeof(root_msg_t));
            break;
            
            default :
                break ;
        }

        // Send data packet
        comms_error_t result = comms_send(sradio, &m_msg, radioSendDone, NULL);
        logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
    }
}

static void send_branch_msg (void *args)
{
    am_addr_t dest_address;
    branch_msg_t packet;
    uint8_t numb;
    
    for(;;)
    {
        osDelay(SS_B_MSG_DELAY*osKernelGetTickFreq());
        
        while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
        if (*my_hood.root == my_address)
        {
            // Pole vaja saata sõnumit
        }
        else
        {
            
            if (my_hood.node_1 == my_address) dest_address = 0; // Pole vaja midagi teha
            else 
            {
                if (my_hood.node_2 == my_address) dest_address = my_hood.node_1;
                else
                {
                    if (my_hood.node_3 == my_address) dest_address = my_hood.node_2;
                    else ; // Siia ei tohiks sattuda kunagi.
                }
            }
        
            osMutexRelease(my_hood_mutex);

            if (0 != dest_address)
            {
	            packet.messageID = SHIP_BRANCH_MSG_ID;
	            packet.senderAddr = dest_address; // Piggybacking destination address here
                update_bnode_info();
                numb = calc_bnodes();
	            packet.num_branch_nodes = 1 + numb;
	            
	            osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
	            info1("Send b msg");
	        }
	    }
    }
}

static void send_root_msg (void *args)
{
    root_msg_t packet;
    uint8_t numb;
    
    for(;;)
    {
        osDelay(SS_R_MSG_DELAY*osKernelGetTickFreq());
        
        while(osMutexAcquire(my_hood_mutex, 1000) != osOK); // Protects my_hood variable.
        
        if (*(my_hood.root) == my_address)
        {
            osMutexRelease(my_hood_mutex);
            
            packet.messageID = SHIP_ROOT_MSG_ID;
	        packet.senderAddr = AM_BROADCAST_ADDR; // Piggybacking destination address here
            update_bnode_info();
            numb = calc_bnodes();
            packet.num_root_nodes = 1 + numb;
            
            osMessageQueuePut(snd_msg_qID, &packet, 0, osWaitForever);
            info1("Send r msg");
        }
        else ; // I am not root, do nothing
        
        osMutexRelease(my_hood_mutex);
    }
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

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

static void add_branch_info(am_addr_t sAddr, uint8_t numBNodes)
{
    uint8_t k;
    bool new_ship = false;
    
	for(k=0;k<MAX_SHIPS;k++)
	{
        if(ship_branches[k].ship_addr == sAddr) // Update for known ship.
        {
            ship_branches[k].num_bnodes = numBNodes;
            ship_branches[k].validity_time = osKernelGetTickCount() + (SS_B_MSG_DELAY * 10 * osKernelGetTickFreq());
            break; 
        }
        else if (ship_branches[k].ship_addr == 0) // End of list, break for() loop
        {
            new_ship = true;
            break; 
        }
        else ; // Continue searching
    }
    
    if(new_ship) // Add new branch-ship
    {
        ship_branches[k].ship_addr = sAddr;
        ship_branches[k].num_bnodes = numBNodes;
        ship_branches[k].validity_time = osKernelGetTickCount() + (SS_B_MSG_DELAY * 10 * osKernelGetTickFreq());
    }
}

static void update_bnode_info()
{
    uint8_t i;
    uint32_t cTime = osKernelGetTickCount();
    
    while(osMutexAcquire(b_node_mutex, 1000) != osOK);
    for(i=0;i<MAX_SHIPS;i++)
    {
	    if(ship_branches[i].validity_time < cTime)
	    {
	        ship_branches[i].ship_addr = 0;
	        ship_branches[i].num_bnodes = 0;
	        ship_branches[i].validity_time = 0;
	    }
    }
	osMutexRelease(b_node_mutex);
}

static uint8_t calc_bnodes()
{
    uint8_t i;
    uint16_t numb = 0;
    
    while(osMutexAcquire(b_node_mutex, 1000) != osOK);
    for(i=0;i<MAX_SHIPS;i++)
    {
        if(ship_branches[i].ship_addr != 0)numb += ship_branches[i].num_bnodes;
    }
	osMutexRelease(b_node_mutex);
    
    return numb;
}


