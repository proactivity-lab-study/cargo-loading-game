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
// If crane location data is unavailable for more than 1000 kernel ticks, returns 
// last available crane location. Availability here is related to cloc_mutex.
// This function can block for 1000 kernel ticks.
uint16_t distToCrane(uint8_t x, uint8_t y);

#endif //CRANE_CONTROL_H_
