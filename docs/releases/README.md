# Release evidence

One file per firmware release, `vX.Y.Z.md`: the bench runs and CI results that show the version met the
[release criteria](../release-criteria.md) for its kind, plus hand-written install notes. Each is written by
`python tools/release_evidence.py collect vX.Y.Z`, merged before the release is tagged, checked by the `release`
workflow and attached to the GitHub release. Releases before the criteria have none; their bench results are in
their release notes.
