/**
 * 
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#ifndef SYSTEM_STATE_H_
#define SYSTEM_STATE_H_

// Ship database
typedef struct sdb_t {
	bool shipInGame;
	am_addr_t shipAddr;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint32_t ltime;	// Cargo loading deadline expressed as Kernel tick count
	bool isCargoLoaded;
} sdb_t;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_game();
void init_system(comms_layer_t* radio, am_addr_t my_addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns buffer index of ship with address 'id' or value MAX_SHIPS if no such ship.
uint8_t getIndex(am_addr_t ship_addr);

// Marks cargo status as true for ship with address 'addr', if such a ship is found.
// Use with care! There is no revers command to mark cargo status false.
// This function can block.
void markCargo(am_addr_t addr);

// Returns address of ship in location 'x', 'y' or 0 if no ship in this location.
// This function can block.
am_addr_t isShipHere(uint8_t x, uint8_t y);

#endif//SYSTEM_STATE_H_
