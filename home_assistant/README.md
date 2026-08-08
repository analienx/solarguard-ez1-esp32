# Home Assistant boundary

Generate the package with:

```powershell
python watchdog.py generate-ha --watchdog-url http://WATCHDOG_IP --lease-guard binary_sensor.YOUR_SAFETY_DATA_HEALTHY
```

Copy `solar_watchdog.generated.yaml` into the HA project's `current/packages/` directory and merge the one generated secret line into the deployed `secrets.yaml`. Do not commit that secret.

The package renews the lease every 30 seconds only while the optional guard binary sensor is on, polls status, exposes concise diagnostic entities, and notifies on a latched or unreachable actuator state. Use the guard when stale Zigbee meter data must invoke the watchdog; this installation uses `binary_sensor.solar_phase_b_data_healthy` to cover an SLZB coordinator hang. It does not replace the existing `solar_controller.yaml`, `solar_phase_b.yaml`, or router-down Zigbee breaker automation.

Before enabling ordinary inverter-limit increases in HA, add `binary_sensor.ez1_watchdog_healthy == on` to the existing centralized write wrapper. Decreases may remain available during degraded operation. Make that HA change in the HA repository with its own impact analysis and configuration validation.

Release is intentionally explicit:

```yaml
action: rest_command.ez1_watchdog_release
data:
  latch_id: "{{ states('sensor.ez1_watchdog_latch_id') | int }}"
```

Only perform release after normal solar-control inputs are initialized. The watchdog does not raise the inverter limit; HA resumes gradually.
