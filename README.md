# SteelBattalionDriver

A Windows 64bit Driver for the Steel Battalion controller

This driver will let you use your old Steel Battalion controller as a USB-HID gamepad in a windows system.

## What this fork changes

This fork makes the gear lever and tuner dial usable in apps that read DirectInput's standard joystick layout, such as JoyToKey, and adds panel light control.

| Control | Reported as |
|---|---|
| Gear lever R, N, 1-5 | Buttons 40-46, with only the current gear held |
| Tuner dial positions 0-15 | Buttons 47-62, with only the current position held |

The Shifter and Weapons Select axes are gone, so apps that read those two axes directly need to use the buttons instead. In JoyToKey, set the number of buttons to at least 62.

The driver also runs the panel lights by itself, with no app needed:

- The light for the current gear is on, and the other gear lights are off.
- Every other button light is on, and goes off only while that button is held.

### Download

The driver is on the [releases page](https://github.com/mxpbxb/SteelBattalionDriver/releases). It's signed with a test certificate, not a trusted one, so Windows must be in test signing mode with Secure Boot off.

### Building and signing

Builds with Visual Studio 2022 and the Windows Driver Kit. Build from a short path, as long paths break the build tools. A build needs your own test certificate, and Windows must be in test signing mode to load it.

## How to install the driver

Read [How to use the Steel Battalion Controller on Windows 10](https://medium.com/@oscarsc/how-to-use-the-steel-battalion-controller-on-windows-10-2e21aa8ccd28)

## Where does it work

If the game you want to use the controller with lets you configure generic gamepads or joysticks you will probably be able to use the Steel Battalion 
controller without any modifications to the game.

If you are using steam and the game only supports XInput controllers (Xbox-like gamepads) you can map the Steel Battalion
controller from the steam gamepad preferences.

## About the project

This started as a side project to enable to use the Steel Battalion controller in [BH Trials](https://store.steampowered.com/app/1067080/BH_Trials/), a game I am working on where you take the controls of a digger.

I also wrote a technical article about [Writing a driver for the Steel Battalion controller](https://medium.com/@oscarsc/writing-a-driver-for-the-steel-battalion-controller-e1e4311f1a40)
