"""Wire values as they appear in log entries and status (roles.h / link.h). tools/check_contract.py checks them
against the firmware's enums."""

GS = {"unknown": 0, "closed": 1, "open": 2, "between": 3, "fault": 4, "no_power": 5}
CAUSE = {"none": 0, "lora": 1, "external": 2}
ACT_OPEN, ACT_CLOSE = 1, 2
RES_OK, RES_ALREADY, RES_BAD, RES_NO_POWER, RES_NOT_SAVED = 0, 1, 2, 4, 5  # 3 was RES_BUSY, never produced
