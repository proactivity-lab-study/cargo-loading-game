/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef CRANE_CONTROL_H_
#define CRANE_CONTROL_H_

#include "game_types.h"

// Crane control tactics
typedef enum {
	cc_do_nothing = 0,	// Don't send crane control command messages.
	cc_to_address,		// Call crane to specified ship and place cargo.
	cc_to_location,		// Call crane to specified location and place cargo.
	cc_parrot_ship,		// Send same command message as specified ship.
	cc_popular_command,	// Send the command that is currently most popular.
	cc_only_consensus   // Don't send crane commands unless a consensus is found, then send consensus value.
} cmd_sel_tactic_t;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initCraneControl(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void craneReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Crane command and tactics functions
 **********************************************************************************************/

// Sets whether crane is commanded to move along x coordinate first or along y coordinate first.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
void setXFirst(bool val);

// Returns whether crane is commanded to move along x coordinate first or along y coordinate first.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
bool getXFirst();

// Sets whether cargo is always placed whenever crane is at some ship location.
// Setting 'true' results in always issuing place cargo commend when crane is at some ship location.
// Setting 'false' results in placing cargo only at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
void setAlwaysPlaceCargo(bool val);

// Returns cargo placement tactics choice. 
// If 'true' cargo is placed on always when crane is at some ship location.
// If 'false' cargo is only placed at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
bool getAlwaysPlaceCargo();

// Sets tactical choice for crane command selection.
// Possible choices are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.
void setCraneTactics(cmd_sel_tactic_t tt, am_addr_t ship_addr, loc_bundle_t loc);

// Set the desired consensus value for next crane round.
void send_consensus_command(crane_command_t cons_val);

// Returns current tactical choise.
// Possible return values are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.
cmd_sel_tactic_t getCraneTactics(am_addr_t *ship_addr, loc_bundle_t *loc);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns distance to crane, zero distance means crane is at location (x; y).
uint16_t distToCrane(loc_bundle_t loc);

#endif //CRANE_CONTROL_H_
