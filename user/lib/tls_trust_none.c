/* Linked when no CA bundle was built: tls_open reports TLS_NO_TRUST. */
#include "tls_trust.h"

const br_x509_trust_anchor tls_trust_anchors[1] = {{{NULL, 0}, 0, {0, {{NULL, 0, NULL, 0}}}}};
const size_t tls_trust_anchor_count = 0;
