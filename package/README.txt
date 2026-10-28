DESTINY 2 VR  -  version {VERSION} (beta)
by The Cosmic Tangerine

A PC VR mod for Project Sunrise, the offline Destiny 2 preservation project. Play Destiny 2 in your
headset with motion controllers: head tracking, your own hands and arms, guns held where your hands
are, dual wield, swords and supers in first person, and the sparrow from the cockpit.

Free and unofficial. Not affiliated with Bungie or with the Project Sunrise team: please do not
take Destiny 2 VR problems to Sunrise's Discord or issues. Report them here instead:
https://github.com/thecosmictangerine-cloud/destiny-2-vr/issues


==================================================================================================
 IMPORTANT: NEVER INSTALL THIS INTO YOUR NORMAL STEAM DESTINY 2
==================================================================================================

This mod only works with Project Sunrise: the old 2020 build of the game, installed in its own
folder, played offline. Your normal Destiny 2 from Steam is an online game with anti-cheat, and
putting a modified file into it can get your account banned.

Both the installer and the mod check this and refuse to run anywhere else, but do not try.


--------------------------------------------------------------------------------------------------
 WHAT YOU NEED
--------------------------------------------------------------------------------------------------

- Windows 10 or 11, 64-bit.
- Project Sunrise installed with the Sunrise Launcher (it needs a Steam account that owns Destiny 2
  and about 100 GB of disk): https://projectsunrise.dev/guides/installing/
  Start it once without VR to check it works before installing this mod.
- A PC VR headset with an OpenXR runtime. Developed on a Meta Quest 3 over Link / Air Link.
  Other headsets (Virtual Desktop, SteamVR headsets) should work but are less tested.

This version contains Project Sunrise 0.5.1 (the current official release) with VR added.


--------------------------------------------------------------------------------------------------
 INSTALL
--------------------------------------------------------------------------------------------------

1. Unzip this whole folder anywhere (for example your Desktop).
2. Double-click Install.bat.
   It finds your Sunrise game folder (or asks you to choose it: the folder with destiny2.exe),
   checks that it really is a Sunrise install, backs up Sunrise's own steam_api64.dll and puts the
   VR one in its place. It also creates SVR_Settings.txt in the game folder.

If Windows shows "Windows protected your PC", click "More info" and then "Run anyway".

Manual install, if you prefer: copy steam_api64.dll into <game folder>\bin\x64\ (replacing the one
there; keep a copy of the original) and SVR_Settings.txt into the game folder.

UNINSTALL: double-click Uninstall.bat. It puts Sunrise's own DLL back.

THE SUNRISE LAUNCHER AFTERWARDS: its big button becomes REPAIR instead of PLAY ("Local files
changed" in its settings), because it sees that Sunrise's DLL was replaced. That is expected. Do
NOT press Repair or Update: they put the official DLL back and VR is gone (just run Install.bat
again if that happens). Launch game, in the Launcher's settings, is fine.


--------------------------------------------------------------------------------------------------
 PLAY
--------------------------------------------------------------------------------------------------

1. Start your headset's PC link (Quest Link / Air Link / Virtual Desktop / SteamVR).
2. Start destiny2.exe from the game folder (or the Launcher's settings > Launch game). Never through
   Steam.
3. Click the game window once on the PC so it has focus, then put the headset on.
   VR starts with the game. Menus, the map and loading screens appear on a big screen in front of
   you; point at it with either controller and pull its trigger (or press A) to click. A stick
   click brings the screen back in front of you.
4. Stand naturally and press F7 (or recentre in your headset's menu) if the view is off.

The game window must keep the focus: if you click another window, turning your head no longer
turns your character until you click the game again.


--------------------------------------------------------------------------------------------------
 CONTROLS
--------------------------------------------------------------------------------------------------

Left stick ............ move                  Right stick ........... turn
Right trigger ......... fire                  Left trigger .......... grenade
A ..................... jump                  B ..................... crouch
X ..................... reload                Y ..................... switch weapon
Right grip ............ melee                 Left grip ............. hold the gun with two hands
Left stick click ...... sprint                Right stick click ..... super
Menu button ........... character screen (tap), Director / map (hold)

Aim down sights: raise the gun to your eye. Two-handed: grip with the left hand near the gun.

Dual wield:
  Hold Y ............... give the left hand its own weapon (hold again to put it away)
  Tap Y ................ change the left hand's weapon
  Left trigger ......... fire the left weapon      Left grip ... grenade      X ... reload both
  Holsters ............. grip at your right hip (kinetic), left hip (energy) or over your shoulder
                         (power) to draw that weapon into that hand

Swords: held in your hand, in first person; attack with the right trigger as usual. Y passes the
  sword to the other hand; grip the hilt with the other hand to hold it with both.

Sparrow: hold X to summon it.
  Right trigger throttle, left trigger brake. Steer with the sticks, or with the handlebars: grip
  where the rider's hands are and push one forward. Left stick click boost, A get off, X / Y dodge.

Keyboard (game window focused):
  F9   VR off / on                       F7   recentre (position, direction, menus screen)
  F10  turn the view 45 degrees          F11  raise the view (cycles)
  F8   reset F10 / F11                   F5   experimental stereo (alternate eyes)
  F6   which eye the image is drawn for (centre / right / left)


--------------------------------------------------------------------------------------------------
 SETTINGS
--------------------------------------------------------------------------------------------------

SVR_Settings.txt in the game folder, read when the game starts. Every option is explained inside:
render density (sharpness vs. performance), which OpenXR runtime to use, the menu screen's size and
distance, and whether VR starts with the game.


--------------------------------------------------------------------------------------------------
 KNOWN LIMITATIONS
--------------------------------------------------------------------------------------------------

- The image is the same in both eyes (no 3D depth). F5 tries an experimental stereo mode.
- Bows do not work properly yet.
- Walking around your room moves the view, not the character: play standing or seated in place.
- Project Sunrise's own limits apply: it is an offline preservation project and not every part of
  the game works. See https://projectsunrise.dev/faq/


--------------------------------------------------------------------------------------------------
 TROUBLESHOOTING
--------------------------------------------------------------------------------------------------

- Black or frozen image in the headset: make sure the link (Quest Link etc.) is running BEFORE you
  start the game. If the headset was off, put it on and press the headset's menu/Meta button.
- The image is squashed on the PC monitor: normal, the PC window shows what the headset sees.
  Set "display_shape desktop" in SVR_Settings.txt for a normal picture on the monitor.
- Low frame rate: lower display_scale in SVR_Settings.txt (for example 0.6).
- The game closes at start with a "Destiny 2 VR" message: the DLL is in a folder that is not a
  Sunrise install. Read the message.

Reporting a bug: in <game folder>\bin\x64\Sunrise\settings.json set "file_sink": true (under
"logging"), reproduce the problem, then attach the newest file from <game folder>\bin\x64\Sunrise\logs\
and, if it exists, SVR_DualTrace.txt from the game folder. Say what you were doing.


--------------------------------------------------------------------------------------------------
 SUPPORT THE MOD
--------------------------------------------------------------------------------------------------

If you enjoy it: https://ko-fi.com/cosmictangerine  ·  https://www.patreon.com/cw/CosmicTangerine


--------------------------------------------------------------------------------------------------
 LICENSE
--------------------------------------------------------------------------------------------------

steam_api64.dll is Project Sunrise (https://github.com/stanuwu/Sunrise) with the VR mod compiled in.
Sunrise is GPL-3.0, so this build is GPL-3.0 too: see LICENSE.txt. Full source code of this exact
version: https://github.com/thecosmictangerine-cloud/destiny-2-vr (tag v{VERSION}).
Third-party components and their licences: THIRD-PARTY-NOTICES.txt.

Destiny 2 is a trademark of Bungie, Inc. This project is not affiliated with or endorsed by Bungie,
and contains no game files.
