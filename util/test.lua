--[[
Copyright 2012 Rackspace

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS-IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--]]

local net = require('net')
local timer = require('timer')
local http = require('http')

local LineEmitter = require('line-emitter').LineEmitter

-- Simple test TCP servers which responds to commands with a pre-defined
-- response.
function runTestTCPServer(port, host, commandMap, callback)
  local server

  server = net.createServer(function(client)
    local lineEmitter = LineEmitter:new()
    client:pipe(lineEmitter)

    lineEmitter:on('data', function(data)
      if type(commandMap[data]) == 'string' then
        client:write(commandMap[data])
        timer.setTimeout(2000, function()
          client:destroy()
        end)
      elseif type(commandMap[data]) == 'function' then
        client:write(commandMap[data]())
        timer.setTimeout(2000, function()
          client:destroy()
        end)
      else
        print('nothing')
        client:destroy()
      end
    end)
  end)

  server:listen(port, host, function(err)
    callback(err, server)
  end)
end

-- Simple test HTTP Server
function runTestHTTPServer(port, host, reqCallback, callback)
  local server
  server = http.createServer(function(req, res)
    reqCallback(req, res)
  end)
  server:listen(port, host, function(err)
    callback(err, server)
  end)
end

local exports = {}
exports.runTestTCPServer = runTestTCPServer
exports.runTestHTTPServer = runTestHTTPServer
return exports