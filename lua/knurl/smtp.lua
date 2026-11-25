-- MIT License, adapted from Vectis. Copyright (c) 2026 C89 Systems AB.
local curl = require('curl')
local smtp = {}

function smtp.send(options)
  assert(type(options) == 'table', 'knurl.smtp.send requires an options table')
  local scheme = type(options.url) == 'string' and options.url:match('^(%a+)://')
  scheme = scheme and scheme:lower()
  assert(scheme == 'smtp' or scheme == 'smtps', 'knurl.smtp.send requires an smtp:// or smtps:// URL')

  local transfer = {}
  for key, value in pairs(options) do transfer[key] = value end
  local envelope = options.smtp
  if envelope == nil then envelope = {} end
  assert(type(envelope) == 'table', 'knurl.smtp.send requires smtp to be a table')
  for key in pairs(envelope) do
    assert(key == 'mail_from' or key == 'rcpt' or key == 'use_ssl', 'unknown SMTP envelope field')
  end
  local from = envelope.mail_from or options.mail_from or options.from
  local rcpt = envelope.rcpt or options.rcpt or options.recipients or options.to
  assert(type(from) == 'string' and from ~= '', 'knurl.smtp.send requires mail_from or from')
  if type(rcpt) == 'string' then rcpt = {rcpt} end
  assert(type(rcpt) == 'table' and #rcpt > 0, 'knurl.smtp.send requires rcpt, recipients, or to')
  transfer.smtp = {mail_from = from, rcpt = rcpt, use_ssl = envelope.use_ssl}
  transfer.from, transfer.mail_from, transfer.rcpt, transfer.recipients, transfer.to = nil, nil, nil, nil, nil

  if transfer.body == nil then transfer.body = transfer.message end
  transfer.message = nil
  assert(transfer.body ~= nil or transfer.body_path ~= nil or transfer.upload_path ~= nil or transfer.read ~= nil,
    'knurl.smtp.send requires body, message, body_path, upload_path, or read')
  if transfer.protocols == nil then transfer.protocols = 'smtp,smtps' end

  local result = curl.perform(transfer)
  result.transport_ok = result.ok
  if not result.ok then
    result.error_message = result.error
    result.error = {
      kind = result.code == 28 and 'timeout' or 'transport',
      source = 'curl',
      message = result.error,
      code = result.code,
      code_name = result.code_name,
      dependency_code = result.code,
    }
  end
  return result
end

return smtp
