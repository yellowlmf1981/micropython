/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2018 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "lib/mp-readline/readline.h"
#include "lib/utils/pyexec.h"
#include "lib/oofatfs/ff.h"
#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"

#if MICROPY_PY_LWIP
#include "lwip/init.h"
#include "drivers/cyw43/cyw43.h"
#endif

#include "systick.h"
#include "pendsv.h"
#include "pybthread.h"
#include "gccollect.h"
#include "factoryreset.h"
#include "modmachine.h"
#include "i2c.h"
#include "spi.h"
#include "uart.h"
#include "timer.h"
#include "led.h"
#include "pin.h"
#include "extint.h"
#include "usrsw.h"
#include "usb.h"
#include "rtc.h"
#include "storage.h"
#include "sdcard.h"
#include "sdram.h"
#include "rng.h"
#include "accel.h"
#include "servo.h"
#include "dac.h"
#include "can.h"
#include "modnetwork.h"

void SystemClock_Config(void);

#if MICROPY_PY_THREAD
STATIC pyb_thread_t pyb_thread_main;
#endif

#if MICROPY_HW_ENABLE_STORAGE
STATIC fs_user_mount_t fs_user_mount_flash;
#endif

#if defined(MICROPY_HW_UART_REPL)
#ifndef MICROPY_HW_UART_REPL_RXBUF
#define MICROPY_HW_UART_REPL_RXBUF (260)
#endif
STATIC pyb_uart_obj_t pyb_uart_repl_obj;
STATIC uint8_t pyb_uart_repl_rxbuf[MICROPY_HW_UART_REPL_RXBUF];
#endif

void flash_error(int n) {
    for (int i = 0; i < n; i++) {
        led_state(PYB_LED_RED, 1);
        led_state(PYB_LED_GREEN, 0);
        mp_hal_delay_ms(250);
        led_state(PYB_LED_RED, 0);
        led_state(PYB_LED_GREEN, 1);
        mp_hal_delay_ms(250);
    }
    led_state(PYB_LED_GREEN, 0);
}

void NORETURN __fatal_error(const char *msg) {
    for (volatile uint delay = 0; delay < 10000000; delay++) {
    }
    led_state(1, 1);
    led_state(2, 1);
    led_state(3, 1);
    led_state(4, 1);
    mp_hal_stdout_tx_strn("\nFATAL ERROR:\n", 14);
    mp_hal_stdout_tx_strn(msg, strlen(msg));
    for (uint i = 0;;) {
        led_toggle(((i++) & 3) + 1);
        for (volatile uint delay = 0; delay < 10000000; delay++) {
        }
        if (i >= 16) {
            // to conserve power
            __WFI();
        }
    }
}

void nlr_jump_fail(void *val) {
    printf("FATAL: uncaught exception %p\n", val);
    mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(val));
    __fatal_error("");
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)func;
    printf("Assertion '%s' failed, at file %s:%d\n", expr, file, line);
    __fatal_error("");
}
#endif

STATIC mp_obj_t pyb_main(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_opt, MP_ARG_INT, {.u_int = 0} }
    };

    if (mp_obj_is_str(pos_args[0])) {
        MP_STATE_PORT(pyb_config_main) = pos_args[0];

        // parse args
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
        MP_STATE_VM(mp_optimise_value) = args[0].u_int;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(pyb_main_obj, 1, pyb_main);

#if MICROPY_HW_ENABLE_STORAGE
// avoid inlining to avoid stack usage within main()
MP_NOINLINE STATIC bool init_flash_fs(uint reset_mode) {
    // init the vfs object
    fs_user_mount_t *vfs_fat = &fs_user_mount_flash;
    vfs_fat->flags = 0;
    pyb_flash_init_vfs(vfs_fat);

    // try to mount the flash
    FRESULT res = f_mount(&vfs_fat->fatfs);

    if (reset_mode == 3 || res == FR_NO_FILESYSTEM) {
        // no filesystem, or asked to reset it, so create a fresh one

        // LED on to indicate creation of LFS
        led_state(PYB_LED_GREEN, 1);
        uint32_t start_tick = HAL_GetTick();

        uint8_t working_buf[FF_MAX_SS];
        res = f_mkfs(&vfs_fat->fatfs, FM_FAT, 0, working_buf, sizeof(working_buf));
        if (res == FR_OK) {
            // success creating fresh LFS
        } else {
            printf("MPY: can't create flash filesystem\n");
            return false;
        }

        // set label
        f_setlabel(&vfs_fat->fatfs, MICROPY_HW_FLASH_FS_LABEL);

        // populate the filesystem with factory files
        factory_reset_make_files(&vfs_fat->fatfs);

        // keep LED on for at least 200ms
        systick_wait_at_least(start_tick, 200);
        led_state(PYB_LED_GREEN, 0);
    } else if (res == FR_OK) {
        // mount sucessful
    } else {
    fail:
        printf("MPY: can't mount flash\n");
        return false;
    }

    // mount the flash device (there should be no other devices mounted at this point)
    // we allocate this structure on the heap because vfs->next is a root pointer
    mp_vfs_mount_t *vfs = m_new_obj_maybe(mp_vfs_mount_t);
    if (vfs == NULL) {
        goto fail;
    }
    vfs->str = "/flash";
    vfs->len = 6;
    vfs->obj = MP_OBJ_FROM_PTR(vfs_fat);
    vfs->next = NULL;
    MP_STATE_VM(vfs_mount_table) = vfs;

    // The current directory is used as the boot up directory.
    // It is set to the internal flash filesystem by default.
    MP_STATE_PORT(vfs_cur) = vfs;

    return true;
}
#endif

#if MICROPY_HW_SDCARD_MOUNT_AT_BOOT
STATIC bool init_sdcard_fs(void) {
    bool first_part = true;
    for (int part_num = 1; part_num <= 4; ++part_num) {
        // create vfs object
        fs_user_mount_t *vfs_fat = m_new_obj_maybe(fs_user_mount_t);
        mp_vfs_mount_t *vfs = m_new_obj_maybe(mp_vfs_mount_t);
        if (vfs == NULL || vfs_fat == NULL) {
            break;
        }
        vfs_fat->flags = FSUSER_FREE_OBJ;
        sdcard_init_vfs(vfs_fat, part_num);

        // try to mount the partition
        FRESULT res = f_mount(&vfs_fat->fatfs);

        if (res != FR_OK) {
            // couldn't mount
            m_del_obj(fs_user_mount_t, vfs_fat);
            m_del_obj(mp_vfs_mount_t, vfs);
        } else {
            // mounted via FatFs, now mount the SD partition in the VFS
            if (first_part) {
                // the first available partition is traditionally called "sd" for simplicity
                vfs->str = "/sd";
                vfs->len = 3;
            } else {
                // subsequent partitions are numbered by their index in the partition table
                if (part_num == 2) {
                    vfs->str = "/sd2";
                } else if (part_num == 2) {
                    vfs->str = "/sd3";
                } else {
                    vfs->str = "/sd4";
                }
                vfs->len = 4;
            }
            vfs->obj = MP_OBJ_FROM_PTR(vfs_fat);
            vfs->next = NULL;
            for (mp_vfs_mount_t **m = &MP_STATE_VM(vfs_mount_table);; m = &(*m)->next) {
                if (*m == NULL) {
                    *m = vfs;
                    break;
                }
            }

            #if MICROPY_HW_ENABLE_USB
            if (pyb_usb_storage_medium == PYB_USB_STORAGE_MEDIUM_NONE) {
                // if no USB MSC medium is selected then use the SD card
                pyb_usb_storage_medium = PYB_USB_STORAGE_MEDIUM_SDCARD;
            }
            #endif

            #if MICROPY_HW_ENABLE_USB
            // only use SD card as current directory if that's what the USB medium is
            if (pyb_usb_storage_medium == PYB_USB_STORAGE_MEDIUM_SDCARD)
            #endif
            {
                if (first_part) {
                    // use SD card as current directory
                    MP_STATE_PORT(vfs_cur) = vfs;
                }
            }
            first_part = false;
        }
    }

    if (first_part) {
        printf("MPY: can't mount SD card\n");
        return false;
    } else {
        return true;
    }
}
#endif

#if !MICROPY_HW_USES_BOOTLOADER
STATIC uint update_reset_mode(uint reset_mode) {
    #if MICROPY_HW_HAS_SWITCH
    if (switch_get()) {

        // The original method used on the pyboard is appropriate if you have 2
        // or more LEDs.
        #if defined(MICROPY_HW_LED2)
        for (uint i = 0; i < 3000; i++) {
            if (!switch_get()) {
                break;
            }
            mp_hal_delay_ms(20);
            if (i % 30 == 29) {
                if (++reset_mode > 3) {
                    reset_mode = 1;
                }
                led_state(2, reset_mode & 1);
                led_state(3, reset_mode & 2);
                led_state(4, reset_mode & 4);
            }
        }
        // flash the selected reset mode
        for (uint i = 0; i < 6; i++) {
            led_state(2, 0);
            led_state(3, 0);
            led_state(4, 0);
            mp_hal_delay_ms(50);
            led_state(2, reset_mode & 1);
            led_state(3, reset_mode & 2);
            led_state(4, reset_mode & 4);
            mp_hal_delay_ms(50);
        }
        mp_hal_delay_ms(400);

        #elif defined(MICROPY_HW_LED1)

        // For boards with only a single LED, we'll flash that LED the
        // appropriate number of times, with a pause between each one
        for (uint i = 0; i < 10; i++) {
            led_state(1, 0);
            for (uint j = 0; j < reset_mode; j++) {
                if (!switch_get()) {
                    break;
                }
                led_state(1, 1);
                mp_hal_delay_ms(100);
                led_state(1, 0);
                mp_hal_delay_ms(200);
            }
            mp_hal_delay_ms(400);
            if (!switch_get()) {
                break;
            }
            if (++reset_mode > 3) {
                reset_mode = 1;
            }
        }
        // Flash the selected reset mode
        for (uint i = 0; i < 2; i++) {
            for (uint j = 0; j < reset_mode; j++) {
                led_state(1, 1);
                mp_hal_delay_ms(100);
                led_state(1, 0);
                mp_hal_delay_ms(200);
            }
            mp_hal_delay_ms(400);
        }
        #else
        #error Need a reset mode update method
        #endif
    }
    #endif
    return reset_mode;
}
#endif

void stm32_main(uint32_t reset_mode) {
    // Enable caches and prefetch buffers

    #if defined(STM32F4)

    #if INSTRUCTION_CACHE_ENABLE
    __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
    #endif
    #if DATA_CACHE_ENABLE
    __HAL_FLASH_DATA_CACHE_ENABLE();
    #endif
    #if PREFETCH_ENABLE
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    #endif

    #elif defined(STM32F7) || defined(STM32H7)

    #if ART_ACCLERATOR_ENABLE
    __HAL_FLASH_ART_ENABLE();
    #endif

    SCB_EnableICache();
    SCB_EnableDCache();

    #elif defined(STM32L4)

    #if !INSTRUCTION_CACHE_ENABLE
    __HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
    #endif
    #if !DATA_CACHE_ENABLE
    __HAL_FLASH_DATA_CACHE_DISABLE();
    #endif
    #if PREFETCH_ENABLE
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    #endif

    #endif

    #if __CORTEX_M >= 0x03
    // Set the priority grouping
    NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
    #endif

    // SysTick is needed by HAL_RCC_ClockConfig (called in SystemClock_Config)
    HAL_InitTick(TICK_INT_PRIORITY);

    // set the system clock to be HSE
    SystemClock_Config();

    // enable GPIO clocks
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    #if defined(GPIOD)
    __HAL_RCC_GPIOD_CLK_ENABLE();
    #endif

    #if defined(STM32F4) ||  defined(STM32F7)
        #if defined(__HAL_RCC_DTCMRAMEN_CLK_ENABLE)
        // The STM32F746 doesn't really have CCM memory, but it does have DTCM,
        // which behaves more or less like normal SRAM.
        __HAL_RCC_DTCMRAMEN_CLK_ENABLE();
        #elif defined(CCMDATARAM_BASE)
        // enable the CCM RAM
        __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();
        #endif
    #elif defined(STM32H7)
        // Enable D2 SRAM1/2/3 clocks.
        __HAL_RCC_D2SRAM1_CLK_ENABLE();
        __HAL_RCC_D2SRAM2_CLK_ENABLE();
        __HAL_RCC_D2SRAM3_CLK_ENABLE();
    #endif


    #if defined(MICROPY_BOARD_EARLY_INIT)
    MICROPY_BOARD_EARLY_INIT();
    #endif

    // basic sub-system init
    #if MICROPY_HW_SDRAM_SIZE
    sdram_init();
    #if MICROPY_HW_SDRAM_STARTUP_TEST
    sdram_test(true);
    #endif
    #endif
    #if MICROPY_PY_THREAD
    pyb_thread_init(&pyb_thread_main);
    #endif
    pendsv_init();
    led_init();
    #if MICROPY_HW_HAS_SWITCH
    switch_init0();
    #endif
    machine_init();
    #if MICROPY_HW_ENABLE_RTC
    rtc_init_start(false);
    #endif
    uart_init0();
    spi_init0();
    #if MICROPY_PY_PYB_LEGACY && MICROPY_HW_ENABLE_HW_I2C
    i2c_init0();
    #endif
    #if MICROPY_HW_ENABLE_SDCARD
    sdcard_init();
    #endif
    #if MICROPY_HW_ENABLE_STORAGE
    storage_init();
    #endif
    #if MICROPY_PY_LWIP
    // lwIP doesn't allow to reinitialise itself by subsequent calls to this function
    // because the system timeout list (next_timeout) is only ever reset by BSS clearing.
    // So for now we only init the lwIP stack once on power-up.
    lwip_init();
    systick_enable_dispatch(SYSTICK_DISPATCH_LWIP, mod_network_lwip_poll_wrapper);
    #endif

    #if MICROPY_PY_NETWORK_CYW43
    {
        cyw43_init(&cyw43_state);
        uint8_t buf[8];
        memcpy(&buf[0], "PYBD", 4);
        mp_hal_get_mac_ascii(MP_HAL_MAC_WLAN0, 8, 4, (char*)&buf[4]);
        cyw43_wifi_ap_set_ssid(&cyw43_state, 8, buf);
        cyw43_wifi_ap_set_password(&cyw43_state, 8, (const uint8_t*)"pybd0123");
    }
    #endif

    #if defined(MICROPY_HW_UART_REPL)
    // Set up a UART REPL using a statically allocated object
    pyb_uart_repl_obj.base.type = &pyb_uart_type;
    pyb_uart_repl_obj.uart_id = MICROPY_HW_UART_REPL;
    pyb_uart_repl_obj.is_static = true;
    pyb_uart_repl_obj.timeout = 0;
    pyb_uart_repl_obj.timeout_char = 2;
    uart_init(&pyb_uart_repl_obj, MICROPY_HW_UART_REPL_BAUD, UART_WORDLENGTH_8B, UART_PARITY_NONE, UART_STOPBITS_1, 0);
    uart_set_rxbuf(&pyb_uart_repl_obj, sizeof(pyb_uart_repl_rxbuf), pyb_uart_repl_rxbuf);
    uart_attach_to_repl(&pyb_uart_repl_obj, true);
    MP_STATE_PORT(pyb_uart_obj_all)[MICROPY_HW_UART_REPL - 1] = &pyb_uart_repl_obj;
    #endif

soft_reset:

    #if defined(MICROPY_HW_LED2)
    led_state(1, 0);
    led_state(2, 1);
    #else
    led_state(1, 1);
    led_state(2, 0);
    #endif
    led_state(3, 0);
    led_state(4, 0);

    #if !MICROPY_HW_USES_BOOTLOADER
    // check if user switch held to select the reset mode
    reset_mode = update_reset_mode(1);
    #endif

    // Python threading init
    #if MICROPY_PY_THREAD
    mp_thread_init();
    #endif

    // Stack limit should be less than real stack size, so we have a chance
    // to recover from limit hit.  (Limit is measured in bytes.)
    // Note: stack control relies on main thread being initialised above
    mp_stack_set_top(&_estack);
    mp_stack_set_limit((char*)&_estack - (char*)&_heap_end - 1024);

    // GC init
    gc_init(MICROPY_HEAP_START, MICROPY_HEAP_END);

    #if MICROPY_ENABLE_PYSTACK
    static mp_obj_t pystack[384];
    mp_pystack_init(pystack, &pystack[384]);
    #endif

    // MicroPython init
    mp_init();
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_path), 0);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_)); // current dir (or base dir of the script)
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);

    // Initialise low-level sub-systems.  Here we need to very basic things like
    // zeroing out memory and resetting any of the sub-systems.  Following this
    // we can run Python scripts (eg boot.py), but anything that is configurable
    // by boot.py must be set after boot.py is run.

    #if defined(MICROPY_HW_UART_REPL)
    MP_STATE_PORT(pyb_stdio_uart) = &pyb_uart_repl_obj;
    #else
    MP_STATE_PORT(pyb_stdio_uart) = NULL;
    #endif

    readline_init0();
    pin_init0();
    extint_init0();
    timer_init0();

    #if MICROPY_HW_ENABLE_CAN
    can_init0();
    #endif

    #if MICROPY_HW_ENABLE_USB
    pyb_usb_init0();
    #endif

    // Initialise the local flash filesystem.
    // Create it if needed, mount in on /flash, and set it as current dir.
    bool mounted_flash = false;
    #if MICROPY_HW_ENABLE_STORAGE
    mounted_flash = init_flash_fs(reset_mode);
    #endif

    bool mounted_sdcard = false;
    #if MICROPY_HW_SDCARD_MOUNT_AT_BOOT
    // if an SD card is present then mount it on /sd/
    if (sdcard_is_present()) {
        // if there is a file in the flash called "SKIPSD", then we don't mount the SD card
        if (!mounted_flash || f_stat(&fs_user_mount_flash.fatfs, "/SKIPSD", NULL) != FR_OK) {
            mounted_sdcard = init_sdcard_fs();
        }
    }
    #endif

    #if MICROPY_HW_ENABLE_USB
    // if the SD card isn't used as the USB MSC medium then use the internal flash
    if (pyb_usb_storage_medium == PYB_USB_STORAGE_MEDIUM_NONE) {
        pyb_usb_storage_medium = PYB_USB_STORAGE_MEDIUM_FLASH;
    }
    #endif

    // set sys.path based on mounted filesystems (/sd is first so it can override /flash)
    if (mounted_sdcard) {
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_sd));
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_sd_slash_lib));
    }
    if (mounted_flash) {
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_flash));
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_flash_slash_lib));
    }

    // reset config variables; they should be set by boot.py
    MP_STATE_PORT(pyb_config_main) = MP_OBJ_NULL;

    // run boot.py, if it exists
    // TODO perhaps have pyb.reboot([bootpy]) function to soft-reboot and execute custom boot.py
    if (reset_mode == 1 || reset_mode == 3) {
        const char *boot_py = "boot.py";
        int ret = pyexec_file_if_exists(boot_py);
        if (ret & PYEXEC_FORCED_EXIT) {
            goto soft_reset_exit;
        }
        if (!ret) {
            flash_error(4);
        }
    }

    // turn boot-up LEDs off
    #if !defined(MICROPY_HW_LED2)
    // If there is only one LED on the board then it's used to signal boot-up
    // and so we turn it off here.  Otherwise LED(1) is used to indicate dirty
    // flash cache and so we shouldn't change its state.
    led_state(1, 0);
    #endif
    led_state(2, 0);
    led_state(3, 0);
    led_state(4, 0);

    // Now we initialise sub-systems that need configuration from boot.py,
    // or whose initialisation can be safely deferred until after running
    // boot.py.

    #if MICROPY_HW_ENABLE_USB
    // init USB device to default setting if it was not already configured
    if (!(pyb_usb_flags & PYB_USB_FLAG_USB_MODE_CALLED)) {
        pyb_usb_dev_init(USBD_VID, USBD_PID_CDC_MSC, USBD_MODE_CDC_MSC, NULL);
    }
    #endif

    #if MICROPY_HW_HAS_MMA7660
    // MMA accel: init and reset
    accel_init();
    #endif

    #if MICROPY_HW_ENABLE_SERVO
    servo_init();
    #endif

    #if MICROPY_PY_NETWORK
    mod_network_init();
    #endif

    // At this point everything is fully configured and initialised.

    // Run the main script from the current directory.
    if ((reset_mode == 1 || reset_mode == 3) && pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
        const char *main_py;
        if (MP_STATE_PORT(pyb_config_main) == MP_OBJ_NULL) {
            main_py = "main.py";
        } else {
            main_py = mp_obj_str_get_str(MP_STATE_PORT(pyb_config_main));
        }
        int ret = pyexec_file_if_exists(main_py);
        if (ret & PYEXEC_FORCED_EXIT) {
            goto soft_reset_exit;
        }
        if (!ret) {
            flash_error(3);
        }
    }

    // Main script is finished, so now go into REPL mode.
    // The REPL mode can change, or it can request a soft reset.
    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            if (pyexec_raw_repl() != 0) {
                break;
            }
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

soft_reset_exit:

    // soft reset

    #if MICROPY_HW_ENABLE_STORAGE
    printf("MPY: sync filesystems\n");
    storage_flush();
    #endif

    printf("MPY: soft reboot\n");
    #if MICROPY_PY_NETWORK
    mod_network_deinit();
    #endif
    timer_deinit();
    uart_deinit_all();
    #if MICROPY_HW_ENABLE_CAN
    can_deinit();
    #endif
    machine_deinit();

    #if MICROPY_PY_THREAD
    pyb_thread_deinit();
    #endif

    gc_sweep_all();

    goto soft_reset;
}
