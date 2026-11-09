"""Local stand-in for a public bore server (same protocol as github.com/ekzhang/bore):
control connection with Hello/Heartbeat/Connection, data connections with Accept."""
import asyncio, json, sys, uuid

CONTROL = int(sys.argv[1])
PUBLIC = int(sys.argv[2])
pending = {}


async def recv(reader):
    data = await reader.readuntil(b'\0')
    return json.loads(data[:-1])


def send(writer, msg):
    writer.write(json.dumps(msg).encode() + b'\0')


async def pump(r, w):
    try:
        while True:
            d = await r.read(16384)
            if not d:
                break
            w.write(d)
            await w.drain()
    except (OSError, ConnectionError):
        pass
    finally:
        w.close()


async def control(reader, writer):
    msg = await recv(reader)
    if 'Accept' in msg:
        entry = pending.pop(msg['Accept'], None)
        if entry:
            pr, pw = entry
            await asyncio.gather(pump(reader, pw), pump(pr, writer))
        return
    port = msg['Hello'] or PUBLIC

    async def visitor(vr, vw):
        cid = str(uuid.uuid4())
        pending[cid] = (vr, vw)
        send(writer, {'Connection': cid})
        await writer.drain()

    try:
        server = await asyncio.start_server(visitor, '127.0.0.1', port)
    except OSError:
        send(writer, {'Error': 'port already in use'})
        await writer.drain()
        return
    send(writer, {'Hello': port})
    await writer.drain()
    print('tunnel open on', port, flush=True)
    async def heartbeat():
        while True:
            await asyncio.sleep(0.5)
            send(writer, 'Heartbeat')
            await writer.drain()

    beat = asyncio.ensure_future(heartbeat())
    try:
        await reader.read()  # like bore: the port is released when the control connection ends
    except (OSError, ConnectionError):
        pass
    beat.cancel()
    server.close()
    await server.wait_closed()
    print('tunnel closed on', port, flush=True)


async def main():
    s = await asyncio.start_server(control, '127.0.0.1', CONTROL)
    print('ready', flush=True)
    async with s:
        await s.serve_forever()


asyncio.run(main())
