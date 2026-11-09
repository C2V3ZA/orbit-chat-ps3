#!/usr/bin/env python3
"""Orbit Vermittler (relay) - lets PS3 consoles meet without any router configuration.

Both consoles connect *out* to this server. The group host keeps a control connection open;
a friend asks for a group tag, the host opens a data connection and the relay splices the two
TCP streams. Everything inside is TLS between the consoles: the relay cannot read messages
or voice. Python 3.8+, standard library only.

Protocol (one ASCII line, then raw bytes):
  ORBIT-RELAY 1 HOST <server-id 16 hex> <secret 64 hex>   -> OK | ERR text
      host -> relay: TAGS <tag> ...   PONG          relay -> host: CONN <id>   PING
  ORBIT-RELAY 1 JOIN <group-tag 16 hex>                     -> OK (then spliced) | OFFLINE
  ORBIT-RELAY 1 ACCEPT <id>                                 -> (spliced)
"""
import argparse, asyncio, hashlib, ipaddress, json, logging, os, re, secrets, time

HEX16 = re.compile(r'^[0-9a-f]{16}$')
HEX64 = re.compile(r'^[0-9a-f]{64}$')
MAX_HOSTS, MAX_PIPES, MAX_PER_IP = 1000, 4000, 64
log = logging.getLogger('orbit-relay')


class Relay:
    def __init__(self, state_file):
        self.state_file = state_file
        self.owners = {}      # server id -> sha256(secret): first come, first served, persistent
        self.hosts = {}       # server id -> Host
        self.tags = {}        # group tag -> server id
        self.pending = {}     # connection id -> future(reader, writer)
        self.per_ip = {}
        self.pipes = 0
        try:
            with open(state_file) as f:
                self.owners = json.load(f)
        except (OSError, ValueError):
            pass

    def save(self):
        if not self.state_file:
            return
        tmp = self.state_file + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.owners, f)
        os.replace(tmp, self.state_file)

    async def handle(self, reader, writer, tunnel=False):
        ip = 'tunnel' if tunnel else (writer.get_extra_info('peername') or ('?',))[0]
        # Tunnels (bore, playit) deliver every friend from one address: never limit those.
        try:
            local = not tunnel and (ipaddress.ip_address(ip).is_private or
                                    ipaddress.ip_address(ip).is_loopback)
        except ValueError:
            local = False
        if not local and not tunnel and self.per_ip.get(ip, 0) >= MAX_PER_IP:
            log.warning('too many connections from %s', ip)
            writer.close()
            return
        self.per_ip[ip] = self.per_ip.get(ip, 0) + 1
        try:
            line = await asyncio.wait_for(reader.readline(), 10)
            parts = line.decode('ascii', 'replace').split()
            if len(parts) < 4 or parts[0] != 'ORBIT-RELAY' or parts[1] != '1':
                return
            if parts[2] == 'HOST' and len(parts) == 5:
                await self.host(parts[3], parts[4], reader, writer, ip)
            elif parts[2] == 'STATUS' and local:
                status = {'hosts': {k: {'tags': len(v.tags), 'online_s': int(time.time() - v.since)}
                                    for k, v in self.hosts.items()},
                          'pipes': self.pipes, 'pending': len(self.pending),
                          'per_ip': dict(self.per_ip)}
                writer.write((json.dumps(status) + '\n').encode())
                await writer.drain()
            elif parts[2] == 'JOIN':
                await self.join(parts[3], reader, writer)
            elif parts[2] == 'ACCEPT':
                entry = self.pending.pop(parts[3], None)
                if entry and not entry[0].done():
                    entry[0].set_result((reader, writer))
                    await entry[1]  # keep this handler alive while spliced
        except (asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            self.per_ip[ip] -= 1
            if not self.per_ip[ip]:
                del self.per_ip[ip]
            if not writer.is_closing():
                writer.close()

    async def host(self, sid, secret, reader, writer, ip):
        if not HEX16.match(sid) or not HEX64.match(secret):
            return
        digest = hashlib.sha256(bytes.fromhex(secret)).hexdigest()
        if self.owners.get(sid, digest) != digest:
            writer.write(b'ERR Diese Treffpunkt-ID gehoert einer anderen PS3.\n')
            await writer.drain()
            return
        if sid not in self.owners:
            if len(self.owners) > 100000:
                return
            self.owners[sid] = digest
            self.save()
        if len(self.hosts) >= MAX_HOSTS and sid not in self.hosts:
            writer.write(b'ERR Vermittler voll.\n')
            await writer.drain()
            return
        old = self.hosts.get(sid)
        if old:
            old.close()  # same console reconnected: the new connection wins
        h = Host(sid, writer)
        self.hosts[sid] = h
        writer.write(b'OK\n')
        await writer.drain()
        log.info('host %s online from %s', sid, ip)
        ping = asyncio.create_task(h.pinger())
        try:
            while True:
                line = await asyncio.wait_for(reader.readline(), 75)
                if not line:
                    break
                parts = line.decode('ascii', 'replace').split()
                if parts and parts[0] == 'TAGS':
                    tags = [t for t in parts[1:65] if HEX16.match(t)]
                    for t in list(h.tags):
                        if self.tags.get(t) == sid:
                            del self.tags[t]
                    h.tags = set()
                    for t in tags:
                        owner = self.tags.get(t)
                        if owner is None or owner not in self.hosts or owner == sid:
                            self.tags[t] = sid
                            h.tags.add(t)
        finally:
            ping.cancel()
            if self.hosts.get(sid) is h:
                del self.hosts[sid]
                for t in h.tags:
                    if self.tags.get(t) == sid:
                        del self.tags[t]
                log.info('host %s offline', sid)

    async def join(self, tag, reader, writer):
        sid = self.tags.get(tag) if HEX16.match(tag) else None
        h = self.hosts.get(sid) if sid else None
        if not h or self.pipes >= MAX_PIPES:
            log.info('join %s: no host online', tag[:6])
            writer.write(b'OFFLINE\n')
            await writer.drain()
            return
        cid = secrets.token_hex(12)
        loop = asyncio.get_running_loop()
        fut, done = loop.create_future(), loop.create_future()
        self.pending[cid] = (fut, done)
        try:
            h.send(f'CONN {cid}\n'.encode())
            r2, w2 = await asyncio.wait_for(fut, 10)
        except (asyncio.TimeoutError, ConnectionError, OSError):
            self.pending.pop(cid, None)
            done.set_result(True)
            log.warning('join %s: host %s did not accept within 10 s', tag[:6], sid)
            writer.write(b'OFFLINE\n')
            await writer.drain()
            return
        writer.write(b'OK\n')
        await writer.drain()
        self.pipes += 1
        log.info('join %s -> host %s connected (%d active)', tag[:6], sid, self.pipes)
        try:
            await asyncio.gather(pump(reader, w2), pump(r2, writer), return_exceptions=True)
        finally:
            self.pipes -= 1
            w2.close()
            if not done.done():
                done.set_result(True)


class Host:
    def __init__(self, sid, writer):
        self.sid, self.writer, self.tags, self.since = sid, writer, set(), time.time()

    def send(self, data):
        self.writer.write(data)

    def close(self):
        self.writer.close()

    async def pinger(self):
        while True:
            await asyncio.sleep(25)
            self.send(b'PING\n')
            await self.writer.drain()


async def pump(reader, writer, idle=600):
    try:
        while True:
            data = await asyncio.wait_for(reader.read(16384), idle)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    finally:
        if not writer.is_closing():
            writer.close()


# ---- built-in bore client (protocol of github.com/ekzhang/bore, MIT) ----
# Makes this relay reachable from the internet through a public bore server, without any router
# settings and without a separate bore.exe. Messages are JSON objects terminated by a 0 byte.
BORE_CONTROL_PORT = 7835


async def bore_recv(reader, timeout):
    data = await asyncio.wait_for(reader.readuntil(b'\0'), timeout)
    return json.loads(data[:-1].decode())


def bore_send(writer, message):
    writer.write(json.dumps(message).encode() + b'\0')


def bore_endpoint(server):
    host, _, port = server.partition(':')
    return host, int(port) if port else BORE_CONTROL_PORT


async def bore_accept(relay, server, cid):
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(*bore_endpoint(server)), 10)
        bore_send(writer, {'Accept': cid})
        await writer.drain()
        await relay.handle(reader, writer, tunnel=True)
    except (OSError, asyncio.TimeoutError) as e:
        log.warning('Tunnel: Verbindung konnte nicht angenommen werden: %s', e)


async def bore_tunnel(relay, server, want_port, state):
    delay = 2
    while True:
        writer = None
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(*bore_endpoint(server)), 15)
            bore_send(writer, {'Hello': want_port})
            await writer.drain()
            while True:
                msg = await bore_recv(reader, 15)
                if msg == 'Heartbeat':
                    continue
                break
            if isinstance(msg, dict) and 'Error' in msg:
                log.warning('Tunnel: %s meldet: %s', server, msg['Error'])
                if want_port and 'port' in str(msg['Error']).lower():
                    log.warning('Port %d ist bei %s belegt - nehme einen freien Port.', want_port, server)
                    want_port = 0
                raise ConnectionError(msg['Error'])
            port = msg['Hello']
            host = bore_endpoint(server)[0]
            state['address'] = f'{host}:{port}'
            log.warning('=' * 58)
            log.warning('  Internet-Adresse fuer Orbit:  %s:%d', host, port)
            log.warning('=' * 58)
            delay = 2
            while True:
                msg = await bore_recv(reader, 60)  # the server sends heartbeats regularly
                if isinstance(msg, dict) and 'Connection' in msg:
                    asyncio.create_task(bore_accept(relay, server, msg['Connection']))
        except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError,
                ConnectionError) as e:
            state['address'] = None
            log.warning('Tunnel unterbrochen (%s). Neuer Versuch in %d s ...', e or type(e).__name__, delay)
        finally:
            if writer:
                writer.close()
        await asyncio.sleep(delay)
        delay = min(delay * 2, 30)


class Discovery(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if data == b'ORBIT-RELAY?':
            self.transport.sendto(b'ORBIT-RELAY!', addr)


async def main():
    ap = argparse.ArgumentParser(description='Orbit PS3 Vermittler')
    ap.add_argument('--port', type=int, default=31400)
    ap.add_argument('--host', default='0.0.0.0', help='127.0.0.1 = only reachable through a local tunnel such as playit.gg')
    ap.add_argument('--state', default='orbit-relay-owners.json')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--log', help='also write the log to this file')
    ap.add_argument('--bore', metavar='SERVER',
                    help='reach this relay from the internet through a bore server, e.g. bore.pub')
    ap.add_argument('--bore-port', type=int, default=31400,
                    help='public port to ask the bore server for (0 = any free port)')
    args = ap.parse_args()
    handlers = [logging.StreamHandler()]
    if args.log:
        handlers.append(logging.FileHandler(args.log, encoding='utf-8'))
    logging.basicConfig(level=logging.WARNING if args.quiet else logging.INFO,
                        format='%(asctime)s %(message)s', handlers=handlers)
    relay = Relay(args.state)
    server = await asyncio.start_server(relay.handle, args.host, args.port)
    if args.host != '127.0.0.1':
        # A PS3 in the same home network finds this relay directly (no detour via a tunnel).
        try:
            await asyncio.get_running_loop().create_datagram_endpoint(
                Discovery, local_addr=('0.0.0.0', args.port), allow_broadcast=True)
        except OSError as e:
            log.warning('Heimnetz-Suche nicht verfuegbar: %s', e)
    log.warning('Orbit Vermittler bereit auf Port %d', args.port)
    print('ready', flush=True)
    if args.bore:
        asyncio.create_task(bore_tunnel(relay, args.bore, args.bore_port, {}))
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
