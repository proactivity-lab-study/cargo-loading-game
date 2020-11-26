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

uint8_t getIndex(am_addr_t ship_addr);

#endif//SYSTEM_STATE_H_
