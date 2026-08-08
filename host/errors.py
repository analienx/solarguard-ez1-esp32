class WatchdogError(RuntimeError):
    code = "E000_UNKNOWN"

    def __init__(self, message: str):
        super().__init__(f"{self.code}: {message}")


class UnsupportedPython(WatchdogError):
    code = "E001_PYTHON_UNSUPPORTED"


class NoEspBoard(WatchdogError):
    code = "E010_NO_ESP_BOARD"


class WrongEspChip(WatchdogError):
    code = "E011_WRONG_ESP_CHIP"


class FlashFailed(WatchdogError):
    code = "E020_FLASH_FAILED"


class ProvisionFailed(WatchdogError):
    code = "E030_PROVISION_FAILED"


class EZ1Unreachable(WatchdogError):
    code = "E050_EZ1_UNREACHABLE"


class EZ1IdentityMismatch(WatchdogError):
    code = "E051_EZ1_IDENTITY_MISMATCH"


class EZ1SchemaInvalid(WatchdogError):
    code = "E052_EZ1_SCHEMA_INVALID"


class ClampWriteFailed(WatchdogError):
    code = "E060_CLAMP_WRITE_FAILED"


class ClampVerifyFailed(WatchdogError):
    code = "E061_CLAMP_VERIFY_FAILED"


class LeaseTestFailed(WatchdogError):
    code = "E070_LEASE_TEST_FAILED"


class BleProtocolUnknown(WatchdogError):
    code = "E090_BLE_PROTOCOL_UNKNOWN"