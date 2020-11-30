/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef CRANE_CONTROL_H_
#define CRANE_CONTROL_H_

#include "game_types.h"

// Select crane tactics
enum {
	cc_do_nothing = 0,	// Don't send crane control command messages
	cc_to_address,		// Call crane to specified ship and place cargo
	cc_to_location,		// Call crane to specified location and place cargo
	cc_parrot_ship,		// Send same command message as specified ship
	cc_popular_command	// Send the command that is currently most popular
};

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
// This function can block.
void setXFirst(bool val);

// Returns whether crane is commanded to move along x coordinate first or along y coordinate first.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
// This function can block.
bool getXFirst();

// Sets whether cargo is always placed whenever crane is at some ship location.
// Setting 'true' results in always issuing place cargo commend when crane is at some ship location.
// Setting 'false' results in placing cargo only at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
// This function can block.
void setAlwaysPlaceCargo(bool val);

// Returns cargo placement tactics choice. 
// If 'true' cargo is placed on always when crane is at some ship location.
// If 'false' cargo is only placed at location designated by chosen tactics.
// Used with tactic 'cc_to_address' and 'cc_to_location'.
// This function can block.
bool getAlwaysPlaceCargo();

// Sets tactical choise for crane command selection.
// Possible choices are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.
// This function can block.
void setCraneTactics(uint8_t tt, am_addr_t ship_addr, loc_bundle_t loc);

// Returns current tactical choise.
// Possible return values are defined in crane_control.h
// Currently these are 'cc_do_nothing', 'cc_to_address', 'cc_to_location', 'cc_parrot_ship'
// and 'cc_popular_command'.
// This function can block.
uint8_t getCraneTactics(am_addr_t *ship_addr, loc_bundle_t *loc);
/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns distance to crane, zero distance means crane is at location (x; y).
// This function can block.
uint16_t distToCrane(uint8_t x, uint8_t y);

#endif //CRANE_CONTROL_H_
