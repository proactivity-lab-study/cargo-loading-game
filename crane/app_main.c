/**
 *
 * Copyright Thinnect Inc. 2019
 * Copyright to modifications Proactivity Lab 2020
 *
 * @license MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "retargetserial.h"

#include "cmsis_os2.h"

#include "platform.h"

#include "SignatureArea.h"
#include "DeviceSignature.h"

#include "loggers_ext.h"
#include "logger_fwrite.h"

#include "DeviceSignature.h"
#include "mist_comm_am.h"
#include "radio.h"

#include "endianness.h"

#include "loglevels.h"
#define __MODUUL__ "main"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

// Include the information header binary
#include "incbin.h"
INCBIN(Header, "header.bin");

#include "game_state.h"
#include "crane_state.h"
#include "clg_comm.h"

static void radio_start_done (comms_layer_t * comms, comms_status_t status, void * user)
{
    debug("radio started %d", status);
}

// Perform basic radio setup
static comms_layer_t* radio_setup (am_addr_t node_addr)
{
    static comms_receiver_t rcvr, rcvr2;
    comms_layer_t * radio = radio_init(DEFAULT_RADIO_CHANNEL, 0x22, node_addr);
    if (NULL == radio)
    {
        return NULL;
    }

    if (COMMS_SUCCESS != comms_start(radio, radio_start_done, NULL))
    {
        return NULL;
    }

    // Wait for radio to start, could use osTreadFlagWait and set from callback
    while(COMMS_STARTED != comms_status(radio))
    {
        osDelay(1);
    }

    comms_register_recv(radio, &rcvr, crane_receive_message, NULL, AMID_CRANECOMMUNICATION);
	comms_register_recv(radio, &rcvr2, system_receive_message, NULL, AMID_SYSTEMCOMMUNICATION);
    debug1("radio rdy");
    return radio;
}

// Setup loop - init radio, crane and game, print heartbeat
void setup_loop (void * arg)
{
    am_addr_t node_addr = DEFAULT_AM_ADDR;
    uint8_t node_eui[8];

    // Initialize node signature - get address and EUI64
    if (SIG_GOOD == sigInit())
    {
        node_addr = sigGetNodeId();
        sigGetEui64(node_eui);
        infob1("ADDR:%"PRIX16" EUI64:", node_eui, sizeof(node_eui), node_addr);
    }
    else
    {
        warn1("ADDR:%"PRIX16, node_addr); // Falling back to default addr
    }

    // initialize radio
    comms_layer_t* radio = radio_setup(node_addr);
    if (NULL == radio)
    {
        err1("radio");
        for (;;); // panic
    }

	init_crane(radio);
	init_system(radio);

    // Loop forever
    for (;;)
    {
        osDelay(60*osKernelGetTickFreq()); // 60 secondss
		info("HB"); //heartbeat
    }
}

int logger_fwrite_boot (const char *ptr, int len)
{
    fwrite(ptr, len, 1, stdout);
    fflush(stdout);
    return len;
}

int main ()
{
    PLATFORM_Init();

    // LEDs
    PLATFORM_LedsInit();

    // Configure debug output
    RETARGET_SerialInit();
    log_init(BASE_LOG_LEVEL, &logger_fwrite_boot, NULL);

    info1("Cargo loading game "VERSION_STR" (%d.%d.%d)", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

	PLATFORM_RadioInit(); // Radio GPIO/PRS - LNA on some MGM12P

    // Initialize OS kernel
    osKernelInitialize();

    // Create a thread
    const osThreadAttr_t setup_thread_attr = { .name = "setup" };
    osThreadNew(setup_loop, NULL, &setup_thread_attr);

    if (osKernelReady == osKernelGetState())
    {
        // Switch to a thread-safe logger
        logger_fwrite_init();
        log_init(BASE_LOG_LEVEL, &logger_fwrite, NULL);

        // Start the kernel
        osKernelStart();
    }
    else
    {
        err1("!osKernelReady");
    }

    for(;;);
}