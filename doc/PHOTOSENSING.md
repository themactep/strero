# Photosensing

```
{
  "enabled": "Enable/disable automatic day/night mode switching",
  "night_iso_threshold": "ISO value threshold for switching to night mode",
  "night_count_threshold": "Number of consecutive readings above night ISO threshold before switching",
  "day_iso_threshold": "Primary ISO threshold for day mode detection",
  "day_gb_gain_offset": "GB gain offset required above recorded value for day mode",
  "day_gb_gain_threshold": "Secondary GB gain threshold for day mode",
  "day_iso_secondary_threshold": "Secondary ISO threshold for day mode",
  "day_count_threshold": "Number of consecutive readings meeting day conditions before switching",
  "polling_interval_ms": "Interval between sensor readings in milliseconds",
  "ircut_enabled": "Enable/disable IR cut filter control",
  "ircut_day_state": "IR cut filter state for day mode (1=enabled, 0=disabled)",
  "ircut_night_state": "IR cut filter state for night mode (1=enabled, 0=disabled)",
  "ircut_gpio1": "First GPIO pin for IR cut control",
  "ircut_gpio2": "Second GPIO pin for IR cut control",
  "gb_gain_record_init": "Initial GB gain record value",
  "gr_gain_record_init": "Initial GR gain record value",
  "debug_logging": "Enable detailed debug logging of photosensing values"
}
```
