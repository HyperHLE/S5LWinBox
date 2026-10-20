/*
 * S5LWinBox — the Windows / Desktop engine facade.
 *
 * This is the desktop counterpart of S5LAndBox's engine: one thread owns
 * the s5l8900 machine for its whole life, and the GUI thread publishes
 * input or reads copied frames under a mutex.
 */
#ifndef S5LWINBOX_ENGINE_H
#define S5LWINBOX_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct engine engine_t;

typedef enum {
    ENGINE_STOP_PLAIN = 0,     /* stop, keep whatever resume pair exists      */
    ENGINE_STOP_CHECKPOINT = 1,/* save the automatic resume pair, then stop   */
    ENGINE_STOP_POWER_OFF = 2  /* stop and disarm automatic resume            */
} engine_stop_mode_t;

engine_t *engine_create(const char *root_directory);
void engine_destroy(engine_t *engine);

/* ---------------------------------------------------------------- firmware */
int engine_import_ipsw(engine_t *engine, const char *ipsw_path,
                       bool skip_stock_checksums,
                       char *detail, size_t detail_capacity);
void engine_import_cancel(engine_t *engine);
void engine_import_progress(engine_t *engine, int *active, int *artefact,
                            int *stage, uint64_t *done, uint64_t *total);
int engine_firmware_json(engine_t *engine, char *out, size_t capacity);

/* --------------------------------------------------------------- machines */
int engine_machines_json(engine_t *engine, char *out, size_t capacity);
int engine_get_or_create_default_machine(engine_t *engine, char *out_id, size_t capacity);
int engine_machine_create(engine_t *engine, const char *name,
                          const bool *options, unsigned option_count,
                          unsigned disk_size_gib,
                          char *new_id, size_t id_capacity,
                          char *detail, size_t detail_capacity);
int engine_machine_rename(engine_t *engine, const char *id, const char *name,
                          char *detail, size_t detail_capacity);
int engine_machine_delete(engine_t *engine, const char *id,
                          char *detail, size_t detail_capacity);
int engine_machine_set_disk_size(engine_t *engine, const char *id,
                                 unsigned disk_size_gib,
                                 char *detail, size_t detail_capacity);
int engine_options_json(engine_t *engine, char *out, size_t capacity);
int engine_machine_options_json(engine_t *engine, const char *id,
                                char *out, size_t capacity);
int engine_machine_set_options(engine_t *engine, const char *id,
                               const bool *options, unsigned option_count,
                               char *detail, size_t detail_capacity);
int engine_ipa_inspect_json(const char *ipa_path, char *out, size_t capacity);
int engine_machine_install_ipa(engine_t *engine, const char *id,
                               const char *cache_directory,
                               const char *ipa_path,
                               char *detail, size_t detail_capacity);

/* --------------------------------------------------------------- packages */
int engine_packages_json(engine_t *engine, const char *id,
                         char *out, size_t capacity);
int engine_install_packages(engine_t *engine, const char *id,
                            const char *package_directory,
                            bool include_appsync,
                            char *out, size_t capacity,
                            char *detail, size_t detail_capacity);
void engine_install_progress(engine_t *engine, int *active, int *phase,
                             uint64_t *done, uint64_t *total);

/* -------------------------------------------------------------------- run */
int engine_start(engine_t *engine, const char *instance_id,
                 char *detail, size_t detail_capacity);
void engine_stop(engine_t *engine, engine_stop_mode_t mode);
void engine_set_paused(engine_t *engine, bool paused);
void engine_set_instruction_cap(engine_t *engine, uint64_t cap);
int engine_status_json(engine_t *engine, char *out, size_t capacity);
size_t engine_console_drain(engine_t *engine, char *out, size_t capacity);
uint16_t engine_get_web_log_port(engine_t *engine);
size_t engine_log_entries_json(engine_t *engine, uint32_t last_id, char *out, size_t capacity);

/* ---------------------------------------------------------------- display */
/* Copies current 320x480 BGRA32 screen into dst. Returns true if frame was updated. */
bool engine_copy_frame(engine_t *engine, void *dst_pixels, size_t dst_capacity,
                       uint32_t *out_width, uint32_t *out_height);

/* ------------------------------------------------------------------ audio */
/* Drains pending stereo 16-bit PCM audio samples. Returns number of samples read. */
size_t engine_read_audio_pcm(engine_t *engine, int16_t *dst_samples, size_t max_samples);

/* ------------------------------------------------------------------ input */
bool engine_touch(engine_t *engine, double x, double y,
                  double view_w, double view_h, int phase);
bool engine_button(engine_t *engine, unsigned app_button, bool pressed);

/* -------------------------------------------------------------- snapshots */
int engine_snapshots_json(engine_t *engine, const char *id,
                          char *out, size_t capacity);
int engine_snapshot_create(engine_t *engine, const char *id,
                           const char *name,
                           char *snapshot_id, size_t id_capacity,
                           char *detail, size_t detail_capacity);
int engine_snapshot_restore(engine_t *engine, const char *id,
                            const char *snapshot_id,
                            char *detail, size_t detail_capacity);
int engine_snapshot_delete(engine_t *engine, const char *id,
                           const char *snapshot_id,
                           char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LWINBOX_ENGINE_H */
