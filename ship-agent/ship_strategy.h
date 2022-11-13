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

void initShipStrategy(comms_layer_t* radio, am_addr_t addr);

/**********************************************************************************************
 *	Message receiving
 **********************************************************************************************/

void ship2ShipReceiveMessage(comms_layer_t* comms, const comms_msg_t* msg, void* user);

typedef enum 
{
    BEN_OR_WFLAGS_CRANE     = 1,
    BEN_OR_WFLAGS_MSGONE    = 2,
    BEN_OR_WFLAGS_MSGTWO    = 3
}benor_proto_wflags_t;

void notifyNewCraneRound();

#endif //SHIP_STRATEGY_H_
