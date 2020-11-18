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

typedef struct shipTargetProposal {
	uint8_t messageID;
	uint8_t senderID;
	uint8_t targetID;
} shipTargetProposal;

//propose next command to send to crane
typedef struct shipMoveProposal {
	uint8_t messageID;
	uint8_t senderID;
	uint8_t cmd;//command
} shipMoveProposal;

//propose next command to send to crane
typedef struct shipProposalResponse {
	uint8_t messageID;
	uint8_t senderID;
	uint8_t approved; //0 - not agreed, >1 - agreed
} shipProposalResponse;

//-------- CRANE MESSAGE STRUCTURES
#pragma pack(push)
#pragma pack(1)
typedef struct craneLocationMsg {
	uint8_t messageID;
	am_addr_t senderID;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t cargoPlaced;
} craneLocationMsg;

#pragma pack(1)
typedef struct craneCommandMsg {
	uint8_t messageID;
	am_addr_t senderID;
	uint8_t cmd;//command
} craneCommandMsg;

//-------- SYSTEM MESSAGE STRUCTURES

#pragma pack(1)
typedef struct queryMsg { //structure for all system queries and welcome message
	uint8_t messageID; //this defines the type of the query 
	uint8_t senderID;
	uint8_t shipID; //optional, not used in all queries
} queryMsg;

#pragma pack(1)
typedef struct queryResponseMsg { //structure for all system queries and welcome message responses
	uint8_t messageID; //this defines the type of the response
	uint8_t senderID;
	uint8_t shipID; 
	uint16_t departureT; //optional, not used in all responses
	uint8_t x_coordinate; //optional, not used in all responses
	uint8_t y_coordinate; //optional, not used in all responses
	uint8_t isCargoLoaded;//optional, not used in all responses
} queryResponseMsg;


#pragma pack(pop)

//not related to communication, but I don't want to make a new h-file
typedef struct locBundle {
	uint8_t x_coordinate;
	uint8_t y_coordinate;
}locBundle;


#endif // CLG_COMM_H
