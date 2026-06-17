-- TechEmpower-equivalent plaintext benchmark (pipeline depth = 16).
--
-- Reproduces the TechEmpower plaintext test methodology: 512 connections, each
-- sending 16 pipelined GET /plaintext requests per round. Run with:
--
--   wrk -s bench/techempower_plaintext_pipeline16.lua -c 512 -t <threads> -d 15 \
--       http://127.0.0.1:3984/plaintext
--
-- Note: the official TechEmpower project was archived in March 2026 (Round 23
-- was the final round). This script reproduces their plaintext pipeline=16
-- methodology for local, self-reported comparison only; it is NOT an official
-- TechEmpower result.
wrk.method = "GET"
wrk.headers["Accept"] = "text/plain"

local depth = 16

init = function(args)
  local r = {}
  for i = 1, depth do
    r[i] = wrk.format(nil, "/plaintext")
  end
  req = table.concat(r)
end

request = function()
  return req
end
