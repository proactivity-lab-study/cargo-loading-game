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

void initSystemStatus(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void systemReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns location of ship, if no such ship, returns 0 for both coordinates.
// This function can block.
loc_bundle_t getShipLocation(am_addr_t ship_addr);

// Marks cargo status as true for ship with address 'addr', if such a ship is found.
// Use with care! There is no revers command to mark cargo status false.
// This function can block.
void markCargo(am_addr_t addr);

// Fills buffer pointed to by 'saddr' with addresses of all ships currently known.
// Returns number of ships added to buffer 'saddr'.
// This function can block.
uint8_t  getAllShipsAddr(am_addr_t saddr[], uint8_t mlen);

// Returns address of ship in location 'sloc' or 0 if no ship in this location.
// This function can block.
am_addr_t getShipAddr(loc_bundle_t sloc);

// Returns cargo status of ship 'ship_addr'. Possible return values:
// 0 - cargo has been received, cargo present
// 1 - cargo has not been received, cargo not present
// 2 - ship not in database, unknown ship
// This function can block.
uint8_t getCargoStatus(am_addr_t ship_addr);

#endif //GAME_STATUS_H_
