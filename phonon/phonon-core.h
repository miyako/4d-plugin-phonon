// C interface of the phonon-core static library (../phonon-core, Rust).
// Declared by hand: keep it in step with ../phonon-core/src/lib.rs.
#ifndef PHONON_CORE_H
#define PHONON_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PhononModel PhononModel;

/// Called on the generating thread with mono float PCM in [-1, 1]. Return false to stop.
typedef bool (*phonon_pcm_fn)(const float *pcm, size_t n, void *user);

/// Load a checkpoint folder. NULL on failure, with *error set (release with phonon_string_free).
PhononModel *phonon_load(const char *dir, const char *options_json, char **error);
/// The model's facts as JSON (release with phonon_string_free).
char *phonon_info(const PhononModel *m);
uint32_t phonon_sample_rate(const PhononModel *m);
/// Blocks while another utterance of the same model is generating.
bool phonon_speak(PhononModel *m, const char *text, const char *options_json,
                  phonon_pcm_fn cb, void *user, uint64_t *samples, char **error);
bool phonon_add_voice(PhononModel *m, const char *name, const char *path, char **error);
void phonon_set_threads(size_t n);
size_t phonon_get_threads(void);
const char *phonon_engine_version(void);
void phonon_free(PhononModel *m);
void phonon_string_free(char *s);

#ifdef __cplusplus
}
#endif

#endif
