/* MicroPython configuration for the ESP32-P4 Launcher's Developer Mode app (a .papp). */
#include <stdint.h>
#include <limits.h>
#include <alloca.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX INT_MAX                  /* newlib keeps it behind POSIX feature macros */
#endif

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

/* core */
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_KBD_EXCEPTION               (1)
#define MICROPY_ENABLE_SCHEDULER            (1)
#define MICROPY_SCHEDULER_DEPTH             (8)
#define MICROPY_STACK_CHECK                 (1)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE (256)
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_READER_POSIX                (0)   /* port/modos.c loads files (paths mapped to /sd/python) */
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)
#define MICROPY_USE_INTERNAL_PRINTF         (1)
#define MICROPY_PY_SYS_PLATFORM             "esp32p4"
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (1)
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_BUILTINS_INPUT           (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO     (1)
#define MICROPY_HW_BOARD_NAME               "Guition ESP32-P4 4.3 - Abed Mihilia Launcher"
#define MICROPY_HW_MCU_NAME                 "ESP32P4"
#define MICROPY_BANNER_MACHINE              MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME
#define MICROPY_GCREGS_SETJMP               (0)
#define MICROPY_EPOCH_IS_1970               (1)

/* modules built here (port/ and the extmod files the build lists) */
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "port/modtime_port.h"
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (mp_port_random_seed())
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_IO_IOBASE                (1)

/* not on this port (no VFS, sockets, threads or crypto here) */
#define MICROPY_PY_OS                       (0)   /* port/modos.c provides "os" */
#define MICROPY_PY_MACHINE                  (0)   /* port/modmachine.c provides "machine" */
#define MICROPY_VFS                         (0)
#define MICROPY_PY_SELECT                   (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_HASHLIB                  (0)
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_UCTYPES                  (0)
#define MICROPY_PY_FRAMEBUF                 (0)
#define MICROPY_PY_ASYNCIO                  (0)
#define MICROPY_PY_DEFLATE                  (0)
#define MICROPY_PY_BTREE                    (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_PY_THREAD                   (0)
#define MICROPY_PY_LWIP                     (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_OPENAMP                  (0)
#define MICROPY_PY_SYS_SETTRACE             (0)
#define MICROPY_PERSISTENT_CODE_LOAD        (1)
#define MICROPY_MODULE_FROZEN_MPY           (0)
#define MICROPY_MODULE_FROZEN_STR           (0)

/* while Python waits (sleep, input), keep the scheduler and Ctrl+C alive */
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        extern void mp_handle_pending(bool); \
        mp_handle_pending(true); \
        mp_port_idle(); \
    } while (0);

typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

#define MICROPY_MPHALPORT_H                 "port/mphalport.h"
#define MP_STATE_PORT                       MP_STATE_VM

uint32_t mp_port_random_seed(void);
void mp_port_idle(void);
