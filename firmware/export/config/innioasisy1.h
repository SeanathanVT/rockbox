/*
 * This config file is for the Innioasis Y1.
 *
 * SoC:     MediaTek MT6572 (dual Cortex-A7, ARMv7-A + NEON + VFPv4)
 * Kernel:  Linux 3.4.x (Android 4.2.2 base; minimal-Linux rootfs envisioned)
 * Panel:   480x360 landscape, 32 bpp XRGB8888, /dev/graphics/fb0 (mtkfb)
 * Audio:   MediaTek custom HAL on /dev/eac (NOT ALSA — see docs/audio-stack.md)
 * Input:   /dev/input/event0..4 (mtk-kpd, ACCDET, mtk-tpd, mtk-tpd-kpd, AVRCP)
 * Battery: /sys/class/power_supply/battery/{capacity,batt_vol,status,...}
 */

#define MODEL_NUMBER 125
#define MODEL_NAME   "Innioasis Y1"

/* Hosted Linux, not Android-app */
#ifndef SIMULATOR
#define CONFIG_PLATFORM (PLATFORM_HOSTED)
#endif

#define HAVE_FPU

/* LCD */
#define HAVE_LCD_COLOR
#define HAVE_LCD_ENABLE
#define HAVE_LCD_SHUTDOWN
#define LCD_WIDTH       480
#define LCD_HEIGHT      360
#define LCD_DPI         160
#define LCD_DEPTH       32
#define LCD_PIXELFORMAT XRGB8888
#define CONFIG_LCD      LCD_INGENIC_LINUX  /* fbdev-backed; reused from hibylinux family */

#define HAVE_ALBUMART
#define HAVE_BMP_SCALING
#define HAVE_JPEG
#define HAVE_TAGCACHE

/* Backlight via /sys/class/leds/lcd-backlight (verified on device, max=255) */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS
#define MIN_BRIGHTNESS_SETTING     1
#define MAX_BRIGHTNESS_SETTING     255
#define BRIGHTNESS_STEP            5
#define DEFAULT_BRIGHTNESS_SETTING 200
#define CONFIG_BACKLIGHT_FADING    BACKLIGHT_FADING_SW_SETTING

/* Buttons: 5 capacitive nav buttons via mtk-tpd; power via mtk-kpd */
#define CONFIG_KEYPAD              INNIOASIS_Y1_PAD
#define HAVE_HEADPHONE_DETECTION

/* Capacitive nav row is not a wheel; declare NO_BUTTON_LR so list/wps navigation
 * uses PREV/NEXT instead of LEFT/RIGHT semantics. */
#define NO_BUTTON_LR

/* CPU: 1.3 GHz Cortex-A7 (per /sys cpufreq scaling_max_freq, verify on-device) */
#define CPU_FREQ 1300000000

/* Memory budgets (tune after first build) */
#define CODEC_SIZE          0x100000  /* 1 MB */
#define PLUGIN_BUFFER_SIZE  0x200000  /* 2 MB */

/* Battery — sysfs path matches Android convention, koensayr-verified */
#define CONFIG_BATTERY_MEASURE (VOLTAGE_MEASURE | PERCENTAGE_MEASURE)
#define BATTERY_TYPES_COUNT      1
#define BATTERY_CAPACITY_DEFAULT 1500    /* approx; Y1 doesn't expose energy_full_design */
#define BATTERY_CAPACITY_MIN     1500
#define BATTERY_CAPACITY_MAX     1500
#define BATTERY_CAPACITY_INC     0
#define BATTERY_DEV_NAME         "battery"
#define POWER_DEV_NAME           "usb"

#define CURRENT_NORMAL    100
#define CURRENT_BACKLIGHT 180
#define CURRENT_MAX_CHG   500

/* Power / charging — Android (or our init) owns charge management, we only monitor */
#define HAVE_USB_POWER
#define CONFIG_CHARGING CHARGING_MONITOR
#define HAVE_SW_POWEROFF

/* RTC handled by hosted-Linux layer */
#define CONFIG_RTC RTC_HOSTED

/* Storage: HOSTFS only — /storage/sdcard0 (internal) + /storage/sdcard1 (microSD) */
#define CONFIG_STORAGE          STORAGE_HOSTFS
#define HOSTFS_VOL_DEC          "microSD"
#define HAVE_STORAGE_FLUSH
#define HAVE_MULTIDRIVE
#define NUM_DRIVES              2
#define HAVE_HOTSWAP
#define HAVE_HOTSWAP_STORAGE_AS_MAIN

/* Audio — MTK custom (/dev/eac). Driver is stub until RE complete (see docs/audio-re-progress.md) */
#define HAVE_Y1MTK_CODEC
#define HAVE_SW_VOLUME_CONTROL
#define HAVE_SW_TONE_CONTROLS

/* The HAL's primary output is 44.1k S16_LE stereo (per audio_policy.conf).
 * Lock until we discover whether /dev/eac accepts other rates directly. */
#define HW_SAMPR_CAPS SAMPR_CAP_44

/* ROLO */
#define BOOTFILE_EXT "y1"
#define BOOTFILE     "rockbox." BOOTFILE_EXT
#define BOOTDIR      "/.rockbox"

/* USB (from android_usb/idVendor / idProduct) */
#define USB_VID_STR "0BB4"
#define USB_PID_STR "0C03"
