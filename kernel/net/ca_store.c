#include "x509.h"

/* Built-in trust anchors (DER). Filled in by tools/net-test/gen_ca_store.py. */
const x509_der_t ca_store_builtin[1] = { { 0, 0 } };
const int ca_store_builtin_count = 0;
