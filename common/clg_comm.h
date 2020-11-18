#ifndef CLG_COMM_H
#define CLG_COMM_H

enum RadioCountToLedsEnum
{
    AMID_CRANECOMMUNICATION 	= 6,
	AMID_SYSTEMCOMMUNICATION 	= 7,
	AMID_SHIPCOMMUNICATION 		= 8
};

/************************************************************
 *	Radio message structures
 ************************************************************/

//-------- SHIP MESSAGE STRUCTURES

#pragma pack(push)
#pragma pack(1)
typedef struct coop_msg_t {
	uint8_t messageID;
	am_addr_t senderAddr;
	am_addr_t destinationAddr;
} coop_msg_t;

#pragma pack(1)
typedef struct coop_msg_ans_t {
	uint8_t messageID;
	am_addr_t senderAddr;
	am_addr_t destinationAddr;
	bool agreement;
} coop_msg_ans_t;

//-------- CRANE MESSAGE STRUCTURES

#pragma pack(1)
typedef struct craneLocationMsg {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t cargoPlaced;
} craneLocationMsg;

#pragma pack(1)
typedef struct craneCommandMsg {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t cmd;//command
} craneCommandMsg;

//-------- SYSTEM MESSAGE STRUCTURES

#pragma pack(1)
typedef struct queryMsg { //structure for all system queries and welcome message
	uint8_t messageID; //this defines the type of the query 
	am_addr_t senderAddr;
	uint8_t shipAddr; //optional, not used in all queries
} queryMsg;

#pragma pack(1)
typedef struct queryResponseMsg { //structure for all system queries and welcome message responses
	uint8_t messageID; //this defines the type of the response
	am_addr_t senderAddr;
	am_addr_t shipAddr; 
	uint16_t departureT; //optional, not used in all responses
	uint8_t x_coordinate; //optional, not used in all responses
	uint8_t y_coordinate; //optional, not used in all responses
	uint8_t isCargoLoaded;//optional, not used in all responses
} queryResponseMsg;
#pragma pack(pop)

#endif // CLG_COMM_H
