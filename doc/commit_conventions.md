# Commit Conventions

This project follows [Conventional Commits](https://www.conventionalcommits.org).

## Format

```
<type>(<scope>): <description>
```

- `(scope)` is optional, usually a module or component name
- description starts with a lowercase letter, no period at the end, imperative mood ("add", not "added")
- keep it short, like a code comment: the subject line only; add a body only when the subject cannot say it, one or two lines

## Types

| Type       | When to use                                              | Example                                              |
|------------|----------------------------------------------------------|------------------------------------------------------|
| `feat`     | New functionality                                        | `feat(config): add JSON config parsing`              |
| `fix`      | Bug fix                                                  | `fix(log): prevent null dereference in format string` |
| `refactor` | Code change without changing behavior                    | `refactor(config): use single-exit goto cleanup`     |
| `chore`    | Maintenance: CI, scripts, settings, dependencies         | `chore(skills): add p8-error-cleanup project skill`  |
| `docs`     | Documentation only                                       | `docs: update build instructions in README`          |
| `test`     | Adding or modifying tests                                | `test(config): add edge cases for empty file`        |
| `style`    | Formatting, whitespace, semicolons (no logic changes)    | `style: apply clang-format to engine/`               |
| `perf`     | Performance optimization                                 | `perf(mtk): batch metric emissions to reduce syscalls` |
| `build`    | Build system changes                                     | `build: add linux preset to CMakePresets.json`       |
| `ci`       | CI/CD configuration                                      | `ci: add GitHub Actions workflow for macOS`          |
| `revert`   | Revert a previous commit                                 | `revert: feat(config): add JSON config parsing`      |
