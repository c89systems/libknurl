# Native route authentication

Knurl can protect a native route or a Lua application route with the same auth
policy. The route ACL runs first. Auth runs on a bounded native pool while the
Kore event loop continues serving other requests. Knurl waits for auth before
it opens route resources, grants `100 Continue`, or admits the body. Protected
routes and the login endpoints require HTTPS.

## Browser sessions

Create a private state directory under the operating-system account that runs
Knurl. The password command reads a password of at least 15 bytes from standard
input and hides interactive input. The TOTP command generates and encrypts the
seed, then prints the enrollment URI once for the user's authenticator. Use
`--rotate` to replace an existing enrollment.

```sh
knurl auth init ./auth-state
knurl auth user-password ./auth-state alice
knurl auth user-totp ./auth-state alice
```

```lua
local knurl = require('knurl')
local server <close> = assert(knurl.server{
  port = 8443,
  tls = {host = 'localhost', certificate = 'cert.pem', private_key = 'key.pem'},
})

local account <close> = assert(knurl.auth{
  name = 'account',
  store = './auth-state',
  login = 'totp',
})

assert(server:login{path = '/login', auth = account})
assert(server:logout{path = '/logout', auth = account})
assert(server:static_directory{path = '/private', root_dir = './private', auth = account})
assert(server:route{
  path = '/account/:page',
  auth = account,
  handler = function(req)
    return {json = {user = req.auth.subject, page = req.params.page}}
  end,
})
assert(server:run())
```

The browser submits `POST /login` as `application/x-www-form-urlencoded`
with `username`, `password` and `code`. Choose `login = 'password'` for a
password-only policy, which takes `username` and `password`. On success, the
endpoint returns HTTP 204 and sets a Secure, HttpOnly, SameSite=Strict cookie.
Forms can stay on the application's HTTPS origin, then navigate to a protected
page after success. A same-origin `POST /logout` revokes the session and clears
its cookie. The default lifetime is eight hours and `session_seconds` overrides
it. Use distinct names for different policies.

To let users enroll themselves, register `server:totp_enroll` with a
bootstrap session policy. That policy can use password or email-code login.
Keep the TOTP-required policy on the protected application routes.

```lua
local bootstrap <close> = assert(knurl.auth{
  name = 'bootstrap', store = './auth-state', login = 'password',
})
assert(server:login{path = '/enroll/login', auth = bootstrap})
assert(server:totp_enroll{path = '/enroll/totp', auth = bootstrap})
```

The TOTP login returns HTTP 428 after password verification if the account
needs enrollment. First sign in under the bootstrap policy, then POST an
empty form to `/enroll/totp` with its cookie. The endpoint returns HTTP 201
with a one-time JSON `uri` field for the authenticator. Use
`application/x-www-form-urlencoded` on the same HTTPS origin. Rotation
requires the current authenticator's unused `code` or the operator CLI.
Enrollment and rotation invalidate existing account sessions. Sign in on the
TOTP-required route to establish a new session.

An application receives `req.auth.subject` and `req.auth.method`. The method
is `session` or `machine`. C handlers use `knurl_request_auth()` or
`knurl_request_snapshot_auth()` from a native reader callback. Lua can also
assign a default policy with `server:group{prefix = '/private', auth = account}`.
`route:set_auth(policy)` or `group:set_auth(policy)` replaces an attachment
before the server runs. An explicit route policy takes precedence over its group.

## Email login

Set the delivery address with `knurl auth user-email ./auth-state alice alice@example.com`.
Use `login = 'email'` and provide `smtp` in the policy.

```lua
local email <close> = assert(knurl.auth{
  name = 'email',
  store = './auth-state',
  login = 'email',
  smtp = {
    url = 'smtps://mail.example.com:465',
    from = 'login@example.com',
    username = 'login@example.com',
    password = assert(os.getenv('SMTP_PASSWORD')),
  },
})
assert(server:email_code{path = '/email/code', auth = email})
assert(server:login{path = '/email/login', auth = email})
assert(server:logout{path = '/email/logout', auth = email})
```

`POST /email/code` takes a form with `username`. It returns HTTP 202 once
queued. The response is the same whether or not the account has a delivery
address. Submit `username` and `code` to `POST /email/login` to redeem it.
A code expires after ten minutes and succeeds once. SMTP requires verified TLS.
Use `smtp://` for mandatory STARTTLS or `smtps://` for implicit TLS. The
optional `ca_file` overrides the system trust bundle and `timeout_ms` sets a
timeout up to ten seconds. SMTP delivery errors go to the configured Knurl
diagnostic logger.

## Ordered factors and machine credential issuance

```lua
local clients <close> = assert(knurl.auth{
  name = 'deployment', store = './auth-state', method = 'machine',
  steps = {'password', 'totp'},
})
assert(server:auth_workflow{path = '/provision', auth = clients})
assert(server:text{path = '/integration/health', auth = clients, body = 'ready'})
```

Select an ordered subset of `email_code`, `password` and `totp`. TOTP
requires a preceding factor. Configure SMTP when using `email_code`.
POST an HTTPS form to the workflow route with `username` and, if password
comes first, `password`. An email-first workflow sends a code to the
account's enrolled address and returns the same HTTP 202 shape for unknown
accounts.

Each HTTP 202 includes an opaque `workflow`, the current `step` and its
`required` field. POST back to the same path with that workflow and the
requested `password` or six-digit `code`. The workflow expires after ten
minutes and each successful continuation replaces its identifier.

For a machine policy, the final HTTP 201 returns `client_id` and
`client_secret` once. Use the pair as HTTP Basic credentials on routes
protected by the named policy. The authenticated subject is the account
that completed the workflow. Account changes invalidate its issued
credentials. An operator can revoke an individual credential with
`knurl auth client-revoke ./auth-state deployment <client_id>`. For a
browser policy, choose `method = 'session'`. The final HTTP 204 sets the
policy's session cookie and the existing logout route revokes it.

## Machine authentication

Issue a client secret with `knurl auth client-issue ./auth-state service deployment`.
This prints the secret once. Set `name = 'service'`, `method = 'machine'`, and
`store = './auth-state'` in the policy and attach it to the machine routes.
Each credential belongs to its named policy, so policies can share the same store.
Clients send HTTP Basic credentials over TLS. Use
`knurl auth client-revoke ./auth-state service deployment` to revoke it, or issue it
again to rotate its secret.

For a single client, you can supply `client_id` and a random `client_secret`
directly in the Lua policy in place of `store`. To accept signed OAuth access
tokens from an external issuer, configure `oidc` in a machine policy.

```lua
local service <close> = assert(knurl.auth{
  name = 'service', method = 'machine',
  oidc = {
    issuer = 'https://identity.example.com',
    audience = 'https://api.example.com',
    scope = 'api.machine',
  },
})
assert(server:text{path = '/integration/health', body = 'ready', auth = service})
```

LoneJSON verifies the signature, issuer, audience, expiry, required scope and
subject. The JOSE type defaults to `at+jwt`. Set `token_type` to the type
your issuer uses for access tokens. Knurl discovers the JWKS endpoint over
HTTPS and refreshes cached keys on expiry or when an unfamiliar key ID
appears. An explicit `jwks_uri` skips discovery. You can seed that cache with
`jwks_json`, or set `pin_jwks = true` to use only the supplied keys.

Responses on authenticated routes use `Cache-Control: private, no-store`.
The native reverse proxy removes Knurl session cookies and drops the consumed
machine `Authorization` header from its default upstream request.

## OAuth2 for a reverse proxy

```lua
local upstream <close> = assert(knurl.oauth2{
  issuer = 'https://identity.example.com',
  client_id = 'gateway',
  client_secret = assert(os.getenv('UPSTREAM_CLIENT_SECRET')),
  scope = 'api.machine',
})
assert(server:proxy{
  path = '/gateway', target = 'https://api.example.com', oauth2 = upstream,
})
```

You can supply `token_endpoint` directly or discover it from `issuer`.
Knurl obtains and caches a Bearer token on a native worker. It renews before
expiry, retains rotating refresh tokens, and obtains new client-credentials
tokens when there is no refresh token or the issuer rejects it as invalid.
The proxy supplies the resulting `Authorization` header automatically. All
upstream targets must use HTTPS. The token endpoint uses
`client_secret_basic` by default and supports `client_secret_post = true`.
Both the inbound OIDC and outbound OAuth2 configuration accept `ca_file`
for a private trust root. A Lua application worker can also call
`upstream:token()` for a separate outbound request. The call may wait for
the issuer and handles renewal internally.

## State and C backends

The built-in store writes a fixed set of files in one private directory.

- `accounts.json` holds users, salted password hashes, encrypted TOTP seeds
  and policy-scoped machine credentials.
- `sessions.json`, `email.json`, `totp.json` and `attempts.json` hold
  state for each flow and appear as they are used. Email and TOTP sessions
  are written to the same file as the factor they consume, in one atomic
  update. Ordered workflows keep pending steps and the final session or
  machine credential in the same file as the terminal factor.
- `key` holds the owner-only encryption key. Back it up with the JSON files.

One interprocess lock protects updates and operations that read multiple files.
Knurl syncs and atomically replaces each changed JSON file. Disabling a user
changes the account revision, which invalidates existing sessions without a
session-file rewrite. TOTP replay checks use the enrollment ID so seed rotation
starts a new counter. Use `knurl auth prune ./auth-state` to remove expired
state. Run the store on a local filesystem that supports advisory locks and
atomic rename.

In C, set `backend` in `knurl_auth_config` to supply storage and
verification operations. For a directory or identity provider, set `users`
to supply password verification and enrolled-email lookup while the state
backend handles sessions, TOTP replay protection and email challenges. A
provider verifies one named user directly and returns their canonical subject.
The interface never requires account enumeration or retrieving password hashes
from the directory. Configure `send_email` to replace the native SMTP sender
with a C callback.

Backend callbacks can run concurrently in multiple threads and worker
processes. State operations must be atomic across them. Set `session_check`
in the C policy to revalidate the account after a valid session lookup.
`KNURL_OK` admits the request, `KNURL_EOF` denies it with HTTP 401, and a
provider failure returns HTTP 503. The callback runs after the store releases
its lock. The public `<knurl/auth.h>` header defines the callback contract and
operation inputs.
