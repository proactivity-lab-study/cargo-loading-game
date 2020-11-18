/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef GAME_STATUS_H_
#define GAME_STATUS_H_

#include "game_types.h"

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_system_status(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

//returns location of ship, if no such ship, returns 0 for both coordinates
//this function may block
loc_bundle_t get_ship_location(am_addr_t ship_addr); 

#endif //GAME_STATUS_H_
