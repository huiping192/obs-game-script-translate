#include <obs-module.h>
#include "translate-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-game-translator", "en-US")

bool obs_module_load(void)
{
    register_translate_source();
    return true;
}

void obs_module_unload(void)
{
}
