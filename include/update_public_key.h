#pragma once
// Public key that signs Perch updates (ECDSA P-256). The private key stays on the
// release PC in keys/ (never committed). An update whose signature does not verify is refused.
static const char UPDATE_PUBLIC_KEY[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEstGGCg2tyiMIjMfZ3YEunGoExuHm\n"
  "Up/lm7fDBnBxc2cRcYYQzrOSSOI+gGPqGDvQxkuFRknyFs85kHbCSZ2zxA==\n"
  "-----END PUBLIC KEY-----\n";
