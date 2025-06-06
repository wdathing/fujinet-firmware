
#include "keys.h"

#if CONFIG_IDF_TARGET_ESP32S3 != y
#include <esp32/himem.h>
#endif

#include "../../include/debug.h"
#include "../../include/pinmap.h"

#include "fnSystem.h"
#include "fnConfig.h"
#include "fnWiFi.h"
#include "fnBluetooth.h"
#include "fuji.h"

#include "led.h"

// Global KeyManager object
KeyManager fnKeyManager;

static int mButtonPin[eKey::KEY_COUNT] = {PIN_BUTTON_A, PIN_BUTTON_B, PIN_BUTTON_C};

void KeyManager::setup()
{
#ifdef PINMAP_ESP32S3

    if (PIN_BUTTON_A != GPIO_NUM_NC)
    	fnSystem.set_pin_mode(PIN_BUTTON_A, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
    else
        _keys[eKey::BUTTON_A].disabled = true;

    if (PIN_BUTTON_B != GPIO_NUM_NC)
        fnSystem.set_pin_mode(PIN_BUTTON_B, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
    else
        _keys[eKey::BUTTON_B].disabled = true;

    if (PIN_BUTTON_C != GPIO_NUM_NC)
        fnSystem.set_pin_mode(PIN_BUTTON_C, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
    else
        _keys[eKey::BUTTON_C].disabled = true;

#else /* PINMAP_ESP32S3 */
    mButtonPin[eKey::BUTTON_A] = PIN_BUTTON_A;
    mButtonPin[eKey::BUTTON_B] = PIN_BUTTON_B;
    mButtonPin[eKey::BUTTON_C] = fnSystem.get_safe_reset_gpio();

#   ifdef NO_BUTTONS
    _keys[eKey::BUTTON_A].disabled = true;
    _keys[eKey::BUTTON_B].disabled = true;
    _keys[eKey::BUTTON_C].disabled = true;
    Debug_println("NO_BUTTONS: disabled all buttons");
#   else
    if (PIN_BUTTON_A == GPIO_NUM_NC)
    {
        _keys[eKey::BUTTON_A].disabled = true;
#ifndef BUILD_COCO
        Debug_printf("Button A Disabled\r\n");
#endif        
    }
    else
    {
        fnSystem.set_pin_mode(PIN_BUTTON_A, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
#ifndef BUILD_COCO
        Debug_printf("Button A Enabled on IO%d\r\n",mButtonPin[eKey::BUTTON_A]);
#endif
    }

    if (PIN_BUTTON_B == GPIO_NUM_NC)
    {
        _keys[eKey::BUTTON_B].disabled = true;
#ifndef BUILD_COCO
        Debug_printf("Button B Disabled\r\n");
#endif
    }
    else
    {
        fnSystem.set_pin_mode(PIN_BUTTON_B, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
#ifndef BUILD_COCO
        Debug_printf("Button B Enabled on IO%d\r\n", mButtonPin[eKey::BUTTON_B]);
#endif
    }

    if (fnSystem.get_safe_reset_gpio() == GPIO_NUM_NC)
    {
        _keys[eKey::BUTTON_C].disabled = true;
#ifndef BUILD_COCO
        Debug_printf("Button C (Safe Reset) Disabled\r\n");
#endif
    }
    else
    {
#   ifdef BUILD_APPLE
        // Rev00 has no pullup for Button C
        if (fnSystem.get_hardware_ver() == 1)
            fnSystem.set_pin_mode(PIN_BUTTON_C, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_UP);
        else
            fnSystem.set_pin_mode(PIN_BUTTON_C, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_NONE);
#   else
        fnSystem.set_pin_mode(PIN_BUTTON_C, gpio_mode_t::GPIO_MODE_INPUT, SystemManager::pull_updown_t::PULL_NONE);
#   endif
#ifndef BUILD_COCO
        Debug_printf("Button C (Safe Reset) Enabled on IO%d\r\n", mButtonPin[eKey::BUTTON_C]);
#endif
    }

#   endif /* NO_BUTTONS */
#endif /* PINMAP_ESP32S3 */

    // Start a new task to check the status of the buttons
    #define KEYS_STACKSIZE 4096
    #define KEYS_PRIORITY 1
    xTaskCreate(_keystate_task, "fnKeys", KEYS_STACKSIZE, this, KEYS_PRIORITY, nullptr);
}


// Ignores the current key press
void KeyManager::ignoreKeyPress(eKey key)
{
    _keys[key].action_started_ms = IGNORE_KEY_EVENT;
}

bool KeyManager::keyCurrentlyPressed(eKey key)
{
    // Ignore disabled buttons
    if(_keys[key].disabled)
        return false;

    return fnSystem.digital_read(mButtonPin[key]) == DIGI_LOW;
}

/*
There are 3 types of actions we're looking for:
   * LONG_PRESS: User holds button for at least LONGPRESS_TIME
   * SHORT_PRESS: User presses button and releases in less than LONGPRESS_TIME,
   *              and there isn't another event for DOUBLETAP_DETECT_TIME
   * DOUBLE_TAP: User presses button and releases in less than LONGPRESS_TIME,
   *             and the last SHORT_PRESS was within DOUBLETAP_DETECT_TIME
*/
eKeyStatus KeyManager::getKeyStatus(eKey key)
{
    eKeyStatus result = eKeyStatus::INACTIVE;

    // Ignore disabled buttons
    if(_keys[key].disabled)
        return eKeyStatus::DISABLE;

    unsigned long ms = fnSystem.millis();

    // Button is PRESSED when DIGI_LOW
    if (fnSystem.digital_read(mButtonPin[key]) == DIGI_LOW)
    {
        // If not already active, mark as ACTIVE and note the time
        if (_keys[key].active == false)
        {
            _keys[key].active = true;
            _keys[key].action_started_ms = ms;
        }
        // Detect long-press when time runs out instead of waiting for release
        else
        {
            // Check time elapsed and confirm that we didn't set the start time IGNORE
            if (ms - _keys[key].action_started_ms > LONGPRESS_TIME && _keys[key].action_started_ms != IGNORE_KEY_EVENT)
            {
                result = eKeyStatus::LONG_PRESS;
                // Indicate we ignore further activity until the button is released
                _keys[key].action_started_ms = IGNORE_KEY_EVENT;
            }

        }

    }
    // Button is NOT pressed when DIGI_HIGH
    else
    {
        // If we'd previously marked the key as active
        if (_keys[key].active == true)
        {
            // Since the button has been released, mark it as inactive
            _keys[key].active = false;

            // If we're not supposed to ignore this, it must be a press-and-release event
            if(_keys[key].action_started_ms != IGNORE_KEY_EVENT)
            {
                // If the last SHORT_PRESS was within DOUBLETAP_DETECT_TIME, immediately return a DOUBLETAP event
                if(ms - _keys[key].last_tap_ms < DOUBLETAP_DETECT_TIME)
                {
                    _keys[key].last_tap_ms = 0; // Reset this so we don't keep counting it
                    result = eKeyStatus::DOUBLE_TAP;
                }
                // Otherwise just store when this event happened so we can check for it later
                else
                {
                    _keys[key].last_tap_ms = ms;
                }
            }
        }
        // If there's a last SHORT_PRESS time recorded, see if DOUBLETAP_DETECT_TIME has elapsed
        else
        {
            if(_keys[key].last_tap_ms != 0 && ms - _keys[key].last_tap_ms > DOUBLETAP_DETECT_TIME)
            {
                _keys[key].last_tap_ms = 0; // Reset this so we don't keep counting it
                result = eKeyStatus::SHORT_PRESS;
            }
        }

    }

    return result;
}

void KeyManager::_keystate_task(void *param)
{
#ifndef NO_BUTTONS
    #define BLUETOOTH_LED eLed::LED_BT

    KeyManager *pKM = (KeyManager *)param;

#if defined(BUILD_LYNX) || defined(BUILD_APPLE) || defined(BUILD_RS232) || defined(BUILD_MAC)
    // No button B onboard
    pKM->_keys[eKey::BUTTON_B].disabled = true;
#endif

    while (true)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // Check on the status of the BUTTON_A and do something useful
        switch (pKM->getKeyStatus(eKey::BUTTON_A))
        {
        case eKeyStatus::LONG_PRESS:
            Debug_println("BUTTON_A: LONG PRESS");

#ifdef BLUETOOTH_SUPPORT
            Debug_println("ACTION: Bluetooth toggle");

            if (fnBtManager.isActive())
            {
                fnBtManager.stop();
                fnLedManager.set(BLUETOOTH_LED, false);

                // Start WiFi and connect
                fnWiFi.start();

                // Save Bluetooth status in fnConfig
                Config.store_bt_status(false); // Disabled
                Config.save();
            }
            else
            {
                // Stop WiFi
                fnWiFi.stop();

                fnLedManager.set(BLUETOOTH_LED, true); // BT LED ON
                fnBtManager.start();

                // Save Bluetooth status in fnConfig
                Config.store_bt_status(true); // Enabled
                Config.save();
            }
#endif //BLUETOOTH_SUPPORT
#ifdef BUILD_MAC
            Debug_println("ACTION: Mount all disks");
            theFuji.mount_all();
#endif /* BUILD_MAC */

            break;

        case eKeyStatus::SHORT_PRESS:
            Debug_println("BUTTON_A: SHORT PRESS");
#ifdef PARALLEL_BUS
            // Reset the Commodore via Userport, GPIO 13
            fnSystem.set_pin_mode(GPIO_NUM_13, gpio_mode_t::GPIO_MODE_OUTPUT, SystemManager::pull_updown_t::PULL_UP);
            fnSystem.digital_write(GPIO_NUM_13, DIGI_LOW);
            fnSystem.delay(1);
            fnSystem.digital_write(GPIO_NUM_13, DIGI_HIGH);
            Debug_println("Sent RESET signal to Commodore");
#endif

#if defined(PINMAP_A2_REV0) || defined(PINMAP_FUJILOAF_REV0)
            fnLedManager.blink(LED_BUS, 2); // blink to confirm a button press
            // IEC.releaseLines();
            Debug_printf("Heap: %lu\r\n",esp_get_free_internal_heap_size());
            // Debug_printf("PsramSize: %u\r\n", fnSystem.get_psram_size());
            // Debug_printf("himem phys: %u\r\n", esp_himem_get_phys_size());
            // Debug_printf("himem free: %u\r\n", esp_himem_get_free_size());
            // Debug_printf("himem reserved: %u\r\n", esp_himem_reserved_area_size());
#endif // PINMAP_A2_REV0

// Either toggle BT baud rate or do a disk image rotation on B_KEY SHORT PRESS
#ifdef BLUETOOTH_SUPPORT
            if (fnBtManager.isActive())
            {
                Debug_println("ACTION: Bluetooth baud rate toggle");
                fnBtManager.toggleBaudrate();
            }
            else
#endif
            {
#ifdef BUILD_ATARI
                Debug_println("ACTION: Send image_rotate message to SIO queue");
                sio_message_t msg;
                msg.message_id = SIOMSG_DISKSWAP;
                xQueueSend(SIO.qSioMessages, &msg, 0);
                fnLedManager.blink(BLUETOOTH_LED, 2); // blink to confirm a button press
#endif /* BUILD_ATARI */
#ifdef BUILD_ADAM
                Debug_println("ACTION: Send image_rotate message to SIO queue");
                adamnet_message_t msg;
                msg.message_id = ADAMNETMSG_DISKSWAP;
                xQueueSend(AdamNet.qAdamNetMessages, &msg, 0);
#endif /* BUILD_ADAM*/ 
            }
            break;

        case eKeyStatus::DOUBLE_TAP:
            Debug_println("BUTTON_A: DOUBLE-TAP");
            fnSystem.debug_print_tasks();
            break;

        default:
            break;
        } // BUTTON_A

        // Check on the status of the BUTTON_B and do something useful
        switch (pKM->getKeyStatus(eKey::BUTTON_B))
        {
        case eKeyStatus::LONG_PRESS:
            // Check if we're with a few seconds of booting and disable this button if so -
            // assume the button is stuck/disabled/non-existant
            if(fnSystem.millis() < 3000)
            {
                Debug_println("BUTTON_B: SEEMS STUCK - DISABLING");
                pKM->_keys[eKey::BUTTON_B].disabled = true;
                break;
            }

            Debug_println("BUTTON_B: LONG PRESS");
            Debug_println("ACTION: Reboot");
            fnSystem.reboot();
            break;

        case eKeyStatus::SHORT_PRESS:
            Debug_println("BUTTON_B: SHORT PRESS");
#ifdef BUILD_ATARI
            Debug_printv("Free Internal Heap: %lu\nFree Total Heap: %lu",esp_get_free_internal_heap_size(),esp_get_free_heap_size());
#endif /* BUILD_ATARI */
            break;
        case eKeyStatus::DOUBLE_TAP:
            Debug_println("BUTTON_B: DOUBLE-TAP");
            fnSystem.debug_print_tasks();
            break;

        default:
            break;
        } // BUTTON_B

        // Check on the status of the BUTTON_C and do something useful
        switch (pKM->getKeyStatus(eKey::BUTTON_C))
        {
        case eKeyStatus::LONG_PRESS:
            Debug_println("BUTTON_C: LONG PRESS");
            break;

        case eKeyStatus::SHORT_PRESS:
            Debug_println("BUTTON_C: SHORT PRESS");
            Debug_println("ACTION: Reboot");
            fnSystem.reboot();
            break;

        case eKeyStatus::DOUBLE_TAP:
            Debug_println("BUTTON_C: DOUBLE-TAP");
            break;

        default:
            break;
        } // BUTTON_C
    }
#else
    while (1) {vTaskDelay(1000);};

#endif /* NO_BUTTON */
}
