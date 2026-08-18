# Security Policy

## Supported versions

ALK is currently pre-release software. Security fixes are applied only to the latest revision of the `main` branch until the project begins publishing versioned releases.

| Version | Supported |
| --- | --- |
| Latest `main` | Yes |
| Tagged releases | None published |

The current interpreter is a language-foundation prototype and has not received a production security audit. Do not use it as a security boundary or execute untrusted ALK programs in a privileged process.

## Reporting a vulnerability

Do not open a public issue containing vulnerability details, proof-of-concept code, secrets, or information that would make exploitation easier.

Use GitHub's private vulnerability reporting for this repository:

1. Open the repository's **Security** page.
2. Select **Report a vulnerability**.
3. Submit the report privately.

If private vulnerability reporting is unavailable, open a public issue requesting a private contact channel without including technical details. A maintainer will arrange a private discussion.

Please include, when applicable:

- the affected commit or version;
- operating system, architecture, compiler, and build configuration;
- a minimal reproduction or malformed input;
- expected and observed behavior;
- the security impact and required preconditions;
- any suggested mitigation.

## Response targets

The project aims to acknowledge a report within seven days and provide an initial assessment within fourteen days. These are best-effort targets for a pre-release project, not guaranteed service-level commitments.

After validation, maintainers will coordinate remediation and disclosure with the reporter. Please avoid public disclosure until a fix or reasonable mitigation is available and a disclosure date has been agreed upon.

## Security-relevant areas

Reports are especially useful for:

- memory corruption or undefined behavior in the lexer, parser, evaluator, or value runtime;
- crashes or resource exhaustion caused by crafted source or JSON input;
- unintended file, process, or host access;
- incorrect privilege or trust-boundary assumptions;
- future bytecode verification, JIT, garbage collection, WebAssembly, DOM, and browser-host interfaces.

Ordinary bugs, feature requests, and documentation corrections may be reported through the public issue tracker.
