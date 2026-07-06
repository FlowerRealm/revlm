# Security

## Reporting

Open a [GitHub Security Advisory](https://github.com/FlowerRealm/revlm/security/advisories/new) or a private issue if you find a vulnerability.

## Deployment requirements

Production deployments must set strong, unique values for at least:

- `SESSION_SECRET`
- `REVLM_DB_DSN` credentials
- `REVLM_ADMIN_API_KEY` (if used)
- `REVLM_REDIS_PASSWORD` (if Redis is enabled)

Do not reuse example values from `README.md` or Helm samples.

## Known design choices

- **Upstream API keys** (`upstream_channels.api_key`) and **token reveal** (`user_tokens.token_plain`) are stored in plaintext in MySQL. Restrict database access and encrypt backups.
- **`tmp-devseed`** creates fixed local-only accounts (`root@local`, `user@local`) with weak passwords (`dev-only-root`, `dev-only-user` in source). Never run it against production databases or expose the seeded instance to the network.

## Local development seed

`build/backend/tmp-devseed` is for local integration only. See `README.md` → Maintenance CLI.
