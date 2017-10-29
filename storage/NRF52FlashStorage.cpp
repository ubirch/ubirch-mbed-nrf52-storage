/**
 ******************************************************************************
 * @file    FlashStorage.cpp
 * @author  Waldemar Gruenwald
 * @version V1.0.0
 * @date    23 October 2017
 * @brief   key-storage class implementation
 *
 * @update 	V1.0.1
 * 			08 August 2017
 * 			error handling and debug
 ******************************************************************************
 *
 * Copyright 2016 ubirch GmbH (https://ubirch.com)
 *
 * ```
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ```
 */

/**
 *  For more information about the key-storage,
 *  see documentation for mbed fstorage
 */
#include "FlashStorage.h"
#include <nrf_soc.h>
#include <BLE.h>
#include <EventQueue.h>
#include "NRF52FlashStorage.h"

#include "mbed.h"

extern "C" {
#include <softdevice_handler.h>

}


#define PRINTF(...)
//#define PRINTF printf

// TODO, maybe separate the storage into two different storages, for secure and not secure data

static Thread *sdEventThread;//(osPriorityNormal);
static EventQueue *sdEventQueue;

static void scheduleBleSdEventsProcessing(BLE::OnEventsToProcessCallbackContext *context) {
    sdEventQueue->call(Callback<void()>(&context->ble, &BLE::processEvents));
}

static void initBleSdComplete(BLE::InitializationCompleteCallbackContext *params) {
    BLE &ble = params->ble;
    ble_error_t error = params->error;

    if (error != BLE_ERROR_NONE) {
        PRINTF("SD BLE error \r\n");
        return;
    }

    if (ble.getInstanceID() != BLE::DEFAULT_INSTANCE) {
        return;
    }
}

/*
* flag for the callback, used to determine, when event handling is finished
* so the rest of the program can continue
*/
static uint8_t fs_callback_flag;


inline static void fs_evt_handler(fs_evt_t const *const evt, fs_ret_t result) {
    (void) evt;
    if (result != FS_SUCCESS) {
        PRINTF("    fstorage event handler ERROR   \r\n");
    } else {
        fs_callback_flag = 0;
    }
};

/*
 * set the configuration
 */
FS_REGISTER_CFG(fs_config_t fs_config) =
        {
                .p_start_addr = 0,                // DUMMY
                .p_end_addr = (const uint32_t *) PAGE_SIZE_WORDS,    // DUMMY
                .callback  = fs_evt_handler, // Function for event callbacks.
                .num_pages = NUM_PAGES,      // Number of physical flash pages required.
                .priority  = 0xFE            // Priority for flash usage. // TODO check priority
        };


ble_error_t NRF52FlashStorage::initBleSd() {
    if (softdevice_handler_isEnabled()) {
        PRINTF("BLE is active \r\n");
        return BLE_ERROR_NONE;
    } else {
        PRINTF("BLE not active \r\n");
        activatedSdStatus = true;
        sdEventQueue = new EventQueue(/* event count */ 4 * EVENTS_EVENT_SIZE);
        sdEventThread = new Thread(osPriorityNormal);
        sdEventThread->start(callback(sdEventQueue, &EventQueue::dispatch_forever));
        BLE &ble = BLE::Instance();
        ble.onEventsToProcess(scheduleBleSdEventsProcessing);
        return ble.init(initBleSdComplete);
    }
}


void NRF52FlashStorage::deinitBleSd() {
    if (activatedSdStatus) {
        activatedSdStatus = false;
        if (BLE::Instance().hasInitialized()) {
            BLE::Instance().shutdown();
            sdEventThread->terminate();
            sdEventThread->join();
            delete sdEventThread;
            delete sdEventQueue;
        }
    }
}


bool NRF52FlashStorage::init() {
    /*
     * initialize the storage and check for success
     */
    fs_ret_t ret = fs_init();
    if (ret != FS_SUCCESS) {
        PRINTF("    fstorage INITIALIZATION ERROR    \r\n");
        return false;
    } else {
        PRINTF("    fstorage INITIALIZATION successful    \r\n");
        return true;
    }
}


bool NRF52FlashStorage::readData(uint32_t p_location, unsigned char *buffer, uint16_t length8) {
    if (buffer == NULL || length8 == 0) {
        return false;
    }

    // determine the real location alligned to 32bit (4Byte) values
    uint8_t preLength = (uint8_t) (p_location % 4);
    uint32_t locationReal = p_location - preLength;
    // determine the required length, considering the preLength
    uint16_t lengthReal = (uint16_t) (length8 + preLength);
    if (lengthReal % 4) {
        lengthReal += 4 - (lengthReal % 4);
    }

    uint16_t length32 = lengthReal >> 2;
    uint32_t buf32[length32];
    PRINTF("Data read from flash address 0x%X: ", ((uint32_t) fs_config.p_start_addr) + locationReal);
    for (uint16_t i = 0; i < length32; i++) {
        buf32[i] = *(fs_config.p_start_addr + (locationReal >> 2) + i);
        PRINTF("(%u)[0x%X] = %X \r\n", i, (fs_config.p_start_addr + (locationReal >> 2) + i), buf32[i]);
    }
    PRINTF("\r\n");

    // create a new buffer, to fill all the data and convert the data into it
    unsigned char bufferReal[lengthReal];
    if (!conv32to8(buf32, bufferReal, lengthReal)) {
        return false;            // ERROR
    }

    // shift the data to the right position
    for (int j = 0; j < length8; j++) {
        buffer[j] = bufferReal[j + preLength];
    }
    return true;
}


bool NRF52FlashStorage::erasePage(uint8_t page, uint8_t numPages) {
    // Erase one page (page 0).
    PRINTF("Erasing a flash page at address 0x%X\r\n", (uint32_t) (fs_config.p_start_addr + (PAGE_SIZE_WORDS * page)));

    this->initBleSd();

    fs_callback_flag = 1;
    fs_ret_t ret = fs_erase(&fs_config, fs_config.p_start_addr + (PAGE_SIZE_WORDS * page), numPages);
    if (ret != FS_SUCCESS) {
        PRINTF("    fstorage ERASE ERROR    \r\n");
        return false;
    } else {
        PRINTF("    fstorage ERASE successful    \r\n");
    }
    while (fs_callback_flag == 1) /* do nothing */ ;

    this->deinitBleSd();
    return true;
}


bool NRF52FlashStorage::writeData(uint32_t p_location, const unsigned char *buffer, uint16_t length8) {
    if (buffer == NULL || length8 == 0) {
        PRINTF("ERROR NULL  \r\n");
        return false;
    }

    // determine the real location alligned to 32bit (4Byte) values
    uint8_t preLength = (uint8_t) (p_location % 4);
    uint32_t locationReal = p_location - preLength;
    // determine the required length, considering the preLength
    uint16_t lengthReal = (uint16_t) (length8 + preLength);
    if (lengthReal % 4) {
        lengthReal += 4 - (lengthReal % 4);
    }

    unsigned char bufferReal[lengthReal];

    // check, if there is already data in the buffer
    this->readData(locationReal, bufferReal, lengthReal);
    for (int i = preLength; i < (preLength + length8); ++i) {
        if (bufferReal[i] != 0xFF) {
            PRINTF("ERROR FLASH NOT EMPTY \r\n");
            return false;
        }
    }
    // shift the data in the buffer to the right location
    // and fill the reminding bytes with 0xFF to not overwrite existing data in the memory
    for (int j = 0; j < preLength; j++) {
        bufferReal[j] = 0xFF;
    }
    for (int k = 0; k < length8; k++) {
        bufferReal[k + preLength] = buffer[k];
    }
    for (int l = (preLength + length8); l < lengthReal; l++) {
        bufferReal[l] = 0xFF;
    }

    // convert the data into 32 bit array
    uint16_t length32 = lengthReal >> 2;
    uint32_t buf32[length32];
    if (!this->conv8to32(bufferReal, buf32, lengthReal)) {
        PRINTF("ERROR CONVERSION \r\n");
        return false;            // ERROR
    }

    //Write the buffer into flash
    PRINTF("Writing data to addr =[0x%X], num =[0x%X], data =[0x ", ((uint32_t) fs_config.p_start_addr + locationReal),
           lengthReal);
    for (int m = 0; m < length32; m++) {
        PRINTF(" %X", buf32[m]);
    }
    PRINTF("]\r\n");

    fs_ret_t ret;
    this->initBleSd();

    fs_callback_flag = 1;
    ret = fs_store(&fs_config, (fs_config.p_start_addr + (locationReal >> 2)), buf32,
                   length32);      //Write data to memory address 0x0003F000. Check it with command: nrfjprog --memrd 0x0003F000 --n 16
    if (ret != FS_SUCCESS) {
        PRINTF("    fstorage WRITE ERROR    \r\n");
        return false;
    } else {
        PRINTF("    fstorage WRITE successful    \r\n");
    }
    while (fs_callback_flag == 1) /* do nothing */;

    this->deinitBleSd();
    return true;
}

uint32_t NRF52FlashStorage::getStartAddress() {
    return (uint32_t) (fs_config.p_start_addr);
}

uint32_t NRF52FlashStorage::getEndAddress() {
    return (uint32_t) (fs_config.p_end_addr);
}

