# ACME account, order and recovery flow

ACME runs in its auxiliary process through the selected SDK's libcurl. The
key manager owns account/domain private keys, CSR signing, persisted candidates
and certificate distribution. HTTP workers own installed TLS contexts. The
application serving gate does not stop TLS-ALPN-01.

Account recovery uses the existing account key. An `onlyReturnExisting`
request falls back to registration only for the
[`accountDoesNotExist` problem type](https://www.rfc-editor.org/rfc/rfc8555#section-7.3.1).
The optional problem detail is not required for that classification. Other
account failures use a bounded retry interval rather than creating an account
on every HTTP 400. Failed directory discovery is retried; an account request
that arrived before directory discovery remains pending.

A restarted ACME process requests the public account parameters and registration
state from the live key manager. A key-manager reconnect cancels the ACME
process's old order/signing state. There is one outstanding key-manager order
per domain, including domains repeated on several listeners. Installation or
failure releases that order; a reconnect restarts renewal selection.

Before notifying the CA, the backend waits for installation acknowledgements
from every HTTP worker. Each worker acknowledges after updating all matching
listener contexts. Acknowledgements carry the installed challenge certificate;
the ACME process checks the expected authorization digest and deduplicates worker
IDs. This preserves the [TLS-ALPN-01 preparation order](https://www.rfc-editor.org/rfc/rfc8737#section-3)
and rejects stale acknowledgements from a different challenge.

Completed local steps advance without a fixed one-second delay. Remote work that
is still processing retains the polling interval and overall order deadline.
Each order owns its timer. Signing operations distinguish owned payloads from
borrowed order/auth references; cancellation removes associated operations and
their timers before releasing the order.

Parent IPC routing removes disconnected connections before their pool storage
can be reused and reuses empty routing slots. Worker broadcasts target HTTP
workers. Replacing a child must not duplicate signed requests, certificate
messages or application messages through a stale connection pointer.

ACME trust uses the captured file bytes and resolved directory paths. Generic
Kore curl initialization imports environment settings, so the adapter first
resets both components to the SDK's compiled defaults, then applies its captured
policy. Libcurl exposes those defaults through
[`curl_version_info`](https://curl.se/libcurl/c/curl_version_info.html);
the captured PEM uses [`CURLOPT_CAINFO_BLOB`](https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html).
When the SDK delegates both defaults to OpenSSL, its compiled certificate
directory is selected explicitly. Changes made by a child lifecycle hook to
`SSL_CERT_FILE` or `SSL_CERT_DIR` do not replace a running ACME client's policy.
Peer and hostname verification remain enabled.

The [public fixture](../tests/acme-protocol.c) consumes the shared library and
installed-style public headers, with no Kore headers or injected backend
messages. Its [local CA](../tests/acme-ca.c) serves HTTPS, checks one-use nonces,
protected URLs, JWK/KID selection and RSA JWS signatures, verifies CSR signatures,
domain keys and SANs, and validates the live TLS-ALPN authorization digest on both
listeners. It issues a certificate inside the existing renewal window, then a
longer-lived replacement after an ACME-process restart. Both certificates must
pass PERSIST and real HTTPS verification; ordinary HTTP remains paused until
the host opens serving.

The cases also exercise explicit-file, environment-file and explicit-directory
trust precedence/capture, response bounds, transport timeout and unrelated
account errors. They use no external CA. Restart triggers the same renewal
selection callback used by the hourly timer; the fixture does not wait an hour
or claim wall-clock scheduler coverage. Complete allocation/fatal unwinding,
faults during in-flight signing/authorization/finalization, CA rate-limit/retry
handling, multi-SNI challenge combinations and final installed-artifact
integration remain part of the full implementation goal.
