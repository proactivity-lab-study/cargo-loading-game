/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */

#ifndef CLG_COMM_H
#define CLG_COMM_H

#include "game_types.h"

enum ActiveMessageIdEnum
{
    AMID_CRANECOMMUNICATION 	= 6,
	AMID_SYSTEMCOMMUNICATION 	= 7,
	AMID_SHIPCOMMUNICATION 		= 8
};

/************************************************************
 *	Radio message structures
 ************************************************************/
#pragma pack(push)

//-------- SHIP MESSAGE STRUCTURES

#pragma pack(push)
#pragma pack(1)
typedef struct coop_msg_t {
	uint8_t messageID;
	am_addr_t senderAddr;
	am_addr_t coopAddr;
	bool agreement;
} coop_msg_t;

//-------- CRANE MESSAGE STRUCTURES

#pragma pack(1)
typedef struct crane_location_msg_t {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t cargoPlaced;
} crane_location_msg_t;

#pragma pack(1)
typedef struct crane_command_msg_t {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t cmd; // Command
} crane_command_msg_t;

//-------- SYSTEM MESSAGE STRUCTURES

#pragma pack(1)
typedef struct query_msg_t { // Structure for all system queries and welcome message
	uint8_t messageID; 		// This defines the type of the query 
	am_addr_t senderAddr;
	am_addr_t shipAddr; 	// Optional, not used in all queries
} query_msg_t;

#pragma pack(1)
typedef struct query_response_msg_t { // Structure for system query and welcome message responses
	uint8_t messageID; 		// This defines the type of the response
	am_addr_t senderAddr;
	am_addr_t shipAddr; 
	uint16_t loadingDeadline; 	// Seconds left; optional, not used in all responses
	uint8_t x_coordinate; 	// Optional, not used in all responses
	uint8_t y_coordinate; 	// Optional, not used in all responses
	uint8_t isCargoLoaded;	// Optional, not used in all responses
} query_response_msg_t;

#pragma pack(1)
typedef struct query_response_buf_t { // Structure for all ships and all cargo query responses
	uint8_t messageID; 		// This defines the type of the response
	am_addr_t senderAddr;
	am_addr_t shipAddr;
	uint8_t len; 			// Number of addresses in 'ships' buffer
	am_addr_t ships[MAX_SHIPS];
} query_response_buf_t;

#pragma pack(pop)

#endif // CLG_COMM_H
