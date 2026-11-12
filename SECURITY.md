# Security Policy

Please report vulnerabilities privately to the repository owner rather than
opening a public issue with exploit details.

This project intentionally handles guest pointers and native code execution.
Treat libraries and guest binaries as untrusted unless they have been audited.

Do not commit credentials, private network addresses, machine-specific paths or
tokens. Put local configuration in `.cache/config.sh`, which is ignored by Git.
