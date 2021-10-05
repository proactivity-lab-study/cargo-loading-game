/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#ifndef GAME_TYPES_H
#define GAME_TYPES_H

//-------- RADIO MESSAGE IDs
#define CRANE_COMMAND_MSG 111   //0x6F
#define CRANE_LOCATION_MSG 112  //0x70

#define WELCOME_MSG 115     //0x73
#define GTIME_QMSG 116		//0x74          // Global time query message
#define SHIP_QMSG 117		//0x75          // [ship ID] departure time, location and cargo status query message
#define AS_QMSG 118			//0x76          // Query of all ship IDs of ships in the game
#define ACARGO_QMSG 119		//0x77          // Query of cargo status of all ships in the game

#define WELCOME_RMSG 121	//0x79          // Response to welcome message
#define GTIME_QRMSG 122		//0x7A          // Global time query response message
#define SHIP_QRMSG 123		//0x7B          // [ship ID] departure time, location and cargo status query response message
#define AS_QRMSG 124		//0x7C          // Response for query of all ship IDs of ships in the game
#define ACARGO_QRMSG 125	//0x7D          // Rsponse for query of cargo status of all ships in the game

//-------- AGENT IDs
#define	CRANE_ADDR 13        //0x0D
#define	SYSTEM_ADDR CRANE_ADDR

//-------- CRANE COMMANDS
typedef enum
{
    CM_NO_COMMAND           = 0,
	CM_UP 					= 1,
	CM_DOWN 				= 2,
	CM_LEFT 				= 3,
	CM_RIGHT 				= 4,
	CM_PLACE_CARGO 			= 5,
	CM_CURRENT_LOCATION		= 6,
	CM_NOTHING_TO_DO 		= 7
} crane_command_t;

#define CRANE_UPDATE_INTERVAL 3UL // Seconds
#define MAX_SHIPS 10 // Maximum number of ships in game

#define GRID_LOWER_BOUND 2 	// Including - grid lower bound should be > 0
#define GRID_UPPER_BOUND 30 // Including - grid upper bound should be < 255

#define DURATION_OF_GAME 900UL 	// Seconds
#define DEFAULT_TIME 300 		// Seconds
#define MIN_LOADING_TIME 150 	// Seconds
#define MAX_LOADING_TIME 600 	// Seconds
#define DEFAULT_LOC 13

typedef struct {
	uint8_t x;
	uint8_t y;
} loc_bundle_t;

typedef struct {
	uint8_t crane_x;
	uint8_t crane_y;
	bool cargo_here;
} crane_location_t;

#endif //GAME_TYPES_H
