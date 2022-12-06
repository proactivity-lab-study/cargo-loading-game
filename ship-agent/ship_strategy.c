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
#include "math.h"

#include "loglevels.h"
#define __MODUUL__ "sstrt"
#define __LOG_LEVEL__ (LOG_LEVEL_ship_strategy & BASE_LOG_LEVEL)
#include "log.h"

#define SS_DEFAULT_DELAY 2 // A delay, seconds

static uint8_t cv_probability[5];

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t sfgl_mutex, cmsg_1_mutex, cmsg_2_mutex, prob_array_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static uint32_t fglobal_val; // Read-write by multiple threads, needs mutex protection

static void ben_or_protocol(void *args); // A thread function
static void sendMsg(void *args); // Message sending thread

static uint8_t cmsg_1_val[MAX_SHIPS];
static uint8_t cmsg_2_val[MAX_SHIPS]; // These two ought to be in a struct.
static bool cmsg_2_decision[MAX_SHIPS]; // These two ought to be in a struct.
static uint16_t current_cmsg_1_round = 1;
static uint16_t current_cmsg_2_round = 1;

static crane_command_t get_consensus_value();
static void clear_cons_msg_one();
static uint16_t add_cons_msg_one(uint8_t val, uint16_t round);
static void clear_cons_msg_two();
static uint16_t add_cons_msg_two(uint8_t val, uint16_t round, bool decision);
static crane_command_t get_consensus_value();
static uint8_t get_msgone_mode(uint32_t *cons_val_mode);
static uint8_t get_msgtwo_mode(uint32_t *cons_val_mode);
static crane_command_t get_d_val_single();

static osStatus_t sendConsensusMsgTWO (crane_command_t value_proposal, uint16_t round_num, bool decision);
static osStatus_t sendConsensusMsgONE (crane_command_t value_proposal, uint16_t round_num);
static void sendNextCommandMsg(crane_command_t cmd, am_addr_t dest); // Send 'next command' message
static void sendNextShipMsg(am_addr_t next_ship_addr, am_addr_t dest); // Send 'next ship' message
static uint32_t randomNumber(uint32_t rndL, uint32_t rndH);

static osThreadId_t ben_or_thread_id;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initShipStrategy(comms_layer_t* radio, am_addr_t addr)
{
	snd_msg_qID = osMessageQueueNew(MAX_SHIPS + 3, comms_get_payload_max_length(radio), NULL);
	sfgl_mutex = osMutexNew(NULL);	// Protects fglobal_val
	cmsg_1_mutex = osMutexNew(NULL);	// Protects database of consensus msg one type messages
	cmsg_2_mutex = osMutexNew(NULL);	// Protects database of consensus msg one type messages
	prob_array_mutex = osMutexNew(NULL);
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex
	srand(osKernelGetTickCount()); // Initialise random number generator
    
    info4("Ship strategy init");
    while(osMutexAcquire(prob_array_mutex, 1000) != osOK); // Protects fglobal_val
    cv_probability[0] = 0;
    cv_probability[1] = 0;
    cv_probability[2] = 0;
    cv_probability[3] = 0;
    cv_probability[4] = 0;
    osMutexRelease(prob_array_mutex);
    
    clear_cons_msg_one();
    clear_cons_msg_two();
    current_cmsg_1_round = 1;
    current_cmsg_2_round = 1;
    
	while(osMutexAcquire(sfgl_mutex, 1000) != osOK); // Protects fglobal_val
	fglobal_val = 0;
	osMutexRelease(sfgl_mutex);

	// Default tactics choices
	setXFirst(true);
	setAlwaysPlaceCargo(true);
	setCraneTactics(cc_to_address, my_address, getShipLocation(my_address));
	
	ben_or_thread_id = osThreadNew(ben_or_protocol, NULL, NULL); 			// Template thread
	snd_task_id = osThreadNew(sendMsg, NULL, NULL); 	// Sends messages
	osThreadFlagsSet(snd_task_id, 0x00000001U); // Sets thread to ready-to-send state
}

/**********************************************************************************************
 *	 Module threads
 *********************************************************************************************/

static void ben_or_protocol(void *args)
{
    #define MSG_ONE_WAIT_TIME 0.5F // Wait time in seconds
    #define MSG_TWO_WAIT_TIME 0.5F // Wait time in seconds
    
    crane_command_t my_proposal, consensus_value = CM_LEFT;
    uint16_t round_cnt = 0, send_msg_one_count = 0, send_msg_two_count = 0;
    bool no_consensus = true, msg_one_OK = false, msg_two_OK = false;
    osStatus_t send_err;
    loc_bundle_t loc = {0,0};
    uint32_t mode_val, d_mode_val;
    uint32_t mode_val_cnt, d_val_cnt;
    uint32_t needed_num, flags;
    am_addr_t saddr[MAX_SHIPS]; // Because game_status has no function that only returns number of ships
    
    //If not already, set crane tactics to cc_only_consensus
    setCraneTactics(cc_only_consensus, AM_BROADCAST_ADDR, loc);
    
	for(;;)
	{

		// Wait for new crane round.
	    // TODO I'm interested would the Ben Or protocol work without this sync. Are round counts necessary at all in our case???
		osThreadFlagsWait(BEN_OR_WFLAGS_CRANE, osFlagsWaitAll, osWaitForever); // osWaitForever, because without sync, no point in blindly sending messages
		
		// Get new consensus value
	    my_proposal = get_consensus_value();
		while (no_consensus)
		{
		    round_cnt++; // Increment current round number.
		    
		    // Send consensus message ONE.
		    send_err = sendConsensusMsgONE(my_proposal, round_cnt);
		    if(osOK != send_err)break; // Bail out and wait for new crane round.

		    msg_one_OK = false;
		    while(!msg_one_OK)
		    {
	            // Wait until received N*2/3 message ONE or send msgONE again if 500 ms have passed.
	            info4("Waiting for msg 1 - %lu", round_cnt);
	            flags = osThreadFlagsWait(BEN_OR_WFLAGS_MSGONE, osFlagsWaitAll, (uint32_t) (MSG_ONE_WAIT_TIME * osKernelGetTickFreq()));
		        info4("a %u", flags);
		        if(flags == osFlagsErrorTimeout)
		        {
		            // Send msg ONE again
		            if(send_msg_one_count < 1)
		            {
		                // Send consensus message ONE.
		                send_err = sendConsensusMsgONE(my_proposal, round_cnt);
		                if(osOK != send_err)
		                {   
		                    send_msg_one_count = 0;
		                    break; // Bail out and wait for new crane round
		                }
		                else send_msg_one_count++;
		            }
		            else
		            {
		                send_msg_one_count = 0;
		                send_err = osErrorResource; // Hack to get out of second while(no_consensus) loop.
		                break; // Bail out and wait for new crane round
		            }
		        }
		        else
		        {
        	        // Check for consensus (at least N/2 of the same value)
		            mode_val_cnt = get_msgone_mode(&mode_val);
		            needed_num = getAllShipsAddr(saddr, MAX_SHIPS);
		            needed_num = (uint8_t) ceil((float) needed_num / 2);
		            
		            if(mode_val_cnt >= needed_num)no_consensus = false;

		            if(!no_consensus)my_proposal = mode_val;
		            else my_proposal = CM_NO_COMMAND; // This actually won't matter according to the consensus protocol.
		            info4("about to send msg 2");
		            // Send consensus message TWO (either D(true) or ?(false).
		            send_err = sendConsensusMsgTWO(my_proposal, round_cnt, !no_consensus);
        		    if(osOK != send_err)break; // Bail out and wait for new crane round.
        		    else msg_one_OK = true;
        		}
        	}
    		if(osOK != send_err)break; // Bail out and wait for new crane round.
    		
    		msg_two_OK = false;
		    while(!msg_two_OK)
		    {
		        info4("Waiting for msg 2 - %u", no_consensus);
		        // Wait until received N*2/3 message TWO
		        flags = osThreadFlagsWait(BEN_OR_WFLAGS_MSGTWO, osFlagsWaitAll, (uint32_t) (MSG_TWO_WAIT_TIME * osKernelGetTickFreq()));
		        
		        if(flags == osFlagsErrorTimeout)
		        {
		            // Send msg TWO again
		            if(send_msg_two_count < 1)
		            {
		                // Send consensus message TWO.
		                send_err = sendConsensusMsgTWO(my_proposal, round_cnt, !no_consensus);
		                if(osOK != send_err)
		                {   
		                    send_msg_two_count = 0;
		                    break; // Bail out and wait for new crane round
		                }
		                else send_msg_two_count++;
		            }
		            else
		            {
		                send_msg_two_count = 0;
		                send_err = osErrorResource; // Hack to get out of second while(no_consensus) loop.
		                break; // Bail out and wait for new crane round
		            }
		        }
		        else
		        {
		            // Check for consensus (at least N/2 of D message)
		            d_val_cnt = get_msgtwo_mode(&d_mode_val);
		            needed_num = getAllShipsAddr(saddr, MAX_SHIPS);
		            needed_num = (uint8_t) ceil((float) needed_num / 2);
		            
		            if(d_val_cnt >= needed_num)
		            {
		                send_consensus_command(consensus_value);
		                no_consensus = false;
		                info4("consensus %u", consensus_value);
		            }
		            else if(d_val_cnt == 1) // TODO what if 1 < d < needed_num???
                    {
                        // If at least one D message change consensus value to value from D message
                        my_proposal = get_d_val_single();
                        info4("allmost consensus");
		            }
		            else //No consensus and no one with D value
		            {
		                // Get new consensus value.
		                my_proposal = get_consensus_value();
		                info4("no consensus");
		            }
		            msg_two_OK = true;
		        }
	        }
	        // Clear consensus message database after each round!
	        clear_cons_msg_one();
	        clear_cons_msg_two();
	    }
	    round_cnt = 0;
	    no_consensus = true;
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
    uint8_t cmd, needed_ships;
    ship_next_cmd_msg_t *cpkt;
    ship_next_ship_msg_t *spkt;
    cons_msg_t *cons_msg;
    uint16_t msg_count;
    am_addr_t saddr[MAX_SHIPS]; // Because game_status has no function that only returns number of ships

    switch(rmsg[0])
    {
        case CONSENSUS_MSG_ONE :
         
            cons_msg = (cons_msg_t*)comms_get_payload(comms, msg, sizeof(cons_msg_t));
            info4("RecMsg1 %u %u", cons_msg->cons_value, ntoh16(cons_msg->senderAddr));
            // Place data to database.
            msg_count = add_cons_msg_one(cons_msg->cons_value, ntoh16(cons_msg->round_num));

            // Check if wait condition is fulfilled.
            needed_ships = getAllShipsAddr(saddr, MAX_SHIPS);
            info4("needed 1 %u", needed_ships);
            needed_ships = ceil((float)((needed_ships - 1) * 2) / 3);
            info4("needed 2 %u ? %u", needed_ships, msg_count);
            if(msg_count >= needed_ships)
            {
                // Wake up consensus algorithm thread.
                osThreadFlagsSet(ben_or_thread_id, BEN_OR_WFLAGS_MSGONE);
            }
            
            break;
            
        case CONSENSUS_MSG_TWO :
         
            cons_msg = (cons_msg_t*)comms_get_payload(comms, msg, sizeof(cons_msg_t));
            info4("RecMsg2 %u %u", cons_msg->cons_value, ntoh16(cons_msg->senderAddr));
            // Place data to database.
            msg_count = add_cons_msg_two(cons_msg->cons_value, ntoh16(cons_msg->round_num), cons_msg->decision);

            // Check if wait condition is fulfilled.
            needed_ships = getAllShipsAddr(saddr, MAX_SHIPS);
            needed_ships = ceil((float)((needed_ships-1) * 2) / 3);
            
            if(msg_count >= needed_ships)
            {
                // Wake up consensus algorithm thread.
                osThreadFlagsSet(ben_or_thread_id, BEN_OR_WFLAGS_MSGTWO);
            }
        
            break;
        
        // Next command message
        case SHIP_MSG_ID_NEXT_CMD :
            cpkt = (ship_next_cmd_msg_t*)comms_get_payload(comms, msg, sizeof(ship_next_cmd_msg_t));
            info1("Rcvd - nxt cmd msg");
            sender = ntoh16(cpkt->senderAddr);
            cmd = cpkt->cmd;

            // Do something with this info.
            info1("Stop compiler for giving warning %u %u", sender, cmd);
            
            break;
            
        // Next ship message
        case SHIP_MSG_ID_NEXT_SHIP :
            spkt = (ship_next_ship_msg_t*)comms_get_payload(comms, msg, sizeof(ship_next_ship_msg_t));
            info1("Rcvd - nxt ship msg");
            sender = ntoh16(spkt->senderAddr);
            ship = ntoh16(spkt->nextShip);
            
            // Do something with this info.
            info1("Stop compiler for giving warning %u %u", sender, ship);
            
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
    cons_msg_t *cons_msg, *cons_pkt;
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
            // Ben-Or consensus algorithm message one and two get handled the same way.
            case CONSENSUS_MSG_ONE :
            case CONSENSUS_MSG_TWO :
            
            cons_msg = (cons_msg_t *)comms_get_payload(sradio, &m_msg, sizeof(cons_msg_t));
            if (cons_msg == NULL)
            {
                continue ;// Continue for(;;) loop
            }

            cons_pkt = (cons_msg_t*)packet;
            cons_msg->messageID = cons_pkt->messageID;
            info4("Sending msg %u", cons_pkt->messageID);
            cons_msg->senderAddr = hton16(my_address);
            cons_msg->cons_value = cons_pkt->cons_value;
            cons_msg->round_num = hton16(cons_pkt->round_num);
            cons_msg->decision = cons_pkt->decision;

            comms_set_packet_type(sradio, &m_msg, AMID_SHIPCOMMUNICATION);
            comms_am_set_destination(sradio, &m_msg, cons_pkt->senderAddr); // Setting destination address
            comms_set_payload_length(sradio, &m_msg, sizeof(cons_msg_t));
            break;
            
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
static void clear_cons_msg_one()
{
    uint8_t i = 0;
    
    while(osMutexAcquire(cmsg_1_mutex, 1000) != osOK);
	
    for(i = 0;i < MAX_SHIPS;i++)
    {
        cmsg_1_val[i] = 0;
    }
    
	osMutexRelease(cmsg_1_mutex);
}

static uint16_t add_cons_msg_one(uint8_t val, uint16_t round)
{
    uint16_t i = MAX_SHIPS;

    while(osMutexAcquire(cmsg_1_mutex, 1000) != osOK);

    if(round == current_cmsg_1_round)
    {
        for(i = 0;i < MAX_SHIPS;i++)
        {
            if(cmsg_1_val[i] == 0)
            {
                cmsg_1_val[i] = val;
                break;
            }
        }
        // No overflow detection i==MAX_SHIPS!
    }
    else info4("Wrong round msg.");

    osMutexRelease(cmsg_1_mutex);

    return i+1;
}

static void clear_cons_msg_two()
{
    uint8_t i = 0;
    
    while(osMutexAcquire(cmsg_2_mutex, 1000) != osOK);
	
    for(i = 0;i < MAX_SHIPS;i++)
    {
        cmsg_2_decision[i] = 0;
        cmsg_2_val[i] = 0;
    }
    
	osMutexRelease(cmsg_2_mutex);
}

static uint16_t add_cons_msg_two(uint8_t val, uint16_t round, bool decision)
{
    uint16_t i = MAX_SHIPS;

    while(osMutexAcquire(cmsg_2_mutex, 1000) != osOK);

    if(round == current_cmsg_2_round)
    {
        for(i = 0;i < MAX_SHIPS;i++)
        {
            if(cmsg_2_val[i] == 0)
            {
                cmsg_2_val[i] = val;
                cmsg_2_decision[i] = decision;
                break;
            }
        }
        // No overflow detection i==MAX_SHIPS!
    }
    else info1("Wrong round msg.");
    
    osMutexRelease(cmsg_2_mutex);

    return i+1;
}


static osStatus_t sendConsensusMsgONE (crane_command_t value_proposal, uint16_t round_num)
{
	cons_msg_t packet;
	uint32_t wait_for_send;
	
	packet.messageID = CONSENSUS_MSG_ONE;
	packet.senderAddr = AM_BROADCAST_ADDR; // Consensus messages are broadcast messages
	packet.cons_value = value_proposal;     // Proposed consensus value (crane commands 1 - 5)
    packet.round_num = round_num;     // Round number
    packet.decision = false; // Not used with msg ONE.
	
	wait_for_send = (uint32_t)(0.5 * osKernelGetTickFreq()); // Wait half a second
	return osMessageQueuePut(snd_msg_qID, &packet, 0, wait_for_send);
}

static osStatus_t sendConsensusMsgTWO (crane_command_t value_proposal, uint16_t round_num, bool decision)
{
	cons_msg_t packet;
	uint32_t wait_for_send;
	
	packet.messageID = CONSENSUS_MSG_TWO;
	packet.senderAddr = AM_BROADCAST_ADDR; // Consensus messages are broadcast messages
	packet.cons_value = value_proposal;     // Proposed consensus value (crane commands 1 - 5)
    packet.round_num = round_num;     // Round number
    packet.decision = decision; // True (D) if decision was made, false (?) if not
	
	wait_for_send = (uint32_t)(0.5 * osKernelGetTickFreq()); // Wait half a second
	return osMessageQueuePut(snd_msg_qID, &packet, 0, wait_for_send);
}

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

static crane_command_t get_consensus_value()
{
    crane_command_t val = CM_NO_COMMAND;
    uint8_t random;
    uint8_t step_val;
    
    // get random number
    random = randomNumber(0,100);
    
    // get consensus value based on random number
    while(osMutexAcquire(prob_array_mutex, 1000) != osOK);
    step_val = cv_probability[0];
    for(int i = 1; i <= 5;i++)
    {
    	if(step_val > random)
    	{
    	    val = i;
    	    break;
    	}
    	else if(i != 5)
    	{
    	    step_val += cv_probability[i];
    	}
    	else ; // Shouldn't get here. val = CM_NO_COMMAND
	}
	osMutexRelease(prob_array_mutex);

    return val;
}


static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}

void prob_towards_ship(loc_bundle_t ship_loc, loc_bundle_t crane_loc, bool prioritise_cargo, bool vertical_first)
{
    uint8_t vertical, horizontal;
    
    // Which two commands are right
    
    while(osMutexAcquire(prob_array_mutex, 1000) != osOK);
    if(ship_loc.x == crane_loc.x && ship_loc.y == crane_loc.y)
    {
        if(prioritise_cargo)
        {
            cv_probability[CM_PLACE_CARGO-1] = 92;
            cv_probability[CM_UP-1] = 2;
            cv_probability[CM_DOWN-1] = 2;
            cv_probability[CM_LEFT-1] = 2;
            cv_probability[CM_RIGHT-1] = 2;
            
            osMutexRelease(prob_array_mutex);
            return;
        }
    }
    else
    {
        if(ship_loc.x > crane_loc.x)horizontal = CM_RIGHT;
        // TODO else if(ship_loc.x == crane_loc.x)horizontal = CM_NO_COMMAND;
        else horizontal = CM_LEFT;
    }
        
        if(ship_loc.y > crane_loc.y)vertical = CM_UP;
        // TODO else if(ship_loc.y == crane_loc.y)horizontal = CM_NO_COMMAND;
        else vertical = CM_DOWN;
    
    if(vertical_first)
    {
        cv_probability[vertical-1] = 50;
        cv_probability[horizontal-1] = 30;
    }
    else
    {
        cv_probability[vertical-1] = 30;
        cv_probability[horizontal-1] = 50;
    }
    
    // The rest.
    if(vertical == CM_UP)cv_probability[vertical] = 5;
    else cv_probability[vertical-2] = 5;
    if(vertical == CM_LEFT)cv_probability[horizontal] = 5;
    else cv_probability[horizontal-2] = 5;
    
    cv_probability[CM_PLACE_CARGO-1] = 10;
    osMutexRelease(prob_array_mutex);
    
    return;
}

static uint32_t distance (loc_bundle_t dist1, loc_bundle_t dist2)
{    
    return abs(dist1.x - dist2.x) + abs(dist1.y - dist2.y);
}

static am_addr_t closest_to_crane()
{
    loc_bundle_t crane_loc, ship_loc;
    uint8_t num_ships, i;
    uint16_t dist, min_dist;
    am_addr_t saddr[MAX_SHIPS], closest_ship;
    
    // Get crane location
    crane_loc = getCraneLoc();
    
    // Get all ships locations
    num_ships = getAllShipsAddr(saddr, MAX_SHIPS);
    
    // Find closest ship to crane (in case of tie, selects first one found) (ignore ships with cargo)
    if(getCargoStatus(saddr[0]) != cs_cargo_received)
    {
        ship_loc = getShipLocation(saddr[0]);
        min_dist = distance (ship_loc, crane_loc);
        closest_ship = saddr[0];
    }
    for(i=1;i<num_ships;i++)
    {
        if(getCargoStatus(saddr[i]) != cs_cargo_received)
        {
            ship_loc = getShipLocation(saddr[i]);
            dist = distance (ship_loc, crane_loc);
            if(dist < min_dist)
            {
                min_dist = dist;
                closest_ship = saddr[i];
            }
        }
    }
    return closest_ship;
}

void notifyNewCraneRound()
{
    loc_bundle_t ship_loc, crane_loc;
    bool prioritise_cargo, vertical_first;
    am_addr_t closest;
    // Initiate calculation of new consensus probabilities
    
    // TODO Reconsider strategy
    
    // Select strategy nearest to crane.
    // Find closest to crane without cargo.
    closest = closest_to_crane();
    ship_loc = getShipLocation(closest);
    crane_loc = getCraneLoc();
    prioritise_cargo = vertical_first = true;
    
    // Generate new probability values based on closest ship strategy
    prob_towards_ship(ship_loc, crane_loc, prioritise_cargo, vertical_first);

    // Start new consensus finding round.
    osThreadFlagsSet(ben_or_thread_id, BEN_OR_WFLAGS_CRANE);
}

static uint8_t get_msgone_mode(uint32_t *cons_val_mode)
{
    uint8_t i = 0;
    uint8_t val_cnt[5], mode_cnt, index;
    
    while(osMutexAcquire(cmsg_1_mutex, 1000) != osOK);
    for(i = 0;i < MAX_SHIPS;i++)
    {
        index = cmsg_1_val[i];
        if(cmsg_1_val[i] > 0 && cmsg_1_val[i] <= CM_PLACE_CARGO)
        {
            val_cnt[index-1]++;
        }
    }
	osMutexRelease(cmsg_1_mutex);

	mode_cnt = val_cnt[0];
	*cons_val_mode = CM_UP; // CM_UP == 1
	for(i = 1;i < CM_PLACE_CARGO;i++)
    {
        if(mode_cnt < val_cnt[i])
        {
            mode_cnt = val_cnt[i];
            *cons_val_mode = i+1;
        }   
    }
    
    return mode_cnt;
}

static uint8_t get_msgtwo_mode(uint32_t *cons_val_d)
{
    uint8_t i = 0;
    uint8_t d_val_cnt[5], mode_cnt, index;
    
    while(osMutexAcquire(cmsg_2_mutex, 1000) != osOK);
    for(i = 0;i < MAX_SHIPS;i++)
    {
        if(cmsg_2_decision[i])
        {
            if(cmsg_2_val[i] > 0 && cmsg_2_val[i] <= CM_PLACE_CARGO)
            {
                index = cmsg_2_val[i];
                d_val_cnt[index-1]++;
            }
        }
    }
	osMutexRelease(cmsg_2_mutex);

	mode_cnt = d_val_cnt[0];
	*cons_val_d = CM_UP; // CM_UP == 1
	for(i = 1;i < CM_PLACE_CARGO;i++)
    {
        if(mode_cnt < d_val_cnt[i])
        {
            mode_cnt = d_val_cnt[i];
            *cons_val_d = i+1;
        }   
    }
    
    return mode_cnt;
}

static crane_command_t get_d_val_single()
{
    uint8_t i = 0;
    crane_command_t val;
    
    while(osMutexAcquire(cmsg_2_mutex, 1000) != osOK);
    for(i = 0;i < MAX_SHIPS;i++)
    {
        if(cmsg_2_decision[i])
        {
            val = cmsg_2_val[i];
            break;
        }
    }
	osMutexRelease(cmsg_2_mutex);

	return val;
}

