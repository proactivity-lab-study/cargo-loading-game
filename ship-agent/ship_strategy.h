/**
 *
 * Copyright Proactivity Lab 2020
 *
 * @license MIT
 */


#ifndef SHIP_STRATEGY_H_
#define SHIP_STRATEGY_H_

/**********************************************************************************************
 *	Initialise module
 **********************************************************************************************/

void init_ship_strategy(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ship_receive_message(comms_layer_t* comms, const comms_msg_t* msg, void* user);

#endif //SHIP_STRATEGY_H_
