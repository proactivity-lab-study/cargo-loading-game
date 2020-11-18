#ifndef GAME_TYPES_H
#define GAME_TYPES_H

//-------- RADIO MESSAGE_TYPES
#define CRANE_COMMAND_MSG 111   //0x6F
#define CRANE_LOCATION_MSG 112  //0x70

#define WELCOME_MSG 115     //0x73
#define GTIME_QMSG 116		//0x74          //global time query message
#define SHIP_QMSG 117		//0x75          //[ship ID] departure time, location and cargo status query message
#define AS_QMSG 118			//0x76          //query of all ship IDs of ships in the game
#define ACARGO_QMSG 119		//0x77          //query of cargo status of all ships in the game

#define WELCOME_RMSG 121	//0x79          //response to welcome message
#define GTIME_QRMSG 122		//0x7A          //global time query response message
#define SHIP_QRMSG 123		//0x7B          //[ship ID] departure time, location and cargo status query response message
#define AS_QRMSG 124		//0x7C          //response for query of all ship IDs of ships in the game
#define ACARGO_QRMSG 125	//0x7D          //response for query of cargo status of all ships in the game

#define SHIP_TARGET_PROPOSAL_MSG 131
#define SHIP_MOVE_PROPOSAL_MSG 132
#define SHIP_PROPOSAL_RESPONSE_MSG 135

//-------- SERIAL MESSAGE_TYPES
#define SERIAL_CRANE_LOCMSG 1
#define SERIAL_NEWSHIP_MSG 2
#define SERIAL_GLOBALTIME_MSG 3

//-------- AGENT ID's
#define INVALID_ID  0
#define	CRANE_ID  13        //0x0D
#define	SYSTEM_ID CRANE_ID

//-------- CRANE COMMANDS
enum
{
	CM_UP 					= 1,
	CM_DOWN 				= 2,
	CM_LEFT 				= 3,
	CM_RIGHT 				= 4,
	CM_PLACE_CARGO 			= 5,
	CM_CURRENT_LOCATION		= 6,
	CM_NOTHING_TO_DO 		= 7
};

#define CRANE_UPDATE_INTERVAL 3UL //seconds
#define MAX_SHIPS 10 //maximum number of ships in knowledge database and game

typedef struct crane_location_t
{
	uint8_t craneX;
	uint8_t craneY;
	bool cargoInCurrentLoc;
}crane_location_t;

typedef struct loc_bundle_t {
	uint8_t x;
	uint8_t y;
}loc_bundle_t;


#endif //GAME_TYPES_H
