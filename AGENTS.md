# Repository guidance

## Windows COM tests

- Run the `shell_selection` test interactively as the signed-in Windows user. COM registration and activation must run in the same user context.
- Running this test from a non-interactive agent session can fail with `0x80040154` (`REGDB_E_CLASSNOTREG`). This is a test execution context issue, not evidence of a product defect.
- If this occurs, report that interactive validation is required. Do not change application code or COM registration to work around the non-interactive test context, and do not claim the test passed.
- See `BUILDING.md` for the test setup and other validation requirements.

## Targeted validation without desktop interruptions

- Run only tests that exercise the behavior changed. Do not rerun unrelated suites
  for routine edits. A build compiles only unless `-Test` names targeted checks.
- Do not run visible UI tests while the user is working. Only run a relevant UI
  test when the user explicitly requests that visible validation in the current
  task; otherwise report it as pending and use targeted non-UI checks.
- Visible tests require `scripts/test-ui.ps1 -Test <name>`. For reader changes,
  choose `-ReaderFormat docx` for DOCX; do not use `all` for a single-format change.
- Never enable `BOOKS_RUN_INTERACTIVE_TESTS` globally or persist it in user settings.
  The scoped script restores its previous process environment on completion.
