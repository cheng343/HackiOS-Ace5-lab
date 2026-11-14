/* Minimal host declaration matching the SecureDT calls used by our helper. */
#ifndef TEST_DEVICE_TREE_H
#define TEST_DEVICE_TREE_H
typedef const struct OpaqueDTEntry *DTEntry;
enum { kSuccess = 1, kError = -1 };
int SecureDTLookupEntry(DTEntry, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, const void **, unsigned int *);
#endif
