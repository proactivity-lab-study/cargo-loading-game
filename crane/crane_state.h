/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef CRANE_STATE_H_
#define CRANE_STATE_H_

#define GRID_LOWER_BOUND 2 //including
#define GRID_UPPER_BOUND 30 //including


/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_crane(comms_layer_t* radio);
void init_crane_loc();

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void crane_receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user);

/**********************************************************************************************
 *	Utility functions
 **********************************************************************************************/


#endif //CRANE_STATE_H_
