# Native route authentication

Knurl can protect a native route or a Lua application route with the same auth
policy. The route ACL runs first. Auth runs on a bounded native pool while the
Kore event loop continues serving other requests. Knurl waits for auth before
it opens route resources, grants `100 Continue`, or admits the body. Protected
routes and the login endpoints require HTTPS.

## Browser sessions

Create a private state directory under the operating-system account that runs
Knurl. The password and TOTP commands read the credential from standard input
and hide input when run interactively. Passwords must contain at least 15
bytes. For TOTP, supply a base32 seed of 16 to 32 bytes and enroll the same
seed in the user's authenticator.

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

## Machine authentication

Issue a client secret with `knurl auth client-issue ./auth-state deployment`.
This prints the secret once. Set `method = 'machine'` and
`store = './auth-state'` in a policy and attach it to the machine routes.
Clients send HTTP Basic credentials over TLS. Use
`knurl auth client-revoke ./auth-state deployment` to revoke it, or issue it
again to rotate its secret.

For a single client, you can supply `client_id` and a random `client_secret`
directly in the Lua policy in place of `store`. To accept signed OAuth access
tokens from an external issuer, configure `oidc` in a machine policy.

```lua
local jwks <close> = assert(io.open('./issuer-jwks.json', 'rb'))
local service <close> = assert(knurl.auth{
  name = 'service', method = 'machine',
  oidc = {
    issuer = 'https://identity.example.com',
    audience = 'https://api.example.com',
    jwks_uri = 'https://identity.example.com/jwks',
    jwks_json = assert(jwks:read('*a')),
    scope = 'api.machine',
  },
})
assert(server:text{path = '/integration/health', body = 'ready', auth = service})
```

LoneJSON verifies the signature, issuer, audience, expiry, required scope and
subject. The JOSE type defaults to `at+jwt`. Set `token_type` to the type
your issuer uses for access tokens. The JWKS are pinned at configuration time.
Reload the application with the new set when the issuer rotates keys. The
issuer handles the client-credentials token exchange.

Responses on authenticated routes use `Cache-Control: private, no-store`.
The native reverse proxy removes Knurl session cookies and drops the consumed
machine `Authorization` header from its default upstream request.

## State and C backends

The built-in store uses private JSON records and a separate owner-only key
file. Back up both together. It stores salted password hashes, encrypted TOTP
seeds, the last accepted TOTP step per enrollment, and digests for active
sessions, email codes and client credentials. Changes to a managed account
invalidate its existing sessions. Use `user-disable` and `user-enable` to
manage access and `knurl auth prune ./auth-state` to remove expired state.
Run the store on a local filesystem that supports advisory locks and atomic
rename.

In C, set `backend` in `knurl_auth_config` to supply storage and
verification operations. For a directory or identity provider, set `users`
to supply password verification and enrolled-email lookup while the state
backend handles sessions, TOTP replay protection and email challenges. A
provider verifies one named user directly and returns their canonical subject.
The interface never requires account enumeration or retrieving password hashes
from the directory. Configure `send_email` to replace the native SMTP sender
with a C callback.

Backend callbacks can run concurrently in multiple threads and worker
processes. State operations must be atomic across them. A backend that mixes
external identity verification with a separate session store can enforce
external account revocation in its session lookup operation. The public
`<knurl/auth.h>` header defines the callback contract and operation inputs.
