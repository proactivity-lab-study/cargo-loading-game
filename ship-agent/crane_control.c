/**
 * 
 * This is the crane control module of ship-agent. It is responsible for sending
 * crane control commands to crane-agent. It also listens to control commands 
 * sent by other ship-agents and keeps track of their commands. All sorts of
 * different crane control tactics are implemented here.
 * 
 * Main functionality of this module is :
 * 
 * - keep track of crane update interval and possibly issue commands each interval
 * - send control command messages to crane-agent
 * - choose approriate crane control command based on current tactics
 * - listen to and store command messages of other ship-agents
 * - keep a local record of current crane state (location)
 * - signal game state module about cargo receivement events (both self and others)
 * 
 * Crane control module sits and waits for crane location messages to arrive. Upon
 * receiving a crane location message, local record of crane state is updated. If
 * cargo is placed since last crane state and crane location matches location of 
 * any ship in game (including self), then game status module is notified of cargo
 * receivement for that ship. Also after receiving a crane location message crane
 * update interval time count is reset. Crane control module must choose a command
 * and send a crane command messages before crane update interval time passes.
 * 
 * Control commands are chosen based on crane control tactics. Ship strategy module 
 * must choose (and implement) a tactic. Some basic tactics are implemented (see
 * crane_control.h) but different new tactics can be added by users. 
 * 
 * Note:
 * 		After ship-agent boot and first initialisation the physical address of 
 * 		crane-agent is not yet known. The first CRANE_LOCATION_MSG to arrive reveals
 * 		(identity manipulation threat?) crane-agent physical address. All messages
 * 		sent before this event will be broadcast and all messages sent after this
 * 		event will be unicast. 
 * 
 * TODO Currently crane command messages are sent as unicast directly to crane-
 * 		agent. This means tactics such as cc_parrot_ship and cc_popular_command
 * 		wont work, because nobody except crane will receive crane control messages.
 * 		Simple solution would be to use broadcast globally, but maybe make it a 
 * 		compile time choice?
 * 
 * TODO CRANE_ADDR and SYSYEM_ADDR are still used to identify crane-agent. This
 * 		however does not solve crane-agent identity theft and impersonation problem
 * 		so in this regard it is redundant. Suggested for removal.
 * 
 * TODO Sending command CM_CURRENT_LOCATION and receiving a location message
 * 		in response resets crane interval time count. This messes up crane 
 * 		command sending. Possible fix is to set some sort of flag about sending
 * 		command CM_CURRENT_LOCATION and then when a location message is received
 * 		crane interval time count is not reset and the flag is cleared instead.
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

#include "crane_control.h"
#include "game_status.h"
#include "clg_comm.h"
#include "game_types.h"
#include "ship_strategy.h"

#include "loglevels.h"
#define __MODUUL__ "ccntr"
#define __LOG_LEVEL__ (LOG_LEVEL_crane_control & BASE_LOG_LEVEL)
#include "log.h"

typedef struct scmd_t
{
	am_addr_t ship_addr;
	crane_command_t ship_cmd;
}scmd_t;

static scmd_t cmds[MAX_SHIPS];
static uint32_t lastCraneEventTime = 0x0FFFFFFFU; // Event initial value; kernel ticks
static crane_location_t cloc;

// Some initial tactics choices
static bool Xfirst = true; // Which coordinate to use first, x is default
static bool alwaysPlaceCargo = true; // Always send 'place cargo' command when crane is on top of a ship

static cmd_sel_tactic_t tactic;
static am_addr_t tactic_addr;
static loc_bundle_t tactic_loc;
static crane_command_t tactic_cmd;

static osMutexId_t cmdb_mutex, cloc_mutex, cctt_mutex;
static osMessageQueueId_t cmsg_qID, lmsg_qID, smsg_qID;
static osThreadId_t snd_task_id;

static comms_msg_t m_msg;
static comms_layer_t* cradio;
static am_addr_t my_address;
static am_addr_t crane_address = AM_BROADCAST_ADDR; // Use actual crane address if possible
static bool first_msg = true; // Used to get actual crane address once

static void craneMainLoop(void *args);
static void locationMsgHandler(void *args);
static void commandMsgHandler(void *args);
static void sendCommandMsg(void *args);

static uint8_t getEmptySlot();
static crane_command_t goToDestination(uint8_t x, uint8_t y);
static crane_command_t parrotShip(am_addr_t sID);
static crane_command_t selectPopular();
static crane_command_t selectCommand(uint8_t x, uint8_t y);
static crane_command_t selectCommandXFirst(uint8_t x, uint8_t y);
static crane_command_t selectCommandYFirst(uint8_t x, uint8_t y);
static void clearCmdsBuf();

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initCraneControl(comms_layer_t* radio, am_addr_t addr)
{
	const osMutexAttr_t cloc_Mutex_attr = { .attr_bits = osMutexRecursive }; // Allow nesting of this mutex

	cmdb_mutex = osMutexNew(NULL);				// Protects ships' crane command database
	cloc_mutex = osMutexNew(&cloc_Mutex_attr);	// Protects current crane location values
	cctt_mutex = osMutexNew(NULL);				// Protects tactics related variables
	
	smsg_qID = osMessageQueueNew(MAX_SHIPS + 3, sizeof(crane_command_msg_t), NULL); // Send queue
	cmsg_qID = osMessageQueueNew(MAX_SHIPS + 3, sizeof(crane_command_msg_t), NULL); // Receive queue
	lmsg_qID = osMessageQueueNew(6, sizeof(crane_location_msg_t), NULL);
	
	// Initialise ships' commands buffer
	clearCmdsBuf();

	cradio = radio;
	my_address = addr;

	// Get crane start location
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	cloc.crane_y = 0;
	cloc.crane_x = 0;
	cloc.cargo_here = false;
	osMutexRelease(cloc_mutex);

	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	tactic = cc_to_address;
	tactic_loc.x = tactic_loc.y = 0; // Most likely I don't have a location yet
	tactic_addr = my_address;
	tactic_cmd = CM_NO_COMMAND;
	osMutexRelease(cctt_mutex);

    osThreadNew(commandMsgHandler, NULL, NULL);		// Handles received crane command messages
    osThreadNew(locationMsgHandler, NULL, NULL);	// Handles received crane location messages
    snd_task_id = osThreadNew(sendCommandMsg, NULL, NULL);		// Handles command message sending
	osThreadFlagsSet(snd_task_id, 0x00000001U); 	// Sets thread to ready-to-send state
	osThreadNew(craneMainLoop, NULL, NULL);			// Crane state changes
}

/**********************************************************************************************
 *	Crane status changes
 *********************************************************************************************/

static void craneMainLoop(void *args)
{
	static crane_command_t cmd = CM_NOTHING_TO_DO;
	cargo_status_t  stat;
	cmd_sel_tactic_t tt;
	uint32_t time_left, ticks;
	am_addr_t addr;
	loc_bundle_t loc;
	crane_command_msg_t packet;

	ticks = (uint32_t)(0.5 * osKernelGetTickFreq()); // Half a second
	for(;;)
	{
		osDelay(ticks);
		
		time_left = CRANE_UPDATE_INTERVAL*osKernelGetTickFreq() + lastCraneEventTime - osKernelGetTickCount();
		if(time_left < ticks)
		{
			while(osMutexAcquire(cctt_mutex, 1000) != osOK);
			tt = tactic;
			addr = tactic_addr;
			loc = tactic_loc;
			cmd = tactic_cmd;
			osMutexRelease(cctt_mutex);

			switch(tt)
			{
				case cc_do_nothing : 		// Don't send crane control command messages.

					cmd = CM_NOTHING_TO_DO;
					break;

				case cc_to_address :		// Call crane to specified ship and place cargo.

					stat = getCargoStatus(addr);
					if(stat == cs_cargo_not_received)
					{
						loc = getShipLocation(addr);
						cmd = goToDestination(loc.x, loc.y);
					}
					else cmd = CM_NOTHING_TO_DO; // Nothing to do, cuz cargo placed or no such ship.
					break;

				case cc_to_location :		// Call crane to specified location and place cargo.

					stat = getCargoStatus(getShipAddr(loc));
					if(stat == cs_cargo_not_received)
					{
						cmd = goToDestination(loc.x, loc.y);
					}
					else cmd = CM_NOTHING_TO_DO; // Nothing to do, cuz cargo placed or no such ship.
					break;

				case cc_parrot_ship :		// Send same command message as specified ship.

					cmd = parrotShip(addr);
					break;

				case cc_popular_command	:	// Send the command that is currently most popular.

					cmd = selectPopular();
					break;
					
			    case cc_only_consensus	:	// Send only when consensus, only consensus value.

					if(cmd != CM_NO_COMMAND)
					{
					    while(osMutexAcquire(cctt_mutex, 1000) != osOK);
					    tactic_cmd = CM_NO_COMMAND; // Reset after every round.
					    osMutexRelease(cctt_mutex);
					}
					else cmd = CM_NOTHING_TO_DO;
					break;

				default :

					cmd = CM_NOTHING_TO_DO;
					break;
			}

			info1("Cmnd sel %u", cmd);
			if(cmd != CM_NOTHING_TO_DO)
			{
				packet.messageID = CRANE_COMMAND_MSG;
				packet.senderAddr = my_address;
				packet.cmd = (uint8_t) cmd;
				osMessageQueuePut(smsg_qID, &packet, 0, 0);
			}
			else ; // Nothing to do.
		}
		else ; // There is still time until crane update event, wait!
	}
}

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void craneReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
	uint8_t pl_len = comms_get_payload_length(comms, msg);
	am_addr_t crane_addr;
	
	if (pl_len == sizeof(crane_command_msg_t))
    {
        crane_command_msg_t * packet = (crane_command_msg_t*)comms_get_payload(comms, msg, sizeof(crane_command_msg_t));
        debug1("rcv-c");
        osStatus_t err = osMessageQueuePut(cmsg_qID, packet, 0, 0);
		if(err == osOK)debug1("command rcvd");
		else debug1("cmsgq err");
    }
    else if (pl_len == sizeof(crane_location_msg_t))
	{
		if(first_msg)crane_addr = comms_am_get_source(comms, msg);

		crane_location_msg_t * packet = (crane_location_msg_t*)comms_get_payload(comms, msg, sizeof(crane_location_msg_t));
        debug1("rcv-l");

		if(packet->messageID == CRANE_LOCATION_MSG && ntoh16(packet->senderAddr) == CRANE_ADDR && first_msg)
		{
			crane_address = crane_addr;
			first_msg = false;
		}

        osStatus_t err = osMessageQueuePut(lmsg_qID, packet, 0, 0);
		if(err == osOK)debug1("crane location rcvd");
		else debug1("lmsgq err");
	}
	else debug1("rcv size %d", (unsigned int)comms_get_payload_length(comms, msg));
}

static void locationMsgHandler(void *args)
{
	crane_location_msg_t packet;
	loc_bundle_t sloc;
	am_addr_t saddr;
	for(;;)
	{
		osMessageQueueGet(lmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_LOCATION_MSG && ntoh16(packet.senderAddr) == CRANE_ADDR)
		{
			lastCraneEventTime = osKernelGetTickCount();
			info1("Crane mov %u %u %u", packet.x_coordinate, packet.y_coordinate, packet.cargoPlaced);

			while(osMutexAcquire(cloc_mutex, 1000) != osOK);
			cloc.crane_x = packet.x_coordinate;
			cloc.crane_y = packet.y_coordinate;
			cloc.cargo_here = packet.cargoPlaced;
			osMutexRelease(cloc_mutex);
			
			// If cargo was placed, check if we need to update our knowledge base
			if(packet.cargoPlaced)
			{
				sloc.x = packet.x_coordinate;
				sloc.y = packet.y_coordinate;
				saddr = getShipAddr(sloc);
				if(saddr != 0 && getCargoStatus(saddr) != 0)markCargo(saddr);
			}

			clearCmdsBuf(); // Clear contents of cmds buffer
			
			// Notify ship strategy module that a new crane round begun.
			notifyNewCraneRound();
		}
	}
}

// Listen to and store commands sent by other ships
static void commandMsgHandler(void *args)
{
	uint8_t i;
	crane_command_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(cmsg_qID, &packet, NULL, osWaitForever);
		if(packet.messageID == CRANE_COMMAND_MSG)
		{
			info1("Cmnd %lu", ntoh16(packet.senderAddr));
			while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
			for(i=0;i<MAX_SHIPS;i++)
			{
				if(cmds[i].ship_addr == ntoh16(packet.senderAddr))
				{
					cmds[i].ship_cmd = (crane_command_t) packet.cmd;
					break;
				}
			}
			if(i>=MAX_SHIPS) // Add ship and command if room
			{
				i = getEmptySlot();
				if(i<MAX_SHIPS)
				{
					cmds[i].ship_addr = ntoh16(packet.senderAddr);
					cmds[i].ship_cmd = (crane_command_t) packet.cmd;
				}
				else ; // Drop this ships command, cuz no room
			}
			osMutexRelease(cmdb_mutex);
		}
	}
}

/**********************************************************************************************
 *	Message sending
 **********************************************************************************************/

static void radioSendDone(comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "Cmnd sent %u", result);
    osThreadFlagsSet(snd_task_id, 0x00000001U);
}

static void sendCommandMsg(void *args)
{
	crane_command_msg_t packet;

	for(;;)
	{
		osMessageQueueGet(smsg_qID, &packet, NULL, osWaitForever);
		
		osThreadFlagsWait(0x00000001U, osFlagsWaitAny, osWaitForever); // Flags are automatically cleared

		comms_init_message(cradio, &m_msg);
		crane_command_msg_t * cMsg = comms_get_payload(cradio, &m_msg, sizeof(crane_command_msg_t));
		if (cMsg == NULL)
		{
			continue ;// Continue for(;;) loop
		}

		cMsg->messageID = packet.messageID;
		cMsg->senderAddr = hton16(packet.senderAddr);
		cMsg->cmd = packet.cmd;
			
		// Send data packet
	    comms_set_packet_type(cradio, &m_msg, AMID_CRANECOMMUNICATION);
	    comms_am_set_destination(cradio, &m_msg, crane_address);
	    comms_set_payload_length(cradio, &m_msg, sizeof(crane_command_msg_t));

	    comms_error_t result = comms_send(cradio, &m_msg, radioSendDone, NULL);
	    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
	}
}

/**********************************************************************************************
 *	Crane command and tactics functions
 **********************************************************************************************/

// Sets whether crane is commanded to move along x coordinate first or along y coordinate first.
// Used with tactic 'cc_to_address' and 'cc_to_location'.

void setXFirst(bool val)
{
	static bool v;
	v = val;
	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	Xfirst = val;
	osMutexRelease(cctt_mutex);
	info1("X first %u", (uint8_t) v);
}

// Returns whether crane is commanded to move along x coordinate first or along y coordinate first.
// Used with tactic 'cc_to_address' and 'cc_to_location'.

bool getXFirst()
{
	bool val;

	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	val = Xfirst;
	osMutexRelease(cctt_mutex);
	
	return val;
}

// Sets whether cargo is always placed whenever crane is at some ship location.
// Setting 'true' results in always issuing place cargo commend when crane is at some ship location.
// Setting 'false' results in placing cargo only at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.

void setAlwaysPlaceCargo(bool val)
{
	static bool v;
	v = val;
	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	alwaysPlaceCargo = val;
	osMutexRelease(cctt_mutex);
	info1("Always place cargo %u", (uint8_t) v);
}

// Returns cargo placement tactics choice. 
// If 'true' cargo is placed on always when crane is at some ship location.
// If 'false' cargo is only placed at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.

bool getAlwaysPlaceCargo()
{
	bool val;

	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	val = alwaysPlaceCargo;
	osMutexRelease(cctt_mutex);

	return val;
}

// Sets tactical choice for crane command selection.
// Possible choices are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.

void setCraneTactics(cmd_sel_tactic_t tt, am_addr_t ship_addr, loc_bundle_t loc)
{
	static cmd_sel_tactic_t tact;
	tact = tt;
	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	tactic = tt;
	tactic_addr = ship_addr;
	tactic_loc = loc;
	osMutexRelease(cctt_mutex);
	info1("Crane tactics %u", tact);
}

// Returns current tactical choise.
// Possible return values are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.

cmd_sel_tactic_t getCraneTactics(am_addr_t *ship_addr, loc_bundle_t *loc)
{
	cmd_sel_tactic_t tt;
	static am_addr_t addr;
	static loc_bundle_t l;
	
	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	tt = tactic;
	addr = tactic_addr;
	l = tactic_loc;
	osMutexRelease(cctt_mutex);

	ship_addr = &addr;
	loc = &l;
	
	return tt;
}

void send_consensus_command(crane_command_t cons_val)
{
    while(osMutexAcquire(cctt_mutex, 1000) != osOK);
    tactic_cmd = cons_val;
	osMutexRelease(cctt_mutex);
}

// Selects an appropriate command to get the crane to location (x; y)
// Takes into account 'Xfirst' and 'alwaysPlaceCargo' choices.
// This function can return CM_NOTHING_TO_DO in some cases
static crane_command_t goToDestination(uint8_t x, uint8_t y)
{
	crane_command_t cmd;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	if(x != 0 && y != 0)cmd = selectCommand(x, y);
	osMutexRelease(cloc_mutex);
	return cmd;
}

// Returns command sent by ship with sID.
// If no such ship or no command, returns CM_NOTHING_TO_DO.
// This tactic can work only if other ships send their 
// crane command messages as broadcast. 
static crane_command_t parrotShip(am_addr_t sID)
{
	crane_command_t cmd, i;

	cmd = CM_NOTHING_TO_DO;

	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		if(cmds[i].ship_addr == sID)
		{
			cmd = cmds[i].ship_cmd;
			break;
		}
	}
	osMutexRelease(cmdb_mutex);

	return cmd;
}

// Returns most popular command sent by all other ships this round.
// In case of tie, favors the first most popular choice found.
// If no ship or commands, returns CM_NOTHING_TO_DO.
// This tactic can work only if other ships send their 
// crane command messages as broadcast. 
static crane_command_t selectPopular()
{
	uint8_t i, n;
	uint8_t cmd[7];

	// Empty the buffer.
	for(i=0;i<7;i++)cmd[i] = 0;

	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		switch(cmds[i].ship_cmd)
		{
			case CM_UP : // 1
			cmd[1]++;
			break;

			case CM_DOWN : // 2
			cmd[2]++;
			break;

			case CM_LEFT : // 3
			cmd[3]++;
			break;

			case CM_RIGHT : // 4
			cmd[4]++;
			break;

			case CM_PLACE_CARGO : // 5
			cmd[5]++;
			break;

			case CM_CURRENT_LOCATION : // 6
			cmd[6]++;
			break;

			default : // 0 && 7
			break;
		}
	}
	osMutexRelease(cmdb_mutex);

	// This favors the first most popular choice.
	n=0;
	cmd[0] = (uint8_t) CM_NOTHING_TO_DO;
	for(i=1;i<7;i++)if(n < cmd[i])
	{
		n = cmd[i];
		cmd[0] = i; // Using the 0 index memory area for this, because it is available.
	}
	
	return cmd[0];
}

static crane_command_t selectCommand(uint8_t x, uint8_t y)
{
	uint8_t len = 0, i;
	cargo_status_t stat;
	bool x_first, placeCargo;
	am_addr_t ships[MAX_SHIPS];
	loc_bundle_t sloc;

	while(osMutexAcquire(cctt_mutex, 1000) != osOK);
	x_first = Xfirst;
	placeCargo = alwaysPlaceCargo;
	osMutexRelease(cctt_mutex);

	// First check if cargo was placed in the last round, if not, maybe we need to.
	if(!cloc.cargo_here)
	{
		// There is no cargo in this place, is there a ship here and do we need to place cargo?
		if(placeCargo)
		{
			len = getAllShipsAddr(ships, MAX_SHIPS);

			if(len > 0 && len <= MAX_SHIPS)
			{
				for(i=0;i<len;i++)
				{
					sloc = getShipLocation(ships[i]);
					// If there is a ship here, then only reasonable command is place cargo.
					if(distToCrane(sloc) == 0)
					{
						stat = getCargoStatus(ships[i]);
						if(stat != cs_cargo_received)return CM_PLACE_CARGO; // Ship here, no cargo.
						else break; // Ship here, has cargo.
					}
				}
			}
			else ; // No more ships in game besides me.

			// If I reach here, then there are no ships besides me in the game
			// or there are more ships but no one is at the current crane location.
			// Therefor just continue with the strategy.
		}
		else ; // Don't care about cargo placement unless it serves my strategy.
	}
	else ; // Cargo was placed by the crane in the last round, so no need to place it again this round.

	if(x_first)return selectCommandXFirst(x, y);
	else return selectCommandYFirst(x, y);
}

static crane_command_t selectCommandYFirst(uint8_t x, uint8_t y)
{
	if(y > cloc.crane_y)return CM_UP;
	else if(y < cloc.crane_y)return CM_DOWN;
	else ;

	if(x > cloc.crane_x)return CM_RIGHT;
	else if(x < cloc.crane_x)return CM_LEFT;
	else ;

	// If we get here, then the crane is at the desired location (x; y).
	// Check if there is cargo here and issue place cargo, if there isn't, else return with 'do nothing'.
	// This ensures that we only place cargo to a ship only once.
	if(cloc.cargo_here)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

static crane_command_t selectCommandXFirst(uint8_t x, uint8_t y)
{
	if(x > cloc.crane_x)return CM_RIGHT;
	else if(x < cloc.crane_x)return CM_LEFT;
	else ;

	if(y > cloc.crane_y)return CM_UP;
	else if(y < cloc.crane_y)return CM_DOWN;
	else ;

	// If we get here, then the crane is at the desired location (x; y).
	// Check if there is cargo here and issue place cargo, if there isn't, else return with 'do nothing'.
	// This ensures that we only place cargo to a ship only once.
	if(cloc.cargo_here)return CM_NOTHING_TO_DO;
	else return CM_PLACE_CARGO;
}

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns distance to crane, zero distance means crane is at location (x; y).

uint16_t distToCrane(loc_bundle_t loc)
{
	static uint16_t dist;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	dist = abs(cloc.crane_x - loc.x) + abs(cloc.crane_y - loc.y);
	osMutexRelease(cloc_mutex);
	return dist;
}

loc_bundle_t getCraneLoc()
{
	static loc_bundle_t craneloc;
	while(osMutexAcquire(cloc_mutex, 1000) != osOK);
	craneloc.x = cloc.crane_x;
	craneloc.y = cloc.crane_y;
	osMutexRelease(cloc_mutex);
	return craneloc;
}

static void clearCmdsBuf()
{
	uint8_t i;
	while(osMutexAcquire(cmdb_mutex, 1000) != osOK);
	for(i=0;i<MAX_SHIPS;i++)
	{
		cmds[i].ship_addr = 0;
		cmds[i].ship_cmd = CM_NO_COMMAND;
	}
	osMutexRelease(cmdb_mutex);
}

//TODO mutex protection if more than commandMsgHandler is calling this function
// Mutex nesting is allowed, but 'release' must be called the same number of times as 'acquire'.
static uint8_t getEmptySlot() 
{
	uint8_t k;
	for(k=0;k<MAX_SHIPS;k++)if(cmds[k].ship_addr == 0)break;
	return k;
}
