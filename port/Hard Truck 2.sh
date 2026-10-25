#!/bin/bash
# PortMaster launcher for Hard Truck 2: King of the Road (hardtruck2-recomp).
# ports/hardtruck2/gamedata holds the player's own install of the game.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# Older PortMaster releases (AmberELEC's own) predate DEVICE_ARCH.
DEVICE_ARCH="${DEVICE_ARCH:-$(uname -m)}"

GAMEDIR="/$directory/ports/hardtruck2"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

if [ "$DEVICE_ARCH" != "aarch64" ]; then
  echo "Hard Truck 2 needs a 64-bit (aarch64) system, this one is $DEVICE_ARCH."
  sleep 5
  exit 1
fi

# The game itself is not part of the port: king.exe and its data folders go in
# gamedata/ (gamedata/README.txt). File names are matched without case.
if [ -z "$(find "$GAMEDIR/gamedata" -maxdepth 1 -iname king.exe)" ]; then
  echo "Copy your Hard Truck 2 install into $GAMEDIR/gamedata (king.exe, COMMON, ENV, MUSIC, ...)."
  sleep 5
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# The game paces itself on the display's vsync; LONGROAD2_FPS_LIMIT=30 here
# trades smoothness for a cooler, longer-running device.

chmod +x "$GAMEDIR/ht2.${DEVICE_ARCH}" 2>/dev/null

# No gamepad support in the game yet: gptokeyb turns the pad into the keyboard
# and mouse it expects (hardtruck2.gptk).
$GPTOKEYB "ht2.${DEVICE_ARCH}" -c "$GAMEDIR/hardtruck2.gptk" &
[ "$(type -t pm_platform_helper)" = function ] && pm_platform_helper "$GAMEDIR/ht2.${DEVICE_ARCH}"

./ht2.${DEVICE_ARCH} --game "$GAMEDIR/gamedata"

if [ "$(type -t pm_finish)" = function ]; then
  pm_finish
else
  $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
  $ESUDO systemctl restart oga_events 2>/dev/null &
  printf "\033c" > /dev/tty0
fi
