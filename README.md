# MISRC GUI

<img width="150" height="150" alt="GUI_Icon" src="assets/Icons/MISRC_Icon.png" />

> (Multiple Input Simultaneous RF Capture Graphical User Interface) 

A universal cross platform GUI tool for interfacing with and visualising monitoring and control of FM RF archival focused, capture device workflows.

### Current Supported Hardware

- MISRC (v1.0-v1.5a / native v2.5)
- CXADC (single cards and Clockgen Mod with sound)
- HSDAOH
- FX3 (Generic tinkering firmware support)
- DdD (DomesDay Duplicator)
- FX3ADC (100mhz MUSE capture device)

### Linux setup note (CXADC DC controls)

For CXADC `DC` up/down controls to work without root, the sysfs parameter files must be writable by the `video` group.

One-time fix for the current boot:

```bash
sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*
```

Verify:

```bash
ls -l /sys/class/cxadc/cxadc*/device/parameters/center_offset
```

Expected group is `video` (e.g. `root video`).


## Downloads


Downloads can be found on the [releases page](https://github.com/harrypm/MISRC-GUI/releases).
- Windows
- MacOS
- Linux
- Android (arm64 APK, very alpha)

x86 (AMD/Intel) and ARM64 (Apple M, Snapdragon, RockChip) are fully supported and intended for long-term support. 


## Visual Overview & Use


The MISRC GUI, the layout is a simplified command and control system, designed for a single 9-24” monitor both touch & non-touch compatible, with a two layer only rule of design meaning there is no more than one sub menu per button press or drop down selection.


<img width="1427" height="752" alt="image" src="https://github.com/user-attachments/assets/7a715ce0-872b-4b38-ab31-e668505b8dc6" />

The main GUI window from beginning from the top row

- Information button
- Metadata button
- Device selection box 
- Device mode selection box 
- Audio monitoring enable and disable 
- Audio levels 
- Timer and level stop control 
- Record button

The record timer system is self fail proof, you cannot set a time lower than your current duration and instantly stop capture by accident, it will count the total timer with a discount of the current duration passed, and has to be armed with a manual button click.

## Information Page 

<img width="473" height="388" alt="image" src="https://github.com/user-attachments/assets/83cfc78d-757c-467f-a74a-5755167ae5d5" />

- A/B  Swap for older V1.5a users
- V4L2 Device discovery for Linux
- Core Pinning (Linux Only)
- Memory Budget (Allows you to limit/raise the buffers for higher stability on low end or high end systems) 

The GUI is built with several layers of fallback and prevention measures to stop hardware issues on an OS level from interfering with your capture such as spillover if there is a slowdown in drive or encoding performance, the recommended amount of RAM ideally is 8GB DDR3 2400Mhz or better, of course on faster ARM64 chips this becomes less of a concern but production stations should have ideally no less then 16GB total. 

## Record & Audio Monitoring

<img width="617" height="46" alt="image" src="https://github.com/user-attachments/assets/af6ad652-ca67-4f5d-bf35-7635d62dd187" />

Record button is clear when not in use.

Record button is RED when capturing is in use.

The record button will turn orange and state finalising when a file is still being processed i.g adding timing header information or encoding from spill over or memory.

Audio monitoring has on/off and CH 1/2 or Ch 3/4 switching and level indicators in Green/Yellow/Red for visual loudness level. 

## Timer and Level Auto Stop

<img width="436" height="340" alt="image" src="https://github.com/user-attachments/assets/d5a7ea02-b2dc-41f6-a2db-d24783cde1c9" />

Timer mode allows for HH:MM:SS timing to stop the capture, with an arm/disarm system but will append current duration to the total timer duration and will not allow you to accidentality stop your capture if you forgot to set the timer beforehand. 

Level Autostop, this allows for you to set a overall % level alongside a timer to automatically stop captures when a tape is clearly reached its end and no active signal is being captured thus presenting a drastically lower level, users should be careful with this to prevent re-runs if tapes have spaced recordings.

## Settings Page

<img width="763" height="647" alt="Screenshot from 2026-08-10 16-58-26" src="https://github.com/user-attachments/assets/db2d99a8-a712-4e24-9d6d-e2fdb2886428" />

- Auto File Naming
- Auto File Date Stamping
- Stop on dropout mode
- Base Name and Output
- A/B RF Capture On/Off
- FLAC/RAW PCM Encoding Control
- Bit-Depth Selection Control (for MISRC/HSDAOH, CXADC is assumed 8-bit)
- FLAC Level & Threads Control
- Stereo/Mono/Quad channel record control for audio (You can select multiple options) 
- Resampling Control for A/B (VHS config 20msps video 10msps hifi shown)
- Playback Input Files

## Scopes & Plugins

There is a unified plugin system for deploying decoders.

Included currently are the following viewer modes.

- Stream Waveform (similar to an oscilloscope) 
- FTT (visualises peaks of signals carriers i.g video/hifi or your local FM radio)
- CVBS (Basic composite video decoder Luma B/W only currently)

<img width="140" height="132" alt="image" src="https://github.com/user-attachments/assets/8a3d0632-5e9b-4c15-985e-73805c62f0e5" />


Experimental support for [Tape-Decode](https://github.com/harrypm/tape-decode-rust) (A Rust re-write of vhs-decode) is also a working progress, this plugin hopes to allow for a quick is this working inspection and in the future potentially direct to file decoding however currently this does have massive limitation implications and performance cost implications, so it's not a high priority over the stability of the core feature of seeing there is something and getting it safely captured to file. 

There is plans for a basic quality hi-fi audio decoder mode based off of the GNU radio script, allowing for directional test point finding and quick testing of sanity of if there is a HiFi FM signal present.

## During Capture Readout 

Statistics per channel will be available on the right hand side. 

- Peak RF Level
- Clipping events observed + & - range
- Errors with feed

### Recording statistics

- Duration (HH.MM.SS)
- RAW Data Handled
- Encoded FLAC file size
- Compression Ratio (I.g 8.7x)

<img width="191" height="547" alt="image" src="https://github.com/user-attachments/assets/790a4ca2-51ed-409a-833b-bdde6c248c06" />

After a capture is finished these values will be persistent until a new capture starts or the application is closed.

Persistence config, once you have configured your settings a global configuration file will be saved and this will carry forward onto new versions or older test builds, this allows you to quickly change between builds to test things or to just instantaneously update to the latest version.

## General Monitoring 

- Sync Status  - confirms the current state of connectivity. 
- XXX MSPS     - shows your current rate of the hardware capture device I.g 40msps or 100msps
- Samples      - xxxxGB gives you a rolling number of how many samples have been fed into the application during the current session. 
- Frames       - like samples shows you how many samples are in a frame counter. 
- Missed       - shows you how many frames of data have been missed. 
- Errors       - shows you how many hard dropout or encoder dropout errors there are. 
- RF Buffer    - shows the current level of the ring buffer for RF feeds. 
- Audio Buffer - shows the current level of the ring buffer for audio feeds.

Understanding the waveform scale. 

There are two visualisation modes currently implemented. 

- Level Bar (vertical colour indicator)
- Waveform Line
- Waveform Phosphor

<img width="154" height="86" alt="image" src="https://github.com/user-attachments/assets/dcb5f58c-f91f-4ff8-b67f-c0a47a9e1ff1" />

On the zero line that is your DC offset position your signal should be level with this position to begin with irrespective of your gain level of the signal of input.

Ideal saturation range at 8-bit is typically within the plus +0.5 and -0.5 range of visualization, of course please do confirm with an oscilloscope and your recorded files before committing to long duration archival visualizations are of a decimated amount i.g (100/1000th samples) of data not an absolute of what is being captured to file.

Trigger modes like an oscilloscope can be done in the simple following. 

<img width="144" height="215" alt="image" src="https://github.com/user-attachments/assets/8ca5cc52-282b-4481-bb73-524bcbeb3cfd" />

Each channel can be triggered by any other channel by selecting the channel trigger mode. 

Channels:

- Ch1 (RF)
- Ch2 (RF)
- Ch3 (MISRC Audio Ch3 or Clockgen Mod Mainboard Headswitching input)

Modes:

- Rising
- Falling
- Sync
- CVBS

There are plans to expand upon these to cover a full range of trigger modes you would typically see inside of an Sigilent/Rigol oscilloscope.

## Capture Ingest Metadata

The Text Icon opens the metadata tab for logging information about your capture.

<img width="665" height="486" alt="image" src="https://github.com/user-attachments/assets/9102a6cb-23c1-4cc7-8349-df923283bc57" />

## Logging 

MISRC GUI has a perpetual logging system, as record button has is pressed your exact system and record config is saved to your log file along with any metadata saved in the fields provided under the metadata window, and any configuration changes overall, this will also log any errors or buffering issues such as spillover usage to a temporary file, It will also confirm a file is properly encoded and saved so you know 100% the buffers were cleared correctly.

It is highly recommended to preserve these files alongside your captures, however unlike previous capture applications you're encoded FLAC files we'll have the correct duration on both the RF and standard audio files, and can have common metadata embedded into them this allows for tools such as [FLAC Chop](https://github.com/harrypm/FLAC-Chop) to easily cut up or target or just remove dead space at the start and end of your capture sets.

This means no need for doing advanced math, simply just note the exact input and output timing positions you wish to make cuts and copy and paste across the different files of your capture sets, however you should also make a note inside of the log file if you do this to your files otherwise the information won't match up and maybe caught by future automated systems for disqualification. 

MediaInfo Metadata Example:

`````
Format :	FLAC
Format/Info :	Free Lossless Audio Codec
File size :	18.0 MiB
Duration :	6 min 55 s
Overall bit rate mode :	Variable
Overall bit rate :	363 kb/s
DURATION_SECONDS :	415.838974
LENGTH :	415839
RF_TOTAL_SAMPLES :	4158389740
RF_SAMPLE_RATE :	10000000
RF_SAMPLE_RATE_KHZ :	10000
`````

Log Example:

``````
[2026-07-31 03:00:26] [INFO] MISRC capture log started (FLAC)
[2026-07-31 03:00:26] [INFO] computer_name: THE-RIPPER
[2026-07-31 03:00:26] [INFO] computer_model_name: unknown
[2026-07-31 03:00:26] [INFO] computer_cores: 32
[2026-07-31 03:00:26] [INFO] user_name: Harry
[2026-07-31 03:00:26] [INFO] operating_system_VERSION: Windows
[2026-07-31 03:00:26] [INFO] misrc_tools_version: dev-6d31f96
[2026-07-31 03:00:26] [INFO] datetime_start: 2026-07-31T03:00:26
[2026-07-31 03:00:26] [INFO] capture_log_path: C:\Users\Harry\Desktop/Test_Capture_2026.07.31_03.00.26_misrc_capture.log
[2026-07-31 03:00:26] [INFO] capture_base_name: Test_Capture
[2026-07-31 03:00:26] [INFO] output_path: C:\Users\Harry\Desktop
[2026-07-31 03:00:26] [INFO] capture_device_name: MS2130
[2026-07-31 03:00:26] [INFO] capture_device_type: hsdaoh
[2026-07-31 03:00:26] [INFO] capture_format: FLAC
[2026-07-31 03:00:26] [INFO] Capture channels: A=on B=on
[2026-07-31 03:00:26] [INFO] CVBS preview state: A=off B=off
[2026-07-31 03:00:26] [INFO] RF settings: bitsA=8 bitsB=8 resampleA=on(20000.0 kHz) resampleB=on(10000.0 kHz)
[2026-07-31 03:00:26] [INFO] Capture limits: capture_limit_seconds=0 record_limit_seconds=0 (record_timer_disarmed)
[2026-07-31 03:00:26] [INFO] Audio monitor: playback=off monitor_ch34=off misrc_mode=on
[2026-07-31 03:00:26] [INFO] MISRC V1.5/V2.5 A/B swap override: off
[2026-07-31 03:00:26] [INFO] Dropout handling: stop_on_dropout=off
[2026-07-31 03:00:26] [INFO] Ingest metadata project: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata tape_id: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata tape_format: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata tape_size: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata tape_speed: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata tape_condition: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata operator: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata location: (empty)
[2026-07-31 03:00:26] [INFO] Ingest metadata notes: (empty)
[2026-07-31 03:00:26] [INFO] FLAC settings: level=8 verify=off threads=8
[2026-07-31 03:00:26] [INFO] FLAC affinity: enabled=off cpu_list=(none) support=unsupported
[2026-07-31 03:00:26] [INFO] FILE_PATH_A: C:\Users\Harry\Desktop/rfA_Test_Capture_2026.07.31_03.00.26_8-bit_20msps.flac
[2026-07-31 03:00:26] [INFO] FILE_PATH_B: C:\Users\Harry\Desktop/rfB_Test_Capture_2026.07.31_03.00.26_8-bit_10msps.flac
[2026-07-31 03:00:26] [INFO] Audio outputs: 4ch=off 2ch12=on 2ch34=off
[2026-07-31 03:00:26] [INFO] AUDIO_2CH_12_FILE_PATH: C:\Users\Harry\Desktop/Test_Capture_2026.07.31_03.00.26_Baseband_stereo_ch1_ch2.wav
[2026-07-31 03:00:32] [INFO] Recording stopped: duration=5.33s (00.00.05) rawA=394.00 MB (413138944 bytes) rawB=394.00 MB (413138944 bytes) waits=0 drops=0
[2026-07-31 03:00:32] [INFO] Capture time: 5.17s (00.00.05)
[2026-07-31 03:00:32] [INFO] Processing time: 0.16s (00.00.00)
[2026-07-31 03:00:32] [INFO] Output data: compressedA=9.31 MB (9763449 bytes) compressedB=9.15 MB (9599031 bytes)
[2026-07-31 03:00:32] [INFO] Compression ratio: total_raw=788.00 MB (826277888 bytes) total_compressed=18.47 MB (19362480 bytes) ratio=42.674x
[2026-07-31 03:00:32] [INFO] datetime_end: 2026-07-31T03:00:32
[2026-07-31 03:00:32] [INFO] Session complete
```````

## History

- December 2025 - Initial version presented by AlessandroAU (back and forth tinkering begins)
- February 2026 - First testing version released by Harry Munday
- April 4th 2026 - V1.0.0 Release (Basic HSDAOH support re-working by machcnz and vaguely stable)
- June 3rd 2026  - V1.0.7 Release first overall stable production release
- August 9th 2026 - Official public pushing for adoption and edge case bug finding! 
- August 13th 20206 - Official release!
