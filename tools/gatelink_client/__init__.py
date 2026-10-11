"""Client for GateLink boards' JSON console (docs/console.md), shared by tools/gatelink.py and the e2e suite.

- `board`: a board's USB console (requests, events), the UART tap, finding and opening boards by role.
- `timeline`: the merged, time-ordered record of everything heard, which `Board` writes its events to.
- `wire`: the firmware's enum values as they appear in log entries and status (roles.h / link.h).
- `keybackup`: key ids, the weak-key rule and encrypted key backups (docs/key-management.md); no board needed.

tools/ is on sys.path when tools/gatelink.py runs, and the suite adds it (`pythonpath` in tests/e2e/pytest.ini).
"""
