#include <chrono>
#include <cstdint>
#include <cctype>
#include <csetjmp>

#include <fcntl.h>
#include <sys/mman.h>

#include "emu.h"
#include "translate.h"
#include "debug.h"
#include "mmu.h"
#include "gdbstub.h"
#include "usblink_queue.h"
#include "os/os.h"

/* cycle_count_delta is a (usually negative) number telling what the time is relative
 * to the next scheduled event. See sched.c */
int cycle_count_delta = 0;

int throttle_delay = 10; /* in milliseconds */

uint32_t cpu_events;

bool do_translate = true;
uint32_t product = 0x0E0, asic_user_flags = 0;
bool turbo_mode = false;

volatile bool exiting, debug_on_start, debug_on_warn, large_nand, large_sdram;
BootOrder boot_order = ORDER_DEFAULT;
uint32_t boot2_base;
const char *path_boot1 = NULL, *path_boot2 = NULL, *path_flash = NULL, *pre_manuf = NULL, *pre_boot2 = NULL, *pre_diags = NULL, *pre_os = NULL;

void *restart_after_exception[32];

const char log_type_tbl[] = LOG_TYPE_TBL;
int log_enabled[MAX_LOG];
FILE *log_file[MAX_LOG];
void logprintf(int type, const char *str, ...) {
    if (log_enabled[type]) {
        va_list va;
        va_start(va, str);
        vfprintf(log_file[type], str, va);
        va_end(va);
    }
}

void emuprintf(const char *format, ...) {
    va_list va;
    va_start(va, format);
    gui_debug_vprintf(format, va);
    va_end(va);
}

void warn(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    gui_debug_printf("Warning (%08x): ", arm.reg[15]);
    gui_debug_vprintf(fmt, va);
    gui_debug_printf("\n");
    va_end(va);
    if (debug_on_warn)
        debugger(DBG_EXCEPTION, 0);
}

__attribute__((noreturn))
void error(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    gui_debug_printf("Error (%08x): ", arm.reg[15]);
    gui_debug_vprintf(fmt, va);
    gui_debug_printf("\n");
    va_end(va);
    debugger(DBG_EXCEPTION, 0);
    cpu_events |= EVENT_RESET;
    __builtin_longjmp(restart_after_exception, 1);
}

int exec_hack() {
    if (arm.reg[15] == 0x10040) {
        arm.reg[15] = arm.reg[14];
        warn("BOOT1 is required to run this version of BOOT2.");
        return 1;
    }
    return 0;
}

extern "C" void usblink_timer();

void throttle_interval_event(int index)
{
    event_repeat(index, 27000000 / 100);

    /* Throttle interval (defined arbitrarily as 100Hz) - used for
     * keeping the emulator speed down, and other miscellaneous stuff
     * that needs to be done periodically */
    static int intervals = 0, prev_intervals = 0;
    intervals += 1;

    usblink_timer();

    usblink_queue_do();

    int c = gui_getchar();
    if(c != -1)
        serial_byte_in((char) c);

    gdbstub_recv();

    rdebug_recv();

    // Calculate speed
    auto interval_end = std::chrono::high_resolution_clock::now();
    static auto prev = interval_end;
    static double speed = 1.0;
    auto time = std::chrono::duration_cast<std::chrono::microseconds>(interval_end - prev).count();
    if (time >= 500000) {
        speed = (double)10000 * (intervals - prev_intervals) / time;
        gui_show_speed(speed);
        prev_intervals = intervals;
        prev = interval_end;
    }

    gui_do_stuff(true);

    if (!turbo_mode && speed > 0.7)
		throttle_timer_wait();
}

bool emu_start(unsigned int port_gdb, unsigned int port_rdbg, const char *snapshot_file)
{
    if(debug_on_start)
        cpu_events |= EVENT_DEBUG_STEP;

    if(snapshot_file)
    {
        // Open snapshot
        int fp = open(snapshot_file, O_RDONLY);
        if(fp == -1)
            return false;

        size_t size = lseek(fp, 0, SEEK_END);
        lseek(fp, 0, SEEK_SET);

        auto snapshot = (struct emu_snapshot *) mmap(NULL, size, PROT_READ, MAP_PRIVATE, fp, 0);
        close(fp);

        if((intptr_t) snapshot == -1)
            return false;

        //sched_reset();
        sched.items[SCHED_THROTTLE].clock = CLOCK_27M;
        sched.items[SCHED_THROTTLE].proc = throttle_interval_event;

        // Resume components
        uint32_t sdram_size;
        if(size < sizeof(emu_snapshot)
                || snapshot->sig != 0xCAFEBEEF
                || !flash_resume(snapshot)
                || !flash_read_settings(&sdram_size, &product, &asic_user_flags)
                || !cpu_resume(snapshot)
                || !memory_resume(snapshot)
                || !sched_resume(snapshot))
        {
            emu_cleanup();
            munmap(snapshot, size);
            return false;
        }

        munmap(snapshot, size);
    }
    else
    {
        if (!path_flash
            || !flash_open(path_flash))
                return false;

        uint32_t sdram_size;
        flash_read_settings(&sdram_size, &product, &asic_user_flags);

        flash_set_bootorder(boot_order);

        if(!memory_initialize(sdram_size))
            return false;
    }

    uint8_t *rom = mem_areas[0].ptr;
    memset(rom, -1, 0x80000);
    for (int i = 0x00000; i < 0x80000; i += 4)
        RAM_FLAGS(&rom[i]) = RF_READ_ONLY;

    if (path_boot1) {
        /* Load the ROM */
        FILE *f = fopen(path_boot1, "rb");
        if (!f) {
            gui_perror(path_boot1);
            return false;
        }
        fread(rom, 1, 0x80000, f);
        fclose(f);
    }

#ifndef NO_TRANSLATION
    translate_init();
#endif

    os_exception_frame_t frame;
    addr_cache_init(&frame);

    throttle_timer_on();

    if(port_gdb)
        gdbstub_init(port_gdb);

    if(port_rdbg)
        rdebug_bind(port_rdbg);

    return true;
}

void emu_loop(bool reset)
{
    if(reset)
    {
    reset:
        memset(mem_areas[1].ptr, 0, mem_areas[1].size);

        memset(&arm, 0, sizeof arm);
        arm.control = 0x00050078;
        arm.cpsr_low28 = MODE_SVC | 0xC0;
        cpu_events &= EVENT_DEBUG_STEP;

        sched_reset();
        sched.items[SCHED_THROTTLE].clock = CLOCK_27M;
        sched.items[SCHED_THROTTLE].proc = throttle_interval_event;

        memory_reset();
    }

    gdbstub_reset();

    addr_cache_flush();
    flush_translations();

    sched_update_next_event(0);

    exiting = false;

    // TODO: try to properly fix that (it causes an ICE on clang)
    #ifndef IS_IOS_BUILD
        __builtin_setjmp(restart_after_exception);
    #endif

    while (!exiting) {
        sched_process_pending_events();
        while (!exiting && cycle_count_delta < 0) {
            if (cpu_events & EVENT_RESET) {
                gui_status_printf("Reset");
                goto reset;
            }

            if (cpu_events & (EVENT_FIQ | EVENT_IRQ)) {
                // Align PC in case the interrupt occurred immediately after a jump
                if (arm.cpsr_low28 & 0x20)
                    arm.reg[15] &= ~1;
                else
                    arm.reg[15] &= ~3;

                if (cpu_events & EVENT_WAITING)
                    arm.reg[15] += 4; // Skip over wait instruction

                arm.reg[15] += 4;
                cpu_exception((cpu_events & EVENT_FIQ) ? EX_FIQ : EX_IRQ);
            }
            cpu_events &= ~EVENT_WAITING;

            if (arm.cpsr_low28 & 0x20)
                cpu_thumb_loop();
            else
                cpu_arm_loop();
        }
    }
}

bool emu_suspend(const char *file)
{
    int fp = open(file, O_RDWR | O_CREAT, (mode_t) 0620);
    if(fp == -1)
        return false;

    size_t size = sizeof(emu_snapshot) + flash_suspend_flexsize();
    if(ftruncate(fp, size))
    {
        close(fp);
        return false;
    }

    auto snapshot = (struct emu_snapshot *) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fp, 0);
    if((intptr_t) snapshot == -1)
    {
        close(fp);
        return false;
    }

    snapshot->product = product;
    snapshot->asic_user_flags = asic_user_flags;
    strncpy(snapshot->path_boot1, path_boot1, sizeof(snapshot->path_boot1) - 1);
    strncpy(snapshot->path_flash, path_flash, sizeof(snapshot->path_flash) - 1);

    if(!flash_suspend(snapshot)
            || !cpu_suspend(snapshot)
            || !sched_suspend(snapshot)
            || !memory_suspend(snapshot))
    {
        munmap(snapshot, size);
        close(fp);
    }

    snapshot->sig = 0xCAFEBEEF;

    munmap(snapshot, size);
    close(fp);
    return true;
}

void emu_cleanup()
{
    exiting = true;

    if(debugger_input)
        fclose(debugger_input);

    // addr_cache_init is rather expensive and needs to be called once only
    //addr_cache_deinit();

    #ifndef NO_TRANSLATION
        translate_deinit();
    #endif

    memory_reset();
    memory_deinitialize();
    flash_close();

    gdbstub_quit();
    rdebug_quit();
}