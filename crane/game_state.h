/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#ifndef GAME_STATE_H_
#define GAME_STATE_H_

#define DURATION_OF_GAME 600UL //seconds
#define DEFAULT_TIME 60
#define DEFAULT_LOC 13

//ship database
typedef struct sdb_t {
	bool shipInGame;
	uint8_t shipAddr;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint32_t ltime;
	bool isCargoLoaded;
} sdb_t;

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_game();
void init_system(comms_layer_t* radio);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void system_receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

uint8_t getIndex(uint8_t id);

#endif//GAME_STATE_H_
