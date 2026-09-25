# Third-party boundary

No build target downloads third-party source. Operators must acquire, verify,
and record immutable source archives separately before enabling production
adapters.

| Component | Required version | Boundary |
| --- | --- | --- |
| FastCGI | 2.4.7 | Optional FastCGI adapter, `make fastcgi` only |
| Jansson | 2.14 | JSON parsing/serialization boundary; host tests accept a compatible system library |
| OpenSSL | 3.5.8 | Production crypto boundary; host tests accept a compatible system library |
| nginx | 1.31.4 | HTTPS reverse proxy; source-build profile in `../config/` |

Record source URL, SHA-256, signature result, and local build flags in the
deployment evidence. Do not vendor unverified archives into this repository.
