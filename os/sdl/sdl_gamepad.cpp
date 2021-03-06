

#include "StdAfx.h"
#include "sdl_inc.h"
#include "sdl_os.h"

typedef std::map<SDL_JoystickID, SDL_GameController*> ControllerMap;

static ControllerMap s_controllers;
static bool          s_gamepad_enabled = true;
static std::mutex    s_mutex;


// don't go through ReportMessagef/ReportMessage!
static void ReportGP(const char *format, ...)
{
    va_list vl;
    va_start(vl, format);
    const string buf = "\n[SDL] " + str_vformat(format, vl);
    OL_ReportMessage(buf.c_str());
    va_end(vl);
}

static SDL_JoystickID initGamepad()
{
    static bool loadedMappings = true; // disable loading!
    if (!loadedMappings)
    {
        SDL_version linked;
        SDL_GetVersion(&linked);
        if (linked.minor > 0 || linked.patch > 1)
        {
            const int mappings = SDL_GameControllerAddMappingsFromFile(OL_PathForFile("data/gamecontrollerdb.txt", "r"));
            ReportGP("Loaded %d game controller mappings: %s",
                             max(0, mappings), mappings < 0 ? SDL_GetError() : "OK");
        }
        loadedMappings = true;
    }

    SDL_JoystickID changed = 0;

    // close unattached controllers
    for (ControllerMap::iterator it=s_controllers.begin(); it != s_controllers.end(); )
    {
        if (!SDL_GameControllerGetAttached(it->second))
        {
            changed = it->first;
            SDL_GameControllerClose(it->second);
            it = s_controllers.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // open new controllers
    for (int i=0; i<SDL_NumJoysticks(); i++)
    {
        if (!SDL_IsGameController(i))
            continue;
        
        SDL_GameController *ctrl = SDL_GameControllerOpen(i);
        if (!ctrl)
        {
            ReportGP("Could not open gamecontroller %d: %s", i, SDL_GetError());
            continue;
        }
        SDL_JoystickID jid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl));
        if (s_controllers.count(jid))
            continue;

        ReportGP("Opened controller %d, named: %s", jid, SDL_GameControllerName(ctrl));
        s_controllers.insert(make_pair(jid, ctrl));
        changed = jid;
    }

    return changed;
}

void OL_SetGamepadEnabled(int enabled)
{
    std::lock_guard<std::mutex> l(s_mutex);
    
    s_gamepad_enabled = enabled;
    if (!enabled)
    {
        for_ (it, s_controllers)
        {
            SDL_GameControllerClose(it.second);
        }
        s_controllers.clear();
    }
}

const char* OL_GetGamepadName(int instance_id)
{
    std::lock_guard<std::mutex> l(s_mutex);
    return s_controllers.count(instance_id) ? SDL_GameControllerName(s_controllers[instance_id]) : NULL;
}


int Controller_HandleEvent(const SDL_Event *evt)
{
    std::lock_guard<std::mutex> l(s_mutex);
    
    OLEvent e;
    memset(&e, 0, sizeof(e));
    
    switch (evt->type) {
    case SDL_CONTROLLERDEVICEADDED:
    {
        ReportGP("SDL_CONTROLLERDEVICEADDED");
        e.which = initGamepad();
        e.type = OL_GAMEPAD_ADDED;
        OLG_OnEvent(&e);
        return 1;
    }
    case SDL_CONTROLLERDEVICEREMOVED:
    {
        ReportGP("SDL_CONTROLLERDEVICEREMOVED");
        e.which = initGamepad();
        e.type = OL_GAMEPAD_REMOVED;
        OLG_OnEvent(&e);
        return 1;
    }
    case SDL_CONTROLLERAXISMOTION: 
    {
        const SDL_ControllerAxisEvent &caxis = evt->caxis;
        e.type = OL_GAMEPAD_AXIS;
        e.which = caxis.which;
        e.y = caxis.value / 32767.f;
        switch (caxis.axis)
        {
        case SDL_CONTROLLER_AXIS_LEFTX: e.key = GamepadAxisLeftX; break;
        case SDL_CONTROLLER_AXIS_LEFTY: e.key = GamepadAxisLeftY; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: e.key = GamepadAxisRightX; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: e.key = GamepadAxisRightY; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: e.key = GamepadAxisTriggerLeftY; break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: e.key = GamepadAxisTriggerRightY; break;
        }
        OLG_OnEvent(&e);
        return 1;
    }
    case SDL_CONTROLLERBUTTONDOWN: // fallthrough
    case SDL_CONTROLLERBUTTONUP:
    {
        const SDL_ControllerButtonEvent &cbutton = evt->cbutton;
        e.key = (int)cbutton.button + GamepadA;
        e.type = cbutton.state == SDL_PRESSED ? OL_KEY_DOWN : OL_KEY_UP;
        e.which = cbutton.which;
        OLG_OnEvent(&e);
        return 1;
    }   
    }
    return 0;
}


int Controller_Init()
{
    std::lock_guard<std::mutex> l(s_mutex);
    
    SDL_Init(SDL_INIT_GAMECONTROLLER);
    return 1;
    
}
