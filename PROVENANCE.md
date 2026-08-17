# Source provenance and dependency policy

ALK is an original implementation. This policy applies to source code, generated source, tests, benchmarks, build scripts, vendored files, snippets, and substantial code-derived translations added to this repository.

## Default rule

Do not import, copy, adapt, translate, vendor, or derive code from another project unless that material is released under one of these licenses:

- Creative Commons Zero (CC0-1.0); or
- The Unlicense.

Code under MIT, BSD, Apache, ISC, MPL, LGPL, GPL, public-source terms, source-available terms, or no stated license is **not accepted as imported code** without the project owner's explicit permission, even if its license would otherwise be compatible with the repository's GPL-3.0 outbound license.

Documentation used only to understand public behavior is not imported code, but contributors must not reproduce protected expression from it. Clean, independent implementation from a written behavior specification is preferred.

## Exceptions

An exception must be approved explicitly and in writing by the project owner before the material enters the repository. The approval record must identify:

- upstream project and exact source URL;
- file, version, or commit;
- upstream license;
- files receiving the imported material;
- scope and date of approval.

Approved exceptions belong in `THIRD_PARTY_APPROVALS.md`. Silence, a pull-request review, or general permission to contribute is not an exception.

## Contribution declaration

By contributing, a contributor asserts that the contribution is their original work, is CC0/Unlicense material documented with its exact provenance, or is covered by a recorded owner-approved exception. Tool-generated code must satisfy the same rule; the person contributing it is responsible for verifying provenance.

The repository currently has no imported third-party code and no runtime or test dependencies.
