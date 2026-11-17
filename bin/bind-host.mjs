// Preload for bridge/server.mjs: bind every numeric-port server.listen() to
// $BIND_HOST (default 127.0.0.1) instead of the hard-coded 0.0.0.0.
import net from 'node:net';
const host = process.env.BIND_HOST || '127.0.0.1';
const orig = net.Server.prototype.listen;
net.Server.prototype.listen = function (...args) {
  if (typeof args[0] === 'number') args[1] = host;
  return orig.apply(this, args);
};
