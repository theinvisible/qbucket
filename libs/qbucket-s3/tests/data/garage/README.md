# Captured from Garage v2.4.1 (tests/docker) on 2026-10-20.
- error-invalid-signature.xml: GET / signed with a wrong secret (HTTP 403).
- put-versioning-not-implemented.xml: PUT ?versioning (Status Enabled) — HTTP 501. GET ?versioning on the same bucket answers 200 with an empty <VersioningConfiguration/>: a stub, hence GarageProfile::reviewProbe.
- GET ?website without a configuration: HTTP 204, empty body.
