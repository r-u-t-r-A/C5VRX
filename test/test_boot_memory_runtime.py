#!/usr/bin/env python3
"""C5VRX by Twotoz/contributors: fault-inject actual boot memory functions.
Host checks cover allocation/startup contracts, not RF or board acceptance.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run(source):
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 'boot.c'
        c.write_text(source)
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                   '-Werror', '-I' + str(ROOT / 'main'), str(c)]
        subprocess.run(command + ['-o', str(Path(tmp) / 'boot')], check=True)
        subprocess.run([str(Path(tmp) / 'boot')], check=True)


class BootMemoryTest(unittest.TestCase):
    def test_receiver_reservation_and_failures(self):
        source = (ROOT / 'main/receiver.c').read_text()
        storage = source[source.index('#define OBSERVER_STACK_BYTES'):source.index('static volatile uint8_t s_gain_mode')]
        functions = source[source.index('bool receiver_prepare(void)'):source.index('void receiver_print(void)')]
        run(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define portBYTE_ALIGNMENT 16
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define ESP_TIMER_TASK 0
#define ESP_OK 0
#define DG_MAP_VERSION 1
/* A byte-addressed IDF stack, with a nontrivial TCB and alignment. */
typedef uint8_t StackType_t;
typedef struct { uint8_t opaque[320]; } StaticTask_t;
typedef void *TaskHandle_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); int dispatch_method;
 const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
static unsigned allocations, creates, starts, deletes, task_calls, fail_task;
static bool fail_alloc, fail_timer, fail_start;
static void *heap_caps_aligned_alloc(unsigned align, size_t size, unsigned caps) {
 assert(align==portBYTE_ALIGNMENT && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
 ++allocations;return fail_alloc?NULL:malloc(size);
}
static void heap_caps_free(void *p) {free(p);}
static void tick_cb(void *p) {(void)p;}
static int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) {
 assert(a->callback==tick_cb && a->skip_unhandled_events);++creates;
 if(fail_timer)return -1;
 *h=(void *)1;return ESP_OK;
}
static int esp_timer_start_periodic(esp_timer_handle_t h, unsigned us) {
 assert(h && us==200);++starts;return fail_start?-1:ESP_OK;
}
static void observer_task(void *p) {(void)p;}
static void sentinel_task(void *p) {(void)p;}
static TaskHandle_t xTaskCreateStatic(void (*fn)(void *),const char *name,unsigned bytes,
 void *arg,unsigned priority,StackType_t *stack,StaticTask_t *task) {
 ++task_calls;assert(!arg && stack && task);
 assert(((uintptr_t)stack % portBYTE_ALIGNMENT)==0);
 if(!strcmp(name,"gain_obs"))assert(fn==observer_task && bytes==4096 && priority==3);
 else assert(!strcmp(name,"gain_fast") && fn==sentinel_task && bytes==3072 && priority==4);
 return task_calls==fail_task?NULL:task;
}
static void vTaskDelete(TaskHandle_t h) {assert(h);++deletes;}
static struct {int version;bool context_valid;} s_gain_map_saved;
static bool s_gain_map_saved_valid;
static bool blob_load(const char *key,void *p,size_t size) {
 assert(!strcmp(key,"gain_map") && p==&s_gain_map_saved && size==sizeof(s_gain_map_saved));
 s_gain_map_saved.version=DG_MAP_VERSION;s_gain_map_saved.context_valid=true;return true;
}
''' + storage + functions + r'''
int main(void) {
 fail_alloc=true;assert(!receiver_prepare());assert(!s_receiver_memory && !s_gain_timer);
 fail_alloc=false;fail_timer=true;assert(!receiver_prepare());assert(!s_receiver_memory && !s_gain_timer);
 fail_timer=false;assert(receiver_prepare());assert(!s_observer && !s_sentinel && !starts && !task_calls);
 unsigned reserved=allocations,timers=creates;
 assert(receiver_prepare());assert(allocations==reserved && creates==timers);
 /* No heap remains after splash: startup must use only the reservation. */
 fail_alloc=true;assert(receiver_start());assert(s_gain_map_saved_valid);
 assert(allocations==reserved && creates==timers && starts==1 && task_calls==2);
 assert(receiver_start());assert(starts==1 && task_calls==2);
 vTaskDelete(s_sentinel);vTaskDelete(s_observer);s_observer=s_sentinel=NULL;
 unsigned removed=deletes;
 fail_task=task_calls+1;assert(!receiver_start());assert(!s_observer && !s_sentinel && deletes==removed);
 fail_task=task_calls+2;assert(!receiver_start());assert(!s_observer && !s_sentinel && deletes==removed+1);
 fail_task=0;fail_start=true;assert(!receiver_start());
 assert(!s_observer && !s_sentinel && deletes==removed+3);
 fail_start=false;assert(receiver_start());assert(allocations==reserved && creates==timers);
 vTaskDelete(s_sentinel);vTaskDelete(s_observer);heap_caps_free(s_receiver_memory);
}
''')

    def test_boot_preserves_task_start_order(self):
        source = (ROOT / 'main/main.c').read_text()
        self.assertLess(source.index('receiver_prepare()'), source.index('rf_start()'))
        self.assertLess(source.index('transport_start()'), source.index('boot_window_begin()'))
        self.assertLess(source.index('boot_window_begin()'), source.index('receiver_start()'))
        self.assertLess(source.index('receiver_start()'), source.index('control_start()'))
        failure = source[source.index('if (!receiver_start())'):source.index('    control_start();')]
        for statement in ['boot_window_end();', 'transport_stop_tx();', 'return;']:
            self.assertIn(statement, failure)
        control = (ROOT / 'main/control.c').read_text()
        # c5div-rx: no command queue; the control task still starts from the
        # early static reservation, never from the heap after the boot window.
        self.assertIn('xTaskCreateStatic(control_task', control)
        self.assertNotIn('xTaskCreate(control_task', control)


if __name__ == '__main__':
    unittest.main()
