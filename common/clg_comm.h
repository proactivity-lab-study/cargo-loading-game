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

// Add your ship-to-ship message structures here
#pragma pack(1)
typedef struct {            // Template for ship-to-ship message
	uint8_t messageID; 		// This defines the type of the message, recommended
	am_addr_t senderAddr;	// This defines the sender of the message, also recommended
	uint8_t val8;			
	uint16_t val16;			// Use hton16() when sending and ntoh16() when receiving, see endianness.h
	uint32_t val32;			// Use hton32() when sending and ntoh32() when receiving, see endianness.h
	float valf;				// Use htonf() when sending and ntohf() when receiving, see endianness.h
} ship_msg_template_t;

// 'Next command' message 
#pragma pack(1)
typedef struct {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t cmd;			
} ship_next_cmd_msg_t;

// 'Next ship' message 
#pragma pack(1)
typedef struct {
	uint8_t messageID;
	am_addr_t senderAddr;
	am_addr_t nextShip;			
} ship_next_ship_msg_t;

//-------- CRANE MESSAGE STRUCTURES

#pragma pack(1)
typedef struct {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t x_coordinate;
	uint8_t y_coordinate;
	uint8_t cargoPlaced;
} crane_location_msg_t;

#pragma pack(1)
typedef struct {
	uint8_t messageID;
	am_addr_t senderAddr;
	uint8_t cmd; // Command
} crane_command_msg_t;

//-------- SYSTEM MESSAGE STRUCTURES

#pragma pack(1)
typedef struct { // Structure for all system queries and welcome message
	uint8_t messageID; 		// This defines the type of the query 
	am_addr_t senderAddr;
	am_addr_t shipAddr; 	// Optional, not used in all queries
} query_msg_t;

#pragma pack(1)
typedef struct { // Structure for system query and welcome message responses
	uint8_t messageID; 		// This defines the type of the response
	am_addr_t senderAddr;
	am_addr_t shipAddr; 
	uint16_t loadingDeadline; 	// Seconds left; optional, not used in all responses
	uint8_t x_coordinate; 	// Optional, not used in all responses
	uint8_t y_coordinate; 	// Optional, not used in all responses
	uint8_t isCargoLoaded;	// Optional, not used in all responses
} query_response_msg_t;

#pragma pack(1)
typedef struct { // Structure for all ships and all cargo query responses
	uint8_t messageID; 		// This defines the type of the response
	am_addr_t senderAddr;
	am_addr_t shipAddr;
	uint8_t len; 			// Number of addresses in 'ships' buffer
	am_addr_t ships[MAX_SHIPS];
} query_response_buf_t;

#pragma pack(pop)

#endif // CLG_COMM_H
