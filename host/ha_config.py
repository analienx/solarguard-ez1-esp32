from __future__ import annotations

import secrets
import re
from pathlib import Path

from .models import validate_secret


PACKAGE_TEMPLATE = '''# Generated EZ1 watchdog package. Do not commit the matching secret.
rest_command:
  ez1_watchdog_lease:
    url: "{base_url}/api/v1/lease"
    method: POST
    headers:
      Authorization: !secret ez1_watchdog_authorization
      Content-Type: application/json
    payload: '{{}}'
    timeout: 5

  ez1_watchdog_release:
    url: "{base_url}/api/v1/release"
    method: POST
    headers:
      Authorization: !secret ez1_watchdog_authorization
      Content-Type: application/json
    payload: '{{"latch_id": {{{{ latch_id | int }}}}}}'
    timeout: 5

rest:
  - resource: "{base_url}/api/v1/status"
    method: GET
    headers:
      Authorization: !secret ez1_watchdog_authorization
    scan_interval: 30
    timeout: 5
    sensor:
      - name: EZ1 Watchdog State
        unique_id: ez1_watchdog_state
        value_template: "{{{{ value_json.state }}}}"
        json_attributes:
          - boot_id
          - uptime_s
          - lease_age_ms
          - failsafe_latched
          - latch_id
          - latch_reason
          - wifi
          - ez1
          - last_action
          - override_detected_count
          - reset_reason

template:
  - binary_sensor:
      - name: EZ1 Watchdog Healthy
        unique_id: ez1_watchdog_healthy
        device_class: connectivity
        state: >-
          {{{{ is_state('sensor.ez1_watchdog_state', 'ARMED') }}}}
        availability: >-
          {{{{ states('sensor.ez1_watchdog_state') not in ['unknown', 'unavailable'] }}}}
      - name: EZ1 Watchdog BLE Ready
        unique_id: ez1_watchdog_ble_ready
        state: >-
          {{{{ (state_attr('sensor.ez1_watchdog_state', 'ez1') or {{}}).get('ble_ready', false) }}}}
      - name: EZ1 Watchdog BLE Identity Verified
        unique_id: ez1_watchdog_ble_identity_verified
        state: >-
          {{{{ (state_attr('sensor.ez1_watchdog_state', 'ez1') or {{}}).get('ble_seen_recently', false) }}}}
  - sensor:
      - name: EZ1 Watchdog Lease Age
        unique_id: ez1_watchdog_lease_age
        unit_of_measurement: s
        state: >-
          {{{{ (state_attr('sensor.ez1_watchdog_state', 'lease_age_ms') | int(0) / 1000) | round(0) }}}}
      - name: EZ1 Watchdog Latch ID
        unique_id: ez1_watchdog_latch_id
        state: "{{{{ state_attr('sensor.ez1_watchdog_state', 'latch_id') | int(0) }}}}"
      - name: EZ1 Watchdog WiFi RSSI
        unique_id: ez1_watchdog_wifi_rssi
        unit_of_measurement: dBm
        device_class: signal_strength
        state: "{{{{ (state_attr('sensor.ez1_watchdog_state', 'wifi') or {{}}).get('rssi', 0) }}}}"
      - name: EZ1 Watchdog Override Count
        unique_id: ez1_watchdog_override_count
        state: "{{{{ state_attr('sensor.ez1_watchdog_state', 'override_detected_count') | int(0) }}}}"
      - name: EZ1 Watchdog Last Action
        unique_id: ez1_watchdog_last_action
        state: "{{{{ (state_attr('sensor.ez1_watchdog_state', 'last_action') or {{}}).get('type', 'NONE') }}}}"
      - name: EZ1 Watchdog Last Action Path
        unique_id: ez1_watchdog_last_action_path
        state: "{{{{ (state_attr('sensor.ez1_watchdog_state', 'last_action') or {{}}).get('path', 'NONE') }}}}"
      - name: EZ1 Watchdog Last Failure
        unique_id: ez1_watchdog_last_failure
        state: "{{{{ (state_attr('sensor.ez1_watchdog_state', 'last_action') or {{}}).get('result', 'NONE') }}}}"
      - name: EZ1 Watchdog Last BLE Result
        unique_id: ez1_watchdog_last_ble_result
        state: "{{{{ (state_attr('sensor.ez1_watchdog_state', 'ez1') or {{}}).get('last_ble_result', 'NONE') or 'NONE' }}}}"
      - name: EZ1 Watchdog Reset Reason
        unique_id: ez1_watchdog_reset_reason
        state: "{{{{ state_attr('sensor.ez1_watchdog_state', 'reset_reason') or 'UNKNOWN' }}}}"

automation:
  - id: ez1_watchdog_renew_safety_lease
    alias: EZ1 Watchdog - Renew Safety Lease
    mode: single
    triggers:
      - trigger: homeassistant
        event: start
      - trigger: time_pattern
        seconds: "/{lease_interval_s}"
    actions:
{lease_guard_condition}      - action: rest_command.ez1_watchdog_lease
        response_variable: lease_response
        continue_on_error: true
      - if:
          - condition: template
            value_template: >-
              {{{{ lease_response is not defined or lease_response.status != 200 }}}}
        then:
          - action: persistent_notification.create
            data:
              notification_id: ez1_watchdog_lease_failed
              title: EZ1 watchdog lease failed
              message: >-
                The watchdog did not accept the safety lease
                (HTTP {{{{ lease_response.status if lease_response is defined else 'no response' }}}}).
                If this persists, the watchdog will trip or is not running.
        else:
          - action: persistent_notification.dismiss
            data:
              notification_id: ez1_watchdog_lease_failed

  - id: ez1_watchdog_offline_notification
    alias: EZ1 Watchdog - Offline Notification
    mode: single
    triggers:
      - trigger: state
        entity_id: sensor.ez1_watchdog_state
        to:
          - unavailable
          - unknown
          - UNPROVISIONED
        for: "00:03:00"
    actions:
      - action: persistent_notification.create
        data:
          notification_id: ez1_watchdog_offline
          title: EZ1 watchdog offline
          message: >-
            The watchdog status has been {{{{ states('sensor.ez1_watchdog_state') }}}} for
            three minutes. Fail-safe protection may be inactive (powered off, crashed,
            unprovisioned after NVS recovery, or unreachable). Check the XIAO over USB.

  - id: ez1_watchdog_critical_state_notification
    alias: EZ1 Watchdog - Critical State Notification
    mode: single
    triggers:
      - trigger: state
        entity_id: sensor.ez1_watchdog_state
    conditions:
      - condition: template
        value_template: >-
          {{{{ states('sensor.ez1_watchdog_state') in ['FAILSAFE_LATCHED', 'ACTUATOR_UNREACHABLE'] }}}}
    actions:
      - action: persistent_notification.create
        data:
          notification_id: ez1_watchdog_critical
          title: EZ1 watchdog requires attention
          message: >-
            State: {{{{ states('sensor.ez1_watchdog_state') }}}};
            latch ID: {{{{ state_attr('sensor.ez1_watchdog_state', 'latch_id') }}}}.
            Returning leases do not release the clamp.
'''


def generate(
    output_dir: Path,
    base_url: str,
    lease_interval_s: int,
    secret: str | None = None,
    lease_guard: str | None = None,
) -> tuple[Path, Path, str]:
    if not base_url.startswith("http://") or base_url.endswith("/"):
        raise ValueError("watchdog URL must look like http://192.168.1.60 without a trailing slash")
    if not 1 <= lease_interval_s <= 59:
        raise ValueError("time-pattern lease interval must be between 1 and 59 seconds")
    if lease_guard and not re.fullmatch(r"binary_sensor\.[a-z0-9_]+", lease_guard):
        raise ValueError("lease guard must be a binary_sensor entity ID")
    token = validate_secret(secret or secrets.token_hex(32))
    output_dir.mkdir(parents=True, exist_ok=True)
    package = output_dir / "solar_watchdog.generated.yaml"
    secret_file = output_dir / "secrets.generated.txt"
    package.write_text(
        PACKAGE_TEMPLATE.format(
            base_url=base_url,
            lease_interval_s=lease_interval_s,
            lease_guard_condition=(
                "      - condition: state\n"
                f"        entity_id: {lease_guard}\n"
                '        state: "on"\n'
                if lease_guard
                else ""
            ),
        ),
        encoding="utf-8",
    )
    secret_file.write_text(f'ez1_watchdog_authorization: "Bearer {token}"\n', encoding="utf-8")
    return package, secret_file, token
