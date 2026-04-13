# Git Workflow

## Branching Strategy

We use a simplified two-branch workflow:

```
main  ←── stable releases, tagged with version numbers
  │
  └── dev  ←── active development, all feature work lands here
```

### Branch Rules

| Branch | Purpose | Merges from | Merges to |
|--------|---------|-------------|-----------|
| `main` | Releases only | `dev` | — |
| `dev` | Active development | feature branches / direct commits | `main` |

### Workflow

1. **Daily development** happens on `dev`
   - Small changes can be committed directly to `dev`
   - Larger features can use short-lived feature branches merged into `dev`

2. **When ready to release**, merge `dev` into `main`:
   ```bash
   git checkout main
   git merge dev
   git tag -a v1.0.0 -m "Release v1.0.0: description"
   git push origin main --tags
   ```

3. **Continue development** on `dev`:
   ```bash
   git checkout dev
   ```

## Tagging & Releases

We use **semantic versioning** with a `v` prefix:

```
v{MAJOR}.{MINOR}.{PATCH}
```

| Component | When to increment |
|-----------|-------------------|
| MAJOR | Breaking changes to API or on-disk format |
| MINOR | New features, backward compatible |
| PATCH | Bug fixes, security patches |

### Creating a Release

```bash
# Ensure dev is clean and tested
git checkout dev
make test

# Merge to main
git checkout main
git merge dev

# Tag the release
git tag -a v1.0.0 -m "Release v1.0.0: AES-XTS userspace library"

# Push everything
git push origin main --tags

# Back to development
git checkout dev
```

### Pre-release Tags (Optional)

For testing releases before they're final:

```
v1.0.0-alpha.1
v1.0.0-beta.1
v1.0.0-rc.1
```

## Commit Messages

Use clear, concise commit messages:

```
<type>: <short description>

<optional body with details>
```

Types:
- `feat:` — New feature
- `fix:` — Bug fix
- `test:` — Adding or updating tests
- `docs:` — Documentation changes
- `refactor:` — Code restructuring without behavior change
- `build:` — Build system or CI changes
- `security:` — Security-related changes

Examples:
```
feat: add AES-256-XTS encryption wrapper
fix: correct tweak calculation for block numbers > 2^32
test: add NIST SP 800-38E test vectors
security: scrub key material on error paths
```

## CI/CD

Automated builds and tests run via Gitea Actions on every push.

- **Push to `dev`**: Build and run all tests
- **Push to `main`**: Build, test, and prepare release artifacts
- **Tag push (`v*`)**: Full release build with packaging

See `.gitea/workflows/build-test.yml` for the workflow definition.
