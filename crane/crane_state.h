/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef CRANE_STATE_H_
#define CRANE_STATE_H_

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void initCrane(comms_layer_t* radio, am_addr_t my_addr);
void initCraneLoc();

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void craneReceiveMessage (comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/


#endif //CRANE_STATE_H_
