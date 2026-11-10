#ifndef PHONON_4DPLUGIN_H
#define PHONON_4DPLUGIN_H

#include "4DPluginAPI.h"

static void Phonon_Load_model(PA_PluginParameters params);
static void Phonon_Load_models(PA_PluginParameters params);
static void Phonon_UNLOAD_MODEL(PA_PluginParameters params);
static void Phonon_Get_models(PA_PluginParameters params);
static void Phonon_Get_model_info(PA_PluginParameters params);
static void Phonon_ADD_VOICE(PA_PluginParameters params);
static void Phonon_Play(PA_PluginParameters params);
static void Phonon_Play_multiple(PA_PluginParameters params);
static void Phonon_PAUSE(PA_PluginParameters params);
static void Phonon_RESUME(PA_PluginParameters params);
static void Phonon_STOP(PA_PluginParameters params);
static void Phonon_Get_status(PA_PluginParameters params);
static void Phonon_Get_statuses(PA_PluginParameters params);
static void Phonon_PAUSE_ALL(PA_PluginParameters params);
static void Phonon_RESUME_ALL(PA_PluginParameters params);
static void Phonon_STOP_ALL(PA_PluginParameters params);
static void Phonon_SET_VOLUME(PA_PluginParameters params);
static void Phonon_Synthesize(PA_PluginParameters params);
static void Phonon_Get_last_error(PA_PluginParameters params);
static void Phonon_SET_THREADS(PA_PluginParameters params);
static void Phonon_Get_info(PA_PluginParameters params);
static void Phonon_UNLOAD_ALL(PA_PluginParameters params);

#endif
