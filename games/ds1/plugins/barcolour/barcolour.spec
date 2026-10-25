# What dsfix.dll calls in the plugin dsfix.barcolour (menu bar / HUD colour: recoloured sheet pages + readable text,
# and the colour wheel - also the PLUGINS menu's picker for plugins' colour settings); `= value` is dsfix.dll's answer
# without the plugin (no picker: those rows don't open one). scripts/gen_host_calls.py -> plugins/barcolour/generated/,
# src/generated/barcolour_wrappers.inc.
interface dsfix.barcolour
side plugin barcolour
pod features::ColourDone

# the frame dispatcher (launcher.cpp)
void features::OnFrameBarColour()

# the colour wheel (fixmenu.cpp: MENU BAR / HUD COLOUR; pluginsmenu.cpp: colour settings; devserver.cpp)
void features::OpenBarColourPicker(int target)
void features::OpenColourPicker(uint32_t colour, uint32_t defaultColour, features::ColourDone done, void* ctx)
bool features::BarColourPickerOpen() = false
int features::BarColourPickerTarget() = -1
void features::UpdateBarColourPicker()
bool features::DrawBarColourPicker(void* screen) = false
bool features::ColourPickerDev(const char* op, uint32_t colour) = false
uint32_t features::ColourPickerColour() = 0
