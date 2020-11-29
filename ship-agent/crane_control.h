/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef CRANE_CONTROL_H_
#define CRANE_CONTROL_H_

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane_control(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/

// Returns distance to crane, zero distance means crane is at location (x; y).
// This function can block.
uint16_t distToCrane(uint8_t x, uint8_t y);

#endif //CRANE_CONTROL_H_
