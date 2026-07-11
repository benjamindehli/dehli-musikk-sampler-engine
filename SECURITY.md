# Security Policy

## Supported versions

Only the latest state of the `main` branch is supported. The engine ships inside the Dehli Musikk plugins, and fixes reach users through new plugin releases.

## Reporting a vulnerability

Please do not report security issues in public GitHub issues.

Use GitHub's private vulnerability reporting instead: open the Security tab of this repository and choose Report a vulnerability. You should get a first response within a week.

## Scope

The engine parses JSON manifests and decodes FLAC audio that are bundled into the plugins at build time, so ordinary use does not expose it to untrusted input. Reports are still welcome for anything that could misbehave on malformed manifest or audio data, since the format is also meant for hand authoring, as well as for issues in the build and packaging scripts.
