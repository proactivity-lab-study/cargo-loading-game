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

static float cv_probability[5];

static osMessageQueueId_t snd_msg_qID;
static osThreadId_t snd_task_id;
static osMutexId_t sfgl_mutex;

static comms_msg_t m_msg;
static comms_layer_t* sradio;
static am_addr_t my_address;
static uint32_t fglobal_val; // Read-write by multiple threads, needs mutex protection

static void ben_or_protocol(void *args); // A thread function
static void sendMsg(void *args); // Message sending thread

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
	
	sradio = radio; 	// This is the only write, so not going to protect it with mutex
	my_address = addr; 	// This is the only write, so not going to protect it with mutex
	srand(osKernelGetTickCount()); // Initialise random number generator
    
    cv_probability[0] = 0.0;
    cv_probability[1] = 0.0;
    cv_probability[2] = 0.0;
    cv_probability[3] = 0.0;
    cv_probability[4] = 0.0;

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
    crane_command_t my_proposal, consensus_value = CM_LEFT;
    uint16_t round_cnt = 0;
    bool no_consensus = true;
    osStatus_t send_err;
    loc_bundle_t loc = {0,0};
    uint32_t mode_val;
    uint32_t mode_val_cnt;
    
    //If not already, set crane tactics to cc_only_consensus
    setCraneTactics(cc_only_consensus, AM_BROADCAST_ADDR, loc);
    
	for(;;)
	{

		// Wait for new crane round.
	    // TODO I'm interested would the Ben Or protocol work without this sync. Are round counts necessary at all in our case???
		osThreadFlagsWait(BEN_OR_WFLAGS_CRANE, osFlagsWaitAll, osWaitForever); // osWaitForever, because without sync, no point in blindly sending messages
		
		while (no_consensus)
		{
		    round_cnt++;
		    // TODO get new consensus value
		    my_proposal = CM_PLACE_CARGO;
		    
		    // Send consensus message ONE.
		    send_err = sendConsensusMsgONE(my_proposal, round_cnt);
		    if(osOK != send_err)break; // Bail out and wait for new crane round.
		    
		    // Wait until received N*2/3 message ONE.
		    // TODO should we use waitforever???
		    osThreadFlagsWait(BEN_OR_WFLAGS_MSGONE, osFlagsWaitAll, osWaitForever);
		    
		        // TODO check for consensus (at least N/2 of the same value)
		        mode_val = CM_DOWN; // Dummy-value
		        mode_val_cnt = 9; // Dummy-value
		        no_consensus = false; // Dummy-value
		        
		        if(!no_consensus)my_proposal = mode_val;
		        else my_proposal = CM_UP; // This actually won't matter according to the consensus protocol.
		        
		        // Send consensus message TWO (either D(true) or ?(false).
		        send_err = sendConsensusMsgTWO(my_proposal, round_cnt, !no_consensus);
    		    if(osOK != send_err)break; // Bail out and wait for new crane round.
		    
		    // Wait until received N*2/3 message TWO
		    // TODO should we use waitforever???
		    osThreadFlagsWait(BEN_OR_WFLAGS_MSGTWO, osFlagsWaitAll, osWaitForever);
		    
		        // TODO check for consensus (at least N/2 of D message)
		            // Send crane command.
		            send_consensus_command(consensus_value);
		            no_consensus = false;
		       
		        // TODO else check for at least one D message
		            // TODO change consensus value to value from D message
		       
		        // TODO else get new consensus value
	        
	        // Start new round if consensus was not found (else wait for new crane round).
	    }
	    round_cnt = 0;
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
static osStatus_t sendConsensusMsgONE (crane_command_t value_proposal, uint16_t round_num)
{
	cons_msg_t packet;
	uint32_t wait_for_send;
	
	packet.messageID = CONSENSUS_MSG_ONE;
	packet.senderAddr = AM_BROADCAST_ADDR; // Consensus messages are broadcast messages
	packet.cons_value = value_proposal;     // Proposed consensus value (crane commands 1 - 5)
    packet.round_num = round_num;     // Round number
    // packet.decision = false; // Not used with msg ONE
	
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
    float random;
    float step_val;
    
    // get random number
    random = (float)randomNumber(0,100)/100;
    
    // get consensus value based on random number
    step_val = cv_probability[0];
    for(int i = 1; i <= 5;i++)
    {
    	if(step_val > random) // TODO what if i=5 ??
    	{
    	    val = i;
    	    break;
    	}
    	else step_val += cv_probability[i];
	}
    
    
    return val;
}


static uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}

void notifyNewCraneRound()
{
    // TODO Initiate calculation of new consensus probabilities

    // Initiate new consensus round.
    osThreadFlagsSet(ben_or_thread_id, BEN_OR_WFLAGS_CRANE);
}
